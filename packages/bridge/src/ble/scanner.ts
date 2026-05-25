/**
 * BLE central — scans for "doudou-*" peripherals advertising our custom
 * service UUID, connects, discovers TX/RX characteristics, and hands
 * the connection off to BleDeviceConnection.
 *
 * Optional runtime dep: `@abandonware/noble`. We dynamic-import so the
 * rest of the bridge builds + runs without it; only enable via
 * `DOUDOU_BLE=1` once the user has actually `pnpm add`-ed noble.
 *
 * Hardware coverage is mandatory to validate this — there's no useful
 * test you can run without a real ESP32-C3 broadcasting.
 */
import { log } from '../log.js';
import type { DeviceRegistry } from '../deviceRegistry.js';
import { BleDeviceConnection, type BleChar, type BlePeripheral } from './connection.js';
import type { Reply } from '@doudou/device-protocol';
import type { DeviceState, QuestionPayload } from '../deviceRegistry.js';

/** Must match the UUIDs in `packages/firmware/main/ble_transport.c`. */
const SVC_UUID = 'dd00dddd00001000800000805f9b34fb';
const TX_UUID  = 'dd01dddd00001000800000805f9b34fb';
const RX_UUID  = 'dd02dddd00001000800000805f9b34fb';

export interface BleScannerOptions {
  registry: DeviceRegistry;
  pairingToken: string;
  /** Reply hook — mirrors the WS path. */
  onReply: (
    state: DeviceState,
    reply: Reply,
    payload: QuestionPayload,
    latencyMs: number,
  ) => Promise<void> | void;
  /** Optional name prefix filter. Default "doudou-". */
  namePrefix?: string;
  onFollowThread?: (threadId: string, state: DeviceState) => Promise<void> | void;
}

export interface BleScannerHandle {
  stop(): Promise<void>;
}

/* Loosely-typed noble surface — keeps tsc happy without @types/noble. */
interface NobleCharacteristic {
  uuid: string;
  subscribe(cb: (err?: Error | null) => void): void;
  on(evt: 'data', cb: (data: Buffer, isNotification: boolean) => void): void;
  writeAsync(data: Buffer, withoutResponse: boolean): Promise<void>;
}
interface NoblePeripheral {
  id: string;
  state: string;
  mtu: number | undefined;
  advertisement: { localName?: string };
  connectAsync(): Promise<void>;
  disconnectAsync(): Promise<void>;
  discoverSomeServicesAndCharacteristicsAsync(
    serviceUUIDs: string[],
    characteristicUUIDs: string[],
  ): Promise<{ characteristics: NobleCharacteristic[] }>;
  once(evt: 'disconnect', cb: () => void): void;
}
interface NobleApi {
  on(evt: 'stateChange', cb: (state: string) => void): void;
  on(evt: 'discover',    cb: (p: NoblePeripheral) => void): void;
  startScanningAsync(uuids?: string[], allowDuplicates?: boolean): Promise<void>;
  stopScanningAsync(): Promise<void>;
}

async function loadNoble(): Promise<NobleApi | null> {
  try {
    // Dynamic import to avoid hard dep at build time.
    // @ts-expect-error optional runtime dep — see comment at top of file.
    const mod = (await import('@abandonware/noble')) as
      | { default?: NobleApi }
      | NobleApi;
    return ('default' in mod && mod.default ? mod.default : (mod as NobleApi)) ?? null;
  } catch (err) {
    log.warn({ err: String(err) }, 'ble: failed to load @abandonware/noble — install it with `pnpm --filter @doudou/bridge add @abandonware/noble` to enable BLE');
    return null;
  }
}

/** Wrap a noble peripheral + its characteristics into the small surface
 *  BleDeviceConnection expects. */
function wrapPeripheral(p: NoblePeripheral, tx: NobleCharacteristic, rx: NobleCharacteristic): {
  peripheral: BlePeripheral;
  txChar: BleChar;
  rxChar: BleChar;
} {
  let disconnectCb: ((reason?: string) => void) | null = null;
  p.once('disconnect', () => disconnectCb?.('peripheral disconnect'));

  return {
    peripheral: {
      id: p.id,
      advertisedName: p.advertisement?.localName,
      disconnect: () => p.disconnectAsync(),
      mtu: () => (typeof p.mtu === 'number' && p.mtu > 23 ? p.mtu : 23),
      onDisconnect: (cb) => { disconnectCb = cb; },
    },
    txChar: {
      subscribe: (cb) =>
        new Promise<void>((resolve, reject) => {
          tx.on('data', (data) => cb(data));
          tx.subscribe((err) => (err ? reject(err) : resolve()));
        }),
      write: () => Promise.reject(new Error('tx is notify-only')),
    },
    rxChar: {
      subscribe: () => Promise.reject(new Error('rx is write-only')),
      write: (data, withoutResponse) => rx.writeAsync(data, withoutResponse),
    },
  };
}

export async function startBleScanner(opts: BleScannerOptions): Promise<BleScannerHandle | null> {
  const noble = await loadNoble();
  if (!noble) return null;

  const namePrefix = opts.namePrefix ?? 'doudou-';
  let stopped = false;
  const handled = new Set<string>();

  noble.on('stateChange', (state) => {
    log.info({ state }, 'ble adapter state');
    if (state === 'poweredOn') {
      void noble.startScanningAsync([SVC_UUID], /*allowDuplicates=*/ false)
        .catch((err: unknown) => log.warn({ err: String(err) }, 'ble: scanning failed to start'));
    } else {
      void noble.stopScanningAsync().catch(() => {/* ignore */});
    }
  });

  noble.on('discover', (peripheral) => {
    if (stopped) return;
    const name = peripheral.advertisement?.localName ?? '';
    if (!name.startsWith(namePrefix)) return;
    if (handled.has(peripheral.id)) return;
    handled.add(peripheral.id);

    log.info({ peripheral: peripheral.id, name }, 'ble: discovered doudou, connecting');
    void (async () => {
      try {
        await peripheral.connectAsync();
        const { characteristics } =
          await peripheral.discoverSomeServicesAndCharacteristicsAsync(
            [SVC_UUID],
            [TX_UUID, RX_UUID],
          );
        const tx = characteristics.find((c) => c.uuid.toLowerCase() === TX_UUID);
        const rx = characteristics.find((c) => c.uuid.toLowerCase() === RX_UUID);
        if (!tx || !rx) {
          log.warn({ peripheral: peripheral.id }, 'ble: missing tx/rx chars');
          await peripheral.disconnectAsync();
          handled.delete(peripheral.id);
          return;
        }
        const wrapped = wrapPeripheral(peripheral, tx, rx);
        new BleDeviceConnection(wrapped.peripheral, wrapped.txChar, wrapped.rxChar, {
          registry: opts.registry,
          pairingToken: opts.pairingToken,
          onReply: opts.onReply,
          onFollowThread: opts.onFollowThread,
        });
      } catch (err) {
        log.warn({ err: String(err), peripheral: peripheral.id }, 'ble: connect/discover failed');
        handled.delete(peripheral.id);
      }
    })();
  });

  log.info('ble scanner started');
  return {
    async stop() {
      stopped = true;
      await noble.stopScanningAsync().catch(() => {/* ignore */});
    },
  };
}
