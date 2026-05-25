/**
 * BLE counterpart to deviceConnection.ts.
 *
 * Wraps one connected noble peripheral and implements the same
 * DeviceTransport interface the registry consumes, so the rest of the
 * bridge doesn't care whether a device is on WebSocket or BLE.
 *
 * Wire flow:
 *   Device → bridge:
 *     Notifications on the TX characteristic arrive as Buffers. Each
 *     buffer is one chunk; `Reassembler` joins them into a JSON
 *     envelope, which we feed to `parseFromDevice` for type-safe
 *     dispatch. Same handshake / hello / reply semantics as WS.
 *
 *   Bridge → device:
 *     `sendStatus` / `sendQuestion` / ... build the envelope, stamp
 *     v/seq/ts, JSON-encode, then chunk + write to the RX char.
 */
import { performance } from 'node:perf_hooks';

import {
  PROTOCOL_VERSION,
  parseFromDevice,
  type DeviceToBridge,
  type Hello,
  type Reply,
  type Welcome,
} from '@doudou/device-protocol';

import { log } from '../log.js';
import {
  type DeviceRegistry,
  type DeviceState,
  type DeviceTransport,
  type ErrorPayload,
  type QuestionPayload,
  type SessionInfoPayload,
  type StatusPayload,
  type ThreadListPayload,
  type UsagePayload,
} from '../deviceRegistry.js';
import { encodeMessage, Reassembler } from './framing.js';

/** Minimum BLE peripheral surface we need — keeps us mockable without
 *  pulling in @abandonware/noble's full type tree. */
export interface BleChar {
  /** Subscribe to notifies on this characteristic (TX, device → bridge). */
  subscribe(cb: (data: Buffer) => void): Promise<void> | void;
  /** Write a chunk to this characteristic (RX, bridge → device). */
  write(data: Buffer, withoutResponse: boolean): Promise<void> | void;
}

export interface BlePeripheral {
  id: string;
  /** noble exposes `name` from the GAP advertisement; we use it to seed deviceId before hello. */
  advertisedName?: string;
  disconnect(): Promise<void> | void;
  /** Negotiated ATT MTU; falls back to 23 if unknown. */
  mtu(): number;
  /** Subscribe to disconnect events. */
  onDisconnect(cb: (reason?: string) => void): void;
}

export interface BleDeviceConnectionOptions {
  registry: DeviceRegistry;
  /** Token the firmware sent in its hello — bridge rejects mismatches. */
  pairingToken: string;
  /** Called with the matched DeviceState + reply after a question replies. */
  onReply: (
    state: DeviceState,
    reply: Reply,
    payload: QuestionPayload,
    latencyMs: number,
  ) => Promise<void> | void;
  /** Called once after the hello/welcome handshake completes. */
  onReady?: (state: DeviceState) => void;
  /** Called when the firmware requests Bridge follow a specific thread. */
  onFollowThread?: (threadId: string, state: DeviceState) => Promise<void> | void;
}

const HELLO_TIMEOUT_MS = 5_000;       // BLE is slower than WS, give it a beat
const HEARTBEAT_MS = 20_000;

export class BleDeviceConnection implements DeviceTransport {
  private outSeq = 1;
  private startMs = performance.now();
  private lastInboundAt = performance.now();
  private state: DeviceState | null = null;
  private status: 'connecting' | 'ready' | 'closed' = 'connecting';
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null;
  private helloTimer: ReturnType<typeof setTimeout> | null = null;
  private readonly reassembler = new Reassembler();
  /** Sequence counter for our chunk header (independent of msg seq). */
  private chunkSeq = 0;

  constructor(
    private readonly peripheral: BlePeripheral,
    private readonly txChar: BleChar,
    private readonly rxChar: BleChar,
    private readonly opts: BleDeviceConnectionOptions,
  ) {
    this.helloTimer = setTimeout(() => {
      if (this.status === 'connecting') {
        log.warn({ peripheral: peripheral.id }, 'ble hello timeout, closing');
        void this.close();
      }
    }, HELLO_TIMEOUT_MS);

    peripheral.onDisconnect((reason) => {
      log.info({ peripheral: peripheral.id, reason }, 'ble peripheral disconnected');
      this.close();
    });

    // Subscribe to TX notifies — device → bridge.
    void Promise.resolve(txChar.subscribe((data) => this.onChunk(data))).catch((err) => {
      log.warn({ err: String(err), peripheral: peripheral.id }, 'ble subscribe failed');
      this.close();
    });
  }

  // ---------- DeviceTransport ----------

  resetSeq(): void {
    this.outSeq = 1;
    this.startMs = performance.now();
  }

  sendStatus(payload: StatusPayload): number {
    return this.rawSend({ type: 'status', ...payload });
  }
  sendQuestion(payload: QuestionPayload, expires_at: number, queue_total: number): number {
    const { expires_in_ms: _drop, ...rest } = payload;
    return this.rawSend({ type: 'question', ...rest, expires_at, queue_total });
  }
  sendError(payload: ErrorPayload): number {
    return this.rawSend({ type: 'error', ...payload });
  }
  sendUsage(payload: UsagePayload): number {
    return this.rawSend({ type: 'usage', ...payload });
  }
  sendSessionInfo(payload: SessionInfoPayload): number {
    return this.rawSend({ type: 'session_info', ...payload });
  }
  sendThreadList(payload: ThreadListPayload): number {
    return this.rawSend({ type: 'thread_list', ...payload });
  }
  sendAck(ackForSeq: number): void {
    this.rawSend({ type: 'ack', ack_for_seq: ackForSeq });
  }
  sendPing(): void {
    this.rawSend({ type: 'ping' });
  }
  sendPong(pongForSeq: number): void {
    this.rawSend({ type: 'pong', pong_for_seq: pongForSeq });
  }

