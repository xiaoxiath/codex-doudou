/**
 * Per-device state that survives WebSocket reconnects.
 *
 * Lifecycle:
 *   socket open → DeviceConnection created → hello arrives → connection
 *   asks the DeviceRegistry to attach to a DeviceState (created if absent)
 *   → state replays unexpired in-flight questions per hello.resume_after_seq
 *   → ready
 *
 * On socket close the DeviceState detaches but stays in memory holding any
 * questions still awaiting reply. The next hello with the same device_id
 * picks up where we left off.
 */
import { performance } from 'node:perf_hooks';

import {
  PROTOCOL_VERSION,
  type ActionType,
  type ErrorCode,
  type Question,
  type Reply,
  type Risk,
  type State,
} from '@doudou/device-protocol';

interface UsageLimit {
  id: string;
  label?: string;
  group_label?: string;
  used_pct?: number;
  window_minutes?: number;
  resets_at?: number;
}

import { log } from './log.js';

export interface QuestionPayload {
  id: string;
  risk: Risk;
  action_type: ActionType;
  title: string;
  body?: string;
  choices: Array<{ id: string; label: string }>;
  expires_in_ms: number;
  require_confirm: boolean;
}

export interface StatusPayload {
  state: State;
  title: string;
  body?: string;
}

export interface ErrorPayload {
  code: ErrorCode;
  title: string;
  body?: string;
  related_id?: string;
}

export interface ThreadSummaryPayload {
  id: string;
  title: string;
  source?: string;
  active?: boolean;
  updated_at?: number;
}

export interface ThreadListPayload {
  threads: ThreadSummaryPayload[];
}

export interface SessionInfoPayload {
  session_id?: string;
  thread_title?: string;
  source?: string;
  model?: string;
  reasoning_effort?: string;
  summary_mode?: string;
  cwd?: string;
  permissions?: string;
  approval_policy?: string;
  sandbox?: string;
  collaboration_mode?: string;
  account_email?: string;
  plan_type?: string;
  agents_md?: boolean;
  git_branch?: string;
  cli_version?: string;
}

export interface UsagePayload {
  session?: {
    input_tokens?: number;
    output_tokens?: number;
    cached_tokens?: number;
    total_tokens?: number;
    current_context_tokens?: number;
    model_context_window?: number;
  };
  limits?: UsageLimit[];
  plan_type?: string;
}

/**
 * In-flight bookkeeping for one question. Survives reconnects.
 */
interface InflightQuestion {
  payload: QuestionPayload;
  /** Absolute ms timestamp (Bridge clock). */
  expires_at: number;
  /** perf clock at first send, used for latency telemetry. */
  first_sent_perf: number;
  resolve: (reply: Reply) => void;
  reject: (err: Error) => void;
}

/** A reply, plus context the bridge needs for audit / Codex response routing. */
export interface AuditedReply {
  reply: Reply;
  payload: QuestionPayload;
  latency_ms: number;
}

/**
 * Transport contract the registry expects from a per-socket connection.
 * Lets us unit-test state without spinning up real sockets.
 */
export interface DeviceTransport {
  /** Stamps v/seq/ts and writes to wire. Returns the seq used (for tests). */
  sendStatus(payload: StatusPayload): number;
  sendQuestion(payload: QuestionPayload, expires_at: number, queue_total: number): number;
  sendError(payload: ErrorPayload): number;
  sendUsage(payload: UsagePayload): number;
  sendSessionInfo(payload: SessionInfoPayload): number;
  sendThreadList(payload: ThreadListPayload): number;
  sendAck(ackForSeq: number): void;
  sendPing(): void;
  sendPong(pongForSeq: number): void;
  /** Force-close the socket. */
  close(code?: number, reason?: string): void;
  /** Reset outbound counter — called on (re)attach. */
  resetSeq(): void;
}

export class DeviceState {
  private transport: DeviceTransport | null = null;
  private inflight = new Map<string, InflightQuestion>();
  private repliedIds = new Map<string, string>();
  /** Most recent status, kept so we can re-push on reconnect. */
  private lastStatus: StatusPayload | null = null;
  /** Most recent usage snapshot, kept for reconnect. */
  private lastUsage: UsagePayload | null = null;
  /** Most recent session-info snapshot (merged), kept for reconnect. */
  private lastSessionInfo: SessionInfoPayload | null = null;
  /** Most recent thread list, kept for reconnect. */
  private lastThreadList: ThreadListPayload | null = null;

  constructor(public readonly deviceId: string) {}

  // ---------- connection lifecycle ----------

  attach(transport: DeviceTransport, resumeAfterSeq: number): void {
    if (this.transport) {
      log.warn({ device: this.deviceId }, 'attach: replacing existing connection');
      this.transport.close(1000, 'replaced by new connection');
    }
    this.transport = transport;
    transport.resetSeq();
    void resumeAfterSeq; // not used directly — see comment below
    this.replay();
  }

  detach(): void {
    this.transport = null;
  }

