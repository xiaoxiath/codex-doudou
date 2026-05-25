/**
 * Long-lived sidecar `codex app-server` for periodic read-only queries:
 *   - `thread/list` every threadIntervalMs (titles aren't in rollout)
 *   - `account/rateLimits/read` every limitsIntervalMs (limits update only
 *     when the host app polls; rollout's snapshot may be minutes stale)
 *
 * One subprocess for the whole bridge lifetime; idles between polls.
 * Despite the name, this class polls more than just thread lists.
 */
import { JsonRpcStdioClient } from './jsonRpcClient.js';
import { log } from '../log.js';

export interface ThreadSummary {
  id: string;
  /** AI-generated title; null when Codex hasn't named it yet. */
  name: string | null;
  /** First user message preview; null on brand-new thread. */
  preview: string | null;
  source?: string | null;
  cwd?: string | null;
  /** Epoch SECONDS per Codex; we'll convert to ms when forwarding. */
  updatedAt?: number;
  ephemeral?: boolean;
}

interface ThreadListResult {
  data?: ThreadSummary[];
}

interface RateLimitWindow {
  usedPercent?: number;
  windowDurationMins?: number;
  resetsAt?: number;
}
interface RateLimitGroup {
  limitId?: string;
  limitName?: string | null;
  primary?: RateLimitWindow | null;
  secondary?: RateLimitWindow | null;
  planType?: string | null;
}
interface RateLimitsResult {
  rateLimits?: RateLimitGroup;
  /** New per-limit-id breakdown — includes Spark etc when applicable. */
  rateLimitsByLimitId?: Record<string, RateLimitGroup>;
}

/** Normalized rate-limits payload the poller emits. */
export interface RateLimitsSnapshot {
  planType?: string;
  groups: Array<{
    id: string;
    /** Friendly label, e.g. "GPT-5.3-Codex-Spark"; undefined = main plan. */
    label?: string;
    primary?: { used_percent: number; window_minutes?: number; resets_at_ms?: number };
    secondary?: { used_percent: number; window_minutes?: number; resets_at_ms?: number };
  }>;
}

const INIT_TIMEOUT_MS = 8_000;
const CALL_TIMEOUT_MS = 6_000;
/** Sidecar restart backoff. Doubles up to MAX_BACKOFF_MS on consecutive failures. */
const MIN_BACKOFF_MS = 2_000;
const MAX_BACKOFF_MS = 60_000;

export interface ThreadListPollerOptions {
  /** thread/list polling cadence. Default 10s. */
  intervalMs?: number;
  /** account/rateLimits/read polling cadence. Default 30s. */
  limitsIntervalMs?: number;
  limit?: number;
}

export interface PollerHandlers {
  onThreads: (threads: ThreadSummary[]) => void;
  onRateLimits?: (snapshot: RateLimitsSnapshot) => void;
}

export class ThreadListPoller {
  private rpc: JsonRpcStdioClient | null = null;
  private threadTimer: ReturnType<typeof setInterval> | null = null;
  private limitsTimer: ReturnType<typeof setInterval> | null = null;
  private handlers: PollerHandlers | null = null;
  private threadInflight = false;
  private limitsInflight = false;
  private stopped = false;
  /** Exponential backoff state for sidecar (re)start attempts. */
  private currentBackoffMs = MIN_BACKOFF_MS;
  private restartInProgress: Promise<void> | null = null;

  constructor(private readonly opts: ThreadListPollerOptions = {}) {}

  async start(handlers: PollerHandlers): Promise<void> {
    this.handlers = handlers;
    await this.spawnRpc();

    // Kick both polls immediately, then on interval.
    await this.pollThreads();
    await this.pollLimits();
    const threadInterval = this.opts.intervalMs ?? 10_000;
    const limitsInterval = this.opts.limitsIntervalMs ?? 30_000;
    this.threadTimer = setInterval(() => void this.pollThreads(), threadInterval);
    this.threadTimer.unref?.();
    this.limitsTimer = setInterval(() => void this.pollLimits(), limitsInterval);
    this.limitsTimer.unref?.();
    log.info({ threadIntervalMs: threadInterval, limitsIntervalMs: limitsInterval }, 'codex sidecar poller started');
  }

  /**
   * Spawn a fresh `codex app-server` and complete the initialize handshake.
   * On success, resets the backoff. On failure throws — caller decides retry.
   */
  private async spawnRpc(): Promise<void> {
    const rpc = new JsonRpcStdioClient('codex', ['app-server']);
    await rpc.start();
    try {
      await withTimeout(
        rpc.request('initialize', {
          clientInfo: { name: 'doudou-bridge-sidecar', version: '0.1.0' },
          capabilities: null,
        }),
        INIT_TIMEOUT_MS,
        'sidecar initialize',
      );
      rpc.notify('initialized');
      rpc.setFallbackServerRequest(async () => ({ decision: 'decline' }));
      rpc.setFallbackNotification(() => undefined);
    } catch (err) {
      await rpc.stop().catch(() => undefined);
      throw err;
    }
    this.rpc = rpc;
    this.currentBackoffMs = MIN_BACKOFF_MS;
  }

