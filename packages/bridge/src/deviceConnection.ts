/**
 * Thin transport wrapper around one WebSocket.
 *
 * State machine inside DeviceState is durable; this object dies with the
 * socket. The hello handshake binds this connection to a DeviceState in
 * the registry — see deviceRegistry.ts.
 */
import { performance } from 'node:perf_hooks';
import type { WebSocket } from 'ws';

import {
  PROTOCOL_VERSION,
  parseFromDevice,
  type DeviceToBridge,
  type Hello,
  type Reply,
  type Welcome,
} from '@doudou/device-protocol';

import { log } from './log.js';
import { normalizeEnvelope } from './textNormalize.js';
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
} from './deviceRegistry.js';

const HEARTBEAT_MS = 15_000;
const HELLO_TIMEOUT_MS = 2_000;
const HEARTBEAT_GRACE = 2;

export interface DeviceConnectionOptions {
  registry: DeviceRegistry;
  pairingToken: string;
  /** Called when a matched reply arrives (after audit / Codex roundtrip is the caller's job). */
  onReply: (state: DeviceState, reply: Reply, payload: QuestionPayload, latencyMs: number) => Promise<void> | void;
  /** Called after successful hello/welcome — bridge uses this to seed cached snapshots. */
  onReady?: (state: DeviceState) => void;
  /** Called when the device asks Bridge to follow a specific thread. */
  onFollowThread?: (threadId: string, state: DeviceState) => Promise<void> | void;
}

export class DeviceConnection implements DeviceTransport {
  private outSeq = 1;
  private startMs = performance.now();
  private lastInboundAt = performance.now();
  private state: DeviceState | null = null;
  private status: 'connecting' | 'ready' | 'closed' = 'connecting';
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null;
  private helloTimer: ReturnType<typeof setTimeout> | null = null;
  private sessionId = '';

  constructor(
    private readonly ws: WebSocket,
    private readonly opts: DeviceConnectionOptions,
  ) {
    this.helloTimer = setTimeout(() => {
      if (this.status === 'connecting') {
        log.warn('hello timeout, closing');
        this.close(1002, 'hello timeout');
      }
    }, HELLO_TIMEOUT_MS);

    ws.on('message', (raw) => this.onRaw(raw.toString()));
    /* Log close code + reason so we can correlate device-side
     * disconnect logs with what the bridge end actually decided. */
    ws.on('close', (code, reason) => {
      log.info(
        {
          device: this.state?.deviceId,
          code,
          reason: reason?.toString() || '',
        },
        'ws close',
      );
      this.onClose();
    });
    ws.on('error', (err) =>
      log.warn({ err: String(err), device: this.state?.deviceId }, 'ws error'),
    );
  }

  // ---------- DeviceTransport implementation ----------

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

  close(code = 1000, reason = ''): void {
    if (this.status === 'closed') return;
    this.status = 'closed';
    this.clearTimers();
    try {
      this.ws.close(code, reason);
    } catch {
      /* ignore */
    }
  }

  // ---------- internal ----------

  private rawSend(envelope: Record<string, unknown> & { type: string }): number {
    if (this.ws.readyState !== this.ws.OPEN) return -1;
    const seq = this.outSeq++;
    const msg = {
      v: PROTOCOL_VERSION,
      seq,
      ts: this.localTs(),
      ...envelope,
    };
    /* Fold CJK/full-width punctuation that isn't in the device's
     * GB2312 一级 font subset down to ASCII — see textNormalize.ts. */
    normalizeEnvelope(msg);
    this.ws.send(JSON.stringify(msg));
    return seq;
  }

  private localTs(): number {
    return Math.round(performance.now() - this.startMs);
  }

  private onRaw(raw: string): void {
    this.lastInboundAt = performance.now();
    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch (err) {
      log.warn({ err: String(err) }, 'invalid JSON from device');
      return;
    }
    const result = parseFromDevice(parsed);
    if (!result.ok) {
      log.warn({ raw, error: result.error }, 'rejected device message');
      return;
    }
    this.dispatch(result.value);
  }

