/**
 * BleDeviceConnection integration test — mocks the noble surface
 * (BleChar / BlePeripheral) and walks the handshake + a question/reply
 * round trip end-to-end without touching real hardware.
 *
 * Pairs with the WS-side `deviceRegistry.test.ts` to cover the
 * transport-independent state machine.
 */
import { describe, expect, it, vi } from 'vitest';

import { DeviceRegistry, type QuestionPayload } from '../deviceRegistry.js';
import { encodeMessage, Reassembler } from './framing.js';
import { BleDeviceConnection, type BleChar, type BlePeripheral } from './connection.js';

/** Drain all queued microtasks so fire-and-forget `void Promise.resolve(...).catch(...)`
 *  side effects (BleDeviceConnection.writeChunks, etc.) have a chance to run
 *  before assertions. */
const flush = () => new Promise<void>((r) => setImmediate(r));

/** Mock peripheral that swaps the BLE link for in-process buffers. */
function makeMockLink() {
  let txCb: ((data: Buffer) => void) | null = null;
  let onDisc: ((reason?: string) => void) | null = null;
  const rxWrites: Buffer[] = [];
  const peripheral: BlePeripheral = {
    id: 'test-peripheral',
    advertisedName: 'doudou-AA',
    disconnect: vi.fn(),
    mtu: () => 247,
    onDisconnect: (cb) => { onDisc = cb; },
  };
  const txChar: BleChar = {
    subscribe: (cb) => { txCb = cb; },
    write: () => Promise.reject(new Error('tx is notify-only')),
  };
  const rxChar: BleChar = {
    subscribe: () => Promise.reject(new Error('rx is write-only')),
    write: (data) => { rxWrites.push(Buffer.from(data)); return Promise.resolve(); },
  };
  return {
    peripheral,
    txChar,
    rxChar,
    /** Simulate the device sending a JSON message via TX notify. */
    deviceSends: (json: string) => {
      if (!txCb) throw new Error('connection not yet subscribed to tx');
      // Single-chunk encoding is enough for our small payloads here.
      for (const chunk of encodeMessage(json, 247, 0)) txCb(chunk);
    },
    /** Reassemble everything bridge has written to RX into one or more JSON strings. */
    rxAsJsons: () => {
      const r = new Reassembler();
      const out: string[] = [];
      for (const c of rxWrites) {
        const got = r.feed(c);
        if (got !== null) out.push(got);
      }
      return out;
    },
    triggerDisconnect: () => onDisc?.('test'),
    rxWrites,
  };
}

describe('BleDeviceConnection', () => {
  it('completes hello handshake, sends welcome, then routes a question', async () => {
    const link = makeMockLink();
    const registry = new DeviceRegistry();
    const onReady = vi.fn();

    new BleDeviceConnection(link.peripheral, link.txChar, link.rxChar, {
      registry,
      pairingToken: 'tok-secret',
      onReply: async () => undefined,
      onReady,
    });
    await flush();

    // 1. Device sends hello with the right token.
    link.deviceSends(JSON.stringify({
      v: 1, type: 'hello', seq: 1, ts: 0,
      device_id: 'doudou-test',
      fw_version: '0.1.0',
      pairing_token: 'tok-secret',
    }));
    await flush();

    // 2. Bridge writes welcome back (single JSON envelope).
    const outbound = link.rxAsJsons();
    expect(outbound).toHaveLength(1);
    const welcome = JSON.parse(outbound[0]!);
    expect(welcome.type).toBe('welcome');
    expect(welcome.session_id).toMatch(/^sess_/);
    expect(welcome.max_question_choices).toBe(4);

    // 3. State was created & attached.
    expect(onReady).toHaveBeenCalledTimes(1);
    const state = registry.getOrCreate('doudou-test');
    expect(state.isConnected).toBe(true);

    // 4. Push a question — bridge serializes + writes via RX.
    const q: QuestionPayload = {
      id: 'q-1',
      risk: 'low',
      action_type: 'run_command',
      title: 'demo',
      choices: [{ id: 'allow', label: '允许' }, { id: 'deny', label: '拒绝' }],
      expires_in_ms: 60_000,
      require_confirm: false,
    };
    const pushPromise = state.pushQuestion(q);
    await flush();
    const afterPush = link.rxAsJsons();
    expect(afterPush.length).toBeGreaterThanOrEqual(2);
    const questionJson = JSON.parse(afterPush[afterPush.length - 1]!);
    expect(questionJson.type).toBe('question');
    expect(questionJson.id).toBe('q-1');
    expect(questionJson.risk).toBe('low');

    // 5. Device replies → push resolves with the reply.
    link.deviceSends(JSON.stringify({
      v: 1, type: 'reply', seq: 2, ts: 1,
      id: 'q-1', device_id: 'doudou-test', choice_id: 'allow',
    }));
    const audited = await pushPromise;
    expect(audited.reply.choice_id).toBe('allow');
  });

  it('rejects a hello with a mismatched pairing token', async () => {
    const link = makeMockLink();
    const registry = new DeviceRegistry();
    new BleDeviceConnection(link.peripheral, link.txChar, link.rxChar, {
      registry,
      pairingToken: 'right-tok',
      onReply: async () => undefined,
    });
    await flush();

    link.deviceSends(JSON.stringify({
      v: 1, type: 'hello', seq: 1, ts: 0,
      device_id: 'evil-device', fw_version: 'x', pairing_token: 'wrong-tok',
    }));
    await flush();

    // Bridge should disconnect; no welcome sent.
    expect(link.peripheral.disconnect).toHaveBeenCalled();
    expect(link.rxAsJsons()).toHaveLength(0);
  });

  it('routes follow_thread to the registered callback', async () => {
    const link = makeMockLink();
    const registry = new DeviceRegistry();
    const onFollow = vi.fn();

    new BleDeviceConnection(link.peripheral, link.txChar, link.rxChar, {
      registry,
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
      onFollowThread: onFollow,
    });
    await flush();

    link.deviceSends(JSON.stringify({
      v: 1, type: 'hello', seq: 1, ts: 0,
      device_id: 'doudou-x', fw_version: 'x', pairing_token: 'tok-12345',
    }));
    await flush();

    link.deviceSends(JSON.stringify({
      v: 1, type: 'follow_thread', seq: 2, ts: 1, thread_id: 'thr-42',
    }));
    await flush();

    expect(onFollow).toHaveBeenCalledTimes(1);
    expect(onFollow.mock.calls[0]![0]).toBe('thr-42');
  });

  it('closes cleanly on disconnect event', async () => {
    const link = makeMockLink();
    new BleDeviceConnection(link.peripheral, link.txChar, link.rxChar, {
      registry: new DeviceRegistry(),
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    await flush();
    link.triggerDisconnect();
    expect(link.peripheral.disconnect).toHaveBeenCalled();
  });
});