  close(code = 0, reason?: string): void {
    void code;
    if (this.status === 'closed') return;
    this.status = 'closed';
    this.clearTimers();
    void Promise.resolve(this.peripheral.disconnect()).catch(() => {
      /* ignore — disconnect during connection teardown is fine */
    });
    if (reason) {
      log.debug({ peripheral: this.peripheral.id, reason }, 'ble close reason');
    }
  }

  // ---------- internals ----------

  private rawSend(envelope: Record<string, unknown> & { type: string }): number {
    if (this.status === 'closed') return -1;
    const seq = this.outSeq++;
    const msg = {
      v: PROTOCOL_VERSION,
      seq,
      ts: Math.round(performance.now() - this.startMs),
      ...envelope,
    };
    const body = JSON.stringify(msg);
    const chunks = encodeMessage(body, this.peripheral.mtu(), this.chunkSeq++ & 0xff);
    void this.writeChunks(chunks).catch((err) => {
      log.warn({ err: String(err), peripheral: this.peripheral.id }, 'ble write failed');
      this.close();
    });
    return seq;
  }

  private async writeChunks(chunks: Buffer[]): Promise<void> {
    // Serialise writes — noble.js doesn't queue and parallel writes can
    // be reordered by the controller.
    for (const chunk of chunks) {
      await Promise.resolve(this.rxChar.write(chunk, /*withoutResponse=*/ false));
    }
  }

  private onChunk(data: Buffer): void {
    this.lastInboundAt = performance.now();
    let body: string | null;
    try {
      body = this.reassembler.feed(data);
    } catch (err) {
      log.warn({ err: String(err), peripheral: this.peripheral.id }, 'ble reassembly error');
      return;
    }
    if (body === null) return;
    this.onRaw(body);
  }

  private onRaw(json: string): void {
    let raw: unknown;
    try {
      raw = JSON.parse(json);
    } catch (err) {
      log.warn({ err: String(err), peripheral: this.peripheral.id }, 'ble json parse error');
      return;
    }
    const parsed = parseFromDevice(raw);
    if (!parsed.ok) {
      log.warn({ err: parsed.error, peripheral: this.peripheral.id }, 'ble schema error');
      return;
    }
    const msg: DeviceToBridge = parsed.value;

    switch (msg.type) {
      case 'hello':    this.onHello(msg); return;
      case 'reply':    this.onReply(msg); return;
      case 'follow_thread':
        if (this.state && this.opts.onFollowThread) {
          void this.opts.onFollowThread(msg.thread_id, this.state);
        }
        return;
      case 'pong':     return;   // nothing to verify yet
      case 'device_status': return;  // info-only
      default:
        log.debug({ type: (msg as { type: string }).type }, 'ble: ignoring device msg');
    }
  }

  private onHello(msg: Hello): void {
    if (this.status !== 'connecting') return;
    if (msg.pairing_token !== this.opts.pairingToken) {
      log.warn({ peripheral: this.peripheral.id }, 'ble hello rejected: bad pairing_token');
      this.close();
      return;
    }
    const state = this.opts.registry.getOrCreate(msg.device_id);
    state.attach(this, msg.resume_after_seq ?? 0);
    this.state = state;
    this.status = 'ready';
    this.clearHelloTimer();
    this.startHeartbeat();

    const welcome: Welcome = {
      v: PROTOCOL_VERSION,
      type: 'welcome',
      seq: this.outSeq++,
      ts: Math.round(performance.now() - this.startMs),
      session_id: `sess_${Math.random().toString(36).slice(2, 10)}`,
      server_time_ms: Date.now(),
      heartbeat_interval_ms: HEARTBEAT_MS,
      max_question_choices: 4,
      features: ['ack', 'device_status'],
    };
    this.rawSend(welcome);
    if (this.opts.onReady) this.opts.onReady(state);
    log.info({ peripheral: this.peripheral.id, device: msg.device_id }, 'ble device ready');
  }

  private onReply(reply: Reply): void {
    if (!this.state) return;
    // DeviceState.onReply marshals the reply through the inflight
    // promise pushQuestion is awaiting — that's where the bridge picks
    // it up. We don't fire opts.onReply here for the same reason WS
    // doesn't (see deviceConnection.ts).
    this.state.onReply(reply);
  }

  private startHeartbeat(): void {
    if (this.heartbeatTimer) return;
    this.heartbeatTimer = setInterval(() => {
      if (this.status !== 'ready') return;
      this.sendPing();
    }, HEARTBEAT_MS);
  }

  private clearHelloTimer(): void {
    if (this.helloTimer) { clearTimeout(this.helloTimer); this.helloTimer = null; }
  }

  private clearTimers(): void {
    this.clearHelloTimer();
    if (this.heartbeatTimer) { clearInterval(this.heartbeatTimer); this.heartbeatTimer = null; }
  }
}