  private dispatch(msg: DeviceToBridge): void {
    if (this.status !== 'ready' && msg.type !== 'hello') {
      log.warn({ type: msg.type, state: this.status }, 'pre-handshake message dropped');
      return;
    }
    switch (msg.type) {
      case 'hello':
        return this.onHello(msg);
      case 'reply':
        return this.onReply(msg);
      case 'follow_thread':
        if (this.state && this.opts.onFollowThread) {
          void this.opts.onFollowThread(msg.thread_id, this.state);
        }
        return;
      case 'ping':
        return this.state?.onPing(msg.seq);
      case 'pong':
      case 'ack':
      case 'device_status':
        return;
    }
  }

  private onHello(hello: Hello): void {
    if (this.status !== 'connecting') {
      log.warn({ device: hello.device_id, state: this.status }, 'duplicate hello');
      return;
    }
    if (hello.pairing_token !== this.opts.pairingToken) {
      log.warn({ device: hello.device_id }, 'pairing token mismatch');
      // Cannot use sendError pre-attach since we haven't bound state, but the
      // wire format is identical — write directly.
      this.rawSend({
        type: 'error',
        code: 'pairing_invalid',
        title: '配对失效',
        body: '请回到电脑重新配对',
      });
      this.close(1008, 'pairing_invalid');
      return;
    }
    this.clearHelloTimer();

    const state = this.opts.registry.getOrCreate(hello.device_id);
    this.state = state;
    this.sessionId = `sess_${Math.random().toString(36).slice(2, 10)}`;

    // Send welcome BEFORE attaching — replay shouldn't see welcome's seq.
    const welcome: Welcome = {
      v: PROTOCOL_VERSION,
      type: 'welcome',
      seq: this.outSeq++,
      ts: this.localTs(),
      server_time_ms: Date.now(),
      session_id: this.sessionId,
      heartbeat_interval_ms: HEARTBEAT_MS,
      max_question_choices: 4,
      features: ['ack', 'device_status'],
    };
    this.ws.send(JSON.stringify(welcome));

    this.status = 'ready';
    log.info(
      {
        device: hello.device_id,
        session: this.sessionId,
        fw: hello.fw_version,
        resume: hello.resume_after_seq ?? 0,
        inflightBefore: state.inflightSize,
      },
      'device ready',
    );

    state.attach(this, hello.resume_after_seq ?? 0);
    this.opts.onReady?.(state);

    this.heartbeatTimer = setInterval(() => this.heartbeatTick(), HEARTBEAT_MS);
  }

  private onReply(reply: Reply): void {
    const state = this.state;
    if (!state) return;
    const result = state.onReply(reply);
    if (result.kind === 'matched') {
      // resolve callback hooked through DeviceState.pushQuestion's Promise —
      // we don't fire opts.onReply here because the bridge already awaits the
      // promise returned from pushQuestion. This split keeps audit logic out
      // of the transport layer entirely.
    } else {
      log.debug({ id: reply.id, kind: result.kind, device: state.deviceId }, 'reply non-matched');
    }
  }

  private heartbeatTick(): void {
    const idleMs = performance.now() - this.lastInboundAt;
    if (idleMs > HEARTBEAT_MS * HEARTBEAT_GRACE) {
      log.warn({ device: this.state?.deviceId, idleMs }, 'heartbeat timeout');
      this.close(1001, 'heartbeat_timeout');
      return;
    }
    this.sendPing();
  }

  private onClose(): void {
    if (this.status === 'closed') {
      this.clearTimers();
      this.state?.detach();
      return;
    }
    this.status = 'closed';
    this.clearTimers();
    if (this.state) {
      log.info(
        { device: this.state.deviceId, inflight: this.state.inflightSize },
        'device disconnected (state retained)',
      );
      this.state.detach();
    }
  }

  private clearHelloTimer(): void {
    if (this.helloTimer) {
      clearTimeout(this.helloTimer);
      this.helloTimer = null;
    }
  }
  private clearTimers(): void {
    this.clearHelloTimer();
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
  }
}