  /**
   * Re-send everything the device should know about right now:
   *  - current status (if any) so the UI doesn't sit on stale "connecting"
   *  - every unexpired in-flight question
   *
   * `resume_after_seq` from hello is informational only — seq numbers
   * reset on every reconnect (per device-protocol.md §2), so we always
   * replay the full unexpired set. The device ignores duplicates by
   * `question.id`.
   */
  private replay(): void {
    if (!this.transport) return;
    if (this.lastStatus) {
      this.transport.sendStatus(this.lastStatus);
    }
    if (this.lastUsage) {
      this.transport.sendUsage(this.lastUsage);
    }
    if (this.lastSessionInfo) {
      this.transport.sendSessionInfo(this.lastSessionInfo);
    }
    if (this.lastThreadList) {
      this.transport.sendThreadList(this.lastThreadList);
    }
    const now = Date.now();
    // Snapshot inflight count for queue_total before iterating.
    const total = Array.from(this.inflight.values()).filter((q) => q.expires_at > now).length;
    let remaining = total;
    for (const [id, inflight] of this.inflight) {
      if (inflight.expires_at <= now) {
        log.debug({ device: this.deviceId, id }, 'dropping expired question on replay');
        this.inflight.delete(id);
        inflight.reject(new Error('expired during reconnect'));
        continue;
      }
      this.transport.sendQuestion(inflight.payload, inflight.expires_at, remaining);
      remaining--;
    }
  }

  // ---------- outbound (called by Bridge / feed) ----------

  pushStatus(payload: StatusPayload): void {
    this.lastStatus = payload;
    this.transport?.sendStatus(payload);
  }

  pushError(payload: ErrorPayload): void {
    this.transport?.sendError(payload);
  }

  pushUsage(payload: UsagePayload): void {
    // Merge with previous snapshot — Codex sends session and limits separately.
    this.lastUsage = {
      session: { ...(this.lastUsage?.session ?? {}), ...(payload.session ?? {}) },
      limits: payload.limits ?? this.lastUsage?.limits,
      plan_type: payload.plan_type ?? this.lastUsage?.plan_type,
    };
    this.transport?.sendUsage(this.lastUsage);
  }

  pushSessionInfo(payload: SessionInfoPayload): void {
    this.lastSessionInfo = {
      ...(this.lastSessionInfo ?? {}),
      ...payload,
    };
    this.transport?.sendSessionInfo(this.lastSessionInfo);
  }

  pushThreadList(payload: ThreadListPayload): void {
    this.lastThreadList = payload;
    this.transport?.sendThreadList(payload);
  }

  pushQuestion(payload: QuestionPayload): Promise<AuditedReply> {
    if (this.inflight.has(payload.id)) {
      // Re-issuing the same id is treated as overwriting — uncommon, but possible
      // if the feed retries. Cancel the prior promise so caller can re-await.
      const prior = this.inflight.get(payload.id)!;
      prior.reject(new Error('superseded by new question with same id'));
      this.inflight.delete(payload.id);
    }
    const expires_at = Date.now() + payload.expires_in_ms;
    const promise = new Promise<AuditedReply>((resolveBase, rejectBase) => {
      const inflight: InflightQuestion = {
        payload,
        expires_at,
        first_sent_perf: performance.now(),
        resolve: (reply) => {
          this.inflight.delete(payload.id);
          const latency_ms = Math.round(performance.now() - inflight.first_sent_perf);
          resolveBase({ reply, payload, latency_ms });
        },
        reject: (err) => {
          this.inflight.delete(payload.id);
          rejectBase(err);
        },
      };
      this.inflight.set(payload.id, inflight);

      // Timer to enforce expiry even if device never replies.
      const timer = setTimeout(() => {
        const still = this.inflight.get(payload.id);
        if (still) {
          this.inflight.delete(payload.id);
          this.transport?.sendError({
            code: 'request_expired',
            title: '请求已过期',
            body: '请回到电脑继续处理',
            related_id: payload.id,
          });
          rejectBase(new Error(`question ${payload.id} expired`));
        }
      }, payload.expires_in_ms + 500);
      timer.unref?.();
    });

    // queue_total = total currently in-flight (including this one we just added)
    const queueTotal = this.inflight.size;
    this.transport?.sendQuestion(payload, expires_at, queueTotal);
    return promise;
  }

  // ---------- inbound (called by DeviceConnection) ----------

  onReply(reply: Reply): { kind: 'duplicate' | 'matched' | 'orphan' } {
    if (this.repliedIds.has(reply.id)) {
      this.transport?.sendAck(reply.seq);
      return { kind: 'duplicate' };
    }
    const inflight = this.inflight.get(reply.id);
    if (!inflight) {
      // Could be a reply to a question we already expired, or to one this
      // bridge process never saw. Ack so device doesn't retry forever.
      this.transport?.sendAck(reply.seq);
      return { kind: 'orphan' };
    }
    this.repliedIds.set(reply.id, reply.choice_id);
    this.transport?.sendAck(reply.seq);
    inflight.resolve(reply);
    return { kind: 'matched' };
  }

  onPing(seq: number): void {
    this.transport?.sendPong(seq);
  }

  // ---------- introspection (tests / metrics) ----------

  get inflightSize(): number {
    return this.inflight.size;
  }
  get repliedCount(): number {
    return this.repliedIds.size;
  }
  get isConnected(): boolean {
    return this.transport !== null;
  }
}

export class DeviceRegistry {
  private byId = new Map<string, DeviceState>();

  getOrCreate(deviceId: string): DeviceState {
    let state = this.byId.get(deviceId);
    if (!state) {
      state = new DeviceState(deviceId);
      this.byId.set(deviceId, state);
      log.info({ device: deviceId }, 'device state created');
    }
    return state;
  }

  /** Iterator over all known device states (connected or not). */
  *all(): IterableIterator<DeviceState> {
    yield* this.byId.values();
  }

  /** Forget a device entirely (used on unpair). */
  forget(deviceId: string): void {
    const state = this.byId.get(deviceId);
    if (state) {
      state.detach();
      this.byId.delete(deviceId);
    }
  }

  get size(): number {
    return this.byId.size;
  }
}

// Re-export for the Bridge orchestrator
export const PROTOCOL_VERSION_REEXPORT = PROTOCOL_VERSION;