  /**
   * Ensure rpc is alive before a poll. If the subprocess exited, attempt
   * to respawn. Uses exponential backoff so repeated crashes don't hammer
   * the system. Returns true if rpc is ready, false otherwise.
   */
  private async ensureRpcAlive(): Promise<boolean> {
    if (this.stopped) return false;
    if (this.rpc && !this.rpc.isDead()) return true;
    if (this.restartInProgress) {
      await this.restartInProgress;
      return Boolean(this.rpc && !this.rpc.isDead());
    }
    this.restartInProgress = (async () => {
      log.warn({ backoffMs: this.currentBackoffMs }, 'codex sidecar dead, attempting restart');
      await new Promise((res) => {
        const t = setTimeout(res, this.currentBackoffMs);
        t.unref?.();
      });
      try {
        await this.spawnRpc();
        log.info('codex sidecar restarted');
      } catch (err) {
        log.warn({ err: String(err) }, 'codex sidecar restart failed');
        this.currentBackoffMs = Math.min(this.currentBackoffMs * 2, MAX_BACKOFF_MS);
      } finally {
        this.restartInProgress = null;
      }
    })();
    await this.restartInProgress;
    return Boolean(this.rpc && !this.rpc.isDead());
  }

  async stop(): Promise<void> {
    this.stopped = true;
    if (this.threadTimer) { clearInterval(this.threadTimer); this.threadTimer = null; }
    if (this.limitsTimer) { clearInterval(this.limitsTimer); this.limitsTimer = null; }
    if (this.rpc) {
      await this.rpc.stop().catch(() => undefined);
      this.rpc = null;
    }
  }

  private async pollThreads(): Promise<void> {
    if (this.stopped || this.threadInflight) return;
    if (!(await this.ensureRpcAlive()) || !this.rpc) return;
    this.threadInflight = true;
    try {
      const r = await withTimeout(
        this.rpc.request<ThreadListResult>('thread/list', { limit: this.opts.limit ?? 12 }),
        CALL_TIMEOUT_MS,
        'thread/list',
      );
      const threads = (r.data ?? []).filter((t) => !t.ephemeral);
      this.handlers?.onThreads(threads);
    } catch (err) {
      log.warn({ err: String(err) }, 'thread/list poll failed');
    } finally {
      this.threadInflight = false;
    }
  }

  private async pollLimits(): Promise<void> {
    if (this.stopped || this.limitsInflight || !this.handlers?.onRateLimits) return;
    if (!(await this.ensureRpcAlive()) || !this.rpc) return;
    this.limitsInflight = true;
    try {
      const r = await withTimeout(
        this.rpc.request<RateLimitsResult>('account/rateLimits/read', {}),
        CALL_TIMEOUT_MS,
        'account/rateLimits/read',
      );
      const snapshot = normalizeRateLimits(r);
      if (snapshot.groups.length > 0) {
        this.handlers.onRateLimits(snapshot);
      }
    } catch (err) {
      log.warn({ err: String(err) }, 'rateLimits/read poll failed');
    } finally {
      this.limitsInflight = false;
    }
  }
}

// Exported for unit tests.
export function normalizeRateLimits(r: RateLimitsResult): RateLimitsSnapshot {
  const byId = r.rateLimitsByLimitId ?? {};
  const ids = Object.keys(byId);
  // Fall back to single `.rateLimits` if the per-id map is empty (older codex).
  if (ids.length === 0 && r.rateLimits) {
    const norm = normalizeGroup(r.rateLimits.limitId ?? 'codex', r.rateLimits);
    if (!norm) return { groups: [] };
    const { planType: _p, ...rest } = norm;
    return {
      ...(r.rateLimits.planType ? { planType: r.rateLimits.planType } : {}),
      groups: [rest],
    };
  }
  // Keep deterministic order: 'codex' first, then alphabetical.
  ids.sort((a, b) => (a === 'codex' ? -1 : b === 'codex' ? 1 : a.localeCompare(b)));
  const groups: Array<RateLimitsSnapshot['groups'][number] & { planType?: string }> = [];
  for (const id of ids) {
    const grp = byId[id];
    if (!grp) continue;
    const norm = normalizeGroup(id, grp);
    if (norm) groups.push(norm);
  }
  return {
    ...(groups[0]?.planType ? { planType: groups[0].planType } : {}),
    groups: groups.map(({ planType: _p, ...rest }) => rest),
  };
}

function normalizeGroup(id: string, g: RateLimitGroup): (RateLimitsSnapshot['groups'][number] & { planType?: string }) | null {
  if (!g) return null;
  const out: RateLimitsSnapshot['groups'][number] & { planType?: string } = {
    id,
    ...(g.limitName && id !== 'codex' ? { label: g.limitName } : {}),
    ...(g.planType ? { planType: g.planType } : {}),
  };
  if (g.primary && typeof g.primary.usedPercent === 'number') {
    out.primary = {
      used_percent: g.primary.usedPercent,
      ...(g.primary.windowDurationMins !== undefined ? { window_minutes: g.primary.windowDurationMins } : {}),
      ...(g.primary.resetsAt !== undefined ? { resets_at_ms: g.primary.resetsAt * 1000 } : {}),
    };
  }
  if (g.secondary && typeof g.secondary.usedPercent === 'number') {
    out.secondary = {
      used_percent: g.secondary.usedPercent,
      ...(g.secondary.windowDurationMins !== undefined ? { window_minutes: g.secondary.windowDurationMins } : {}),
      ...(g.secondary.resetsAt !== undefined ? { resets_at_ms: g.secondary.resetsAt * 1000 } : {}),
    };
  }
  if (!out.primary && !out.secondary) return null;
  return out;
}

function withTimeout<T>(p: Promise<T>, ms: number, what: string): Promise<T> { /* eslint-disable-line @typescript-eslint/no-shadow */
  return new Promise<T>((resolve, reject) => {
    const t = setTimeout(() => reject(new Error(`${what} timed out (${ms}ms)`)), ms);
    t.unref?.();
    p.then(
      (v) => { clearTimeout(t); resolve(v); },
      (e) => { clearTimeout(t); reject(e); },
    );
  });
}
