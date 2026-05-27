/**
 * Watches ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl files.
 *
 * Codex Desktop / CLI / VS Code all persist their threads to per-day folders
 * here. The file is append-only JSONL; tailing it gives us a read-only feed
 * of what the user is doing in any Codex client on this machine — without
 * needing live IPC into that client's app-server process.
 *
 * Caveats vs RealCodexFeed:
 *  - Read-only. We can't approve commands or send prompts; those flows
 *    happen inside the owning client.
 *  - We pick the rollout with the most recent mtime as "the active thread"
 *    and re-scan periodically so newly-opened Desktop chats switch in.
 *  - Status mapping is conservative: rollouts don't have an explicit
 *    "executing command" event, so we infer state from task_started /
 *    user_message / agent_message / task_complete.
 */
import { createReadStream, watch, type FSWatcher } from 'node:fs';
import { stat, readdir, access } from 'node:fs/promises';
import { constants as fsConstants } from 'node:fs';
import { createInterface } from 'node:readline';
import { basename, join } from 'node:path';
import { homedir } from 'node:os';

import { log } from '../log.js';
import type { BridgeEvent, CodexFeed, ReplyDecision } from '../codexFeed.js';
import type { AccountInfo } from './accountProbe.js';

type UsageLimitOut = {
  id: string;
  label?: string;
  used_pct?: number;
  window_minutes?: number;
  resets_at?: number;
};

function buildLimit(id: 'primary' | 'secondary', w: RateLimitWindow): UsageLimitOut {
  const label =
    id === 'primary'
      ? w.window_minutes ? `${Math.round(w.window_minutes / 60)}h` : 'primary'
      : w.window_minutes ? `${Math.round(w.window_minutes / 60 / 24)}d` : 'secondary';
  return {
    id,
    label,
    ...(w.used_percent !== undefined ? { used_pct: w.used_percent } : {}),
    ...(w.window_minutes !== undefined ? { window_minutes: w.window_minutes } : {}),
    ...(w.resets_at !== undefined ? { resets_at: w.resets_at * 1000 } : {}), // rollout stores epoch seconds
  };
}

interface RateLimitWindow {
  used_percent?: number;
  window_minutes?: number;
  resets_at?: number;
}
interface RateLimits {
  primary?: RateLimitWindow | null;
  secondary?: RateLimitWindow | null;
  plan_type?: string | null;
}

const DEFAULT_SESSIONS_ROOT = join(homedir(), '.codex', 'sessions');

export interface RolloutWatcherOptions {
  /** How often to re-scan for a newer rollout (ms). */
  scanIntervalMs?: number;
  /** Only follow threads with this source (e.g. "vscode"). Null = any. */
  filterSource?: string | null;
  /** Override the sessions root (default: `~/.codex/sessions`).
   *  Injectable so unit tests can point at a temp directory. */
  sessionsRoot?: string;
}

interface RolloutMeta {
  path: string;
  threadId: string;
  modelContextWindow?: number;
  cwd?: string;
  source?: string;
}

export interface ActivityEntry {
  ts: string;                    // ISO timestamp from rollout
  kind: 'user' | 'agent' | 'task_complete';
  text?: string;                 // for user / agent
  thread_id: string;
  source?: string;
}

const ACTIVITY_BUFFER_SIZE = 32;

export class RolloutWatcherFeed implements CodexFeed {
  private onEvent: ((e: BridgeEvent) => void) | null = null;
  private current: RolloutMeta | null = null;
  private offset = 0;
  private fileBuf = '';
  private fsWatcher: FSWatcher | null = null;
  private scanTimer: ReturnType<typeof setInterval> | null = null;
  private readingTail = false;
  private pendingRead = false;
  private stopped = false;
  /** Ring buffer of recent activity for the right-screen "活动" view. */
  private activity: ActivityEntry[] = [];
  /** Used by attachTo to detect we're replaying historical content vs live tail. */
  private replayingHistory = false;
  /** True if any status/etc. was actually pushed live during the most
   *  recent replay (because a recent entry slipped through). attachTo
   *  uses this to skip the "跟随会话 idle" stamp that would otherwise
   *  clobber the fresh thinking/executing state. */
  private emittedDuringReplay = false;
  /** Current rollout's session_meta timestamp parsed to ms, used to skip very old replay events. */
  private currentSessionStartMs = 0;
  /** Latest merged usage snapshot — replayed to new devices on connect. */
  private lastUsage: UsagePayloadLite | null = null;
  /** Latest merged session info — replayed to new devices on connect. */
  private lastSessionInfo: SessionInfoLite | null = null;
  /** Account info fetched once at bridge startup. */
  private account: AccountInfo | null = null;
  /** Resolved root directory containing `YYYY/MM/DD/rollout-*.jsonl`. */
  private readonly sessionsRoot: string;
  /** Cache of rollout path → originator (read once from session_meta).
   *  Rollouts are append-only; the first line never changes, so this is
   *  safe to keep for the bridge's lifetime. */
  private readonly originatorCache = new Map<string, string | null>();

  constructor(private readonly opts: RolloutWatcherOptions = {}) {
    this.sessionsRoot = opts.sessionsRoot ?? DEFAULT_SESSIONS_ROOT;
  }

  /** Return newest-first activity entries. */
  getRecentActivity(n = 12): ActivityEntry[] {
    return this.activity.slice(-n).reverse();
  }

  /** Latest usage snapshot (merged across token_count events). */
  getLastUsage(): UsagePayloadLite | null {
    return this.lastUsage;
  }

  /** Latest session-info snapshot (merged). */
  getLastSessionInfo(): SessionInfoLite | null {
    return this.lastSessionInfo;
  }

  /** Currently followed thread id (matches Codex thread/list id). */
  getCurrentThreadId(): string | undefined {
    return this.current?.threadId;
  }

  /** Inject account info from accountProbe; triggers a session_info refresh. */
  setAccountInfo(account: AccountInfo): void {
    this.account = account;
    this.emitSessionInfoMerge({
      ...(account.email ? { account_email: account.email } : {}),
      ...(account.planType ? { plan_type: account.planType } : {}),
    });
  }

  /**
   * Manually switch to follow a specific thread by id. Used when the user
   * taps a thread in the device's thread-list screen. Locates the rollout
   * file matching the UUID, then re-attaches.
   */
  async followThread(threadId: string): Promise<boolean> {
    if (!threadId) return false;
    const path = await this.findRolloutByThreadId(threadId);
    if (!path) {
      log.warn({ threadId }, 'followThread: rollout not found on disk');
      return false;
    }
    if (this.current?.path === path) {
      log.debug({ threadId }, 'followThread: already attached to this thread');
      return true;
    }
    log.info({ threadId, path }, 'followThread: switching');
    await this.attachTo(path);
    return true;
  }

  private async findRolloutByThreadId(threadId: string): Promise<string | null> {
    // Codex thread UUIDs are appended to rollout filenames. Walk last 7 days.
    const today = new Date();
    for (let dayOffset = 0; dayOffset < 7; dayOffset++) {
      const d = new Date(today.getTime() - dayOffset * 86400_000);
      const dir = join(
        this.sessionsRoot,
        String(d.getFullYear()),
        String(d.getMonth() + 1).padStart(2, '0'),
        String(d.getDate()).padStart(2, '0'),
      );
      let files: string[];
      try {
        files = await readdir(dir);
      } catch {
        continue;
      }
      const hit = files.find((f) => f.startsWith('rollout-') && f.endsWith(`-${threadId}.jsonl`));
      if (hit) return join(dir, hit);
    }
    return null;
  }

  async start(handlers: { onEvent: (e: BridgeEvent) => void }): Promise<void> {
    this.onEvent = handlers.onEvent;
    await this.switchToLatest();
    const interval = this.opts.scanIntervalMs ?? 3000;
    this.scanTimer = setInterval(() => void this.maybeSwitch(), interval);
    this.scanTimer.unref?.();
  }

  async sendPrompt(_text: string): Promise<void> {
    throw new Error('rollout watcher is read-only — switch DOUDOU_SOURCE=own to send prompts via doudou');
  }

  async acceptReply(_decision: ReplyDecision): Promise<void> {
    // no-op — we don't generate approval questions in this mode
  }

  async stop(): Promise<void> {
    this.stopped = true;
    if (this.scanTimer) {
      clearInterval(this.scanTimer);
      this.scanTimer = null;
    }
    this.closeWatcher();
  }

  // ---------- file discovery ----------

  private async findLatestRollout(): Promise<string | null> {
    // Walk year/month/day folders, collect ALL candidates, sort by mtime
    // desc, then peek each one's `session_meta.originator` until we find
    // one the user actually cares about.
    //
    // Why filtering matters: anything that talks to a local Codex CLI
    // writes a rollout here — including Claude Code chats, our own
    // sidecar `account/read` probes, and ad-hoc `codex_cli` runs. Without
    // a filter, "latest rollout" can be any of those instead of the
    // user's Codex Desktop thread.
    //
    // Window is 90 days so Codex Desktop sessions that started weeks ago
    // and are still being appended to (the desktop app reuses one long
    // thread file rather than creating new ones per chat) are picked up.
    const candidates: Array<{ path: string; mtimeMs: number }> = [];
    const today = new Date();
    for (let dayOffset = 0; dayOffset < 90; dayOffset++) {
      const d = new Date(today.getTime() - dayOffset * 86400_000);
      const dir = join(
        this.sessionsRoot,
        String(d.getFullYear()),
        String(d.getMonth() + 1).padStart(2, '0'),
        String(d.getDate()).padStart(2, '0'),
      );
      let files: string[];
      try {
        files = await readdir(dir);
      } catch {
        continue;
      }
      for (const f of files) {
        if (!f.startsWith('rollout-') || !f.endsWith('.jsonl')) continue;
        const full = join(dir, f);
        let st;
        try { st = await stat(full); } catch { continue; }
        candidates.push({ path: full, mtimeMs: st.mtimeMs });
      }
    }
    candidates.sort((a, b) => b.mtimeMs - a.mtimeMs);

    /* Deny-list originators that aren't real user sessions. Match the
     * `originator` field from session_meta case-insensitively. */
    const isExcluded = (o: string | null) => {
      if (!o) return false;
      const lo = o.toLowerCase();
      return lo === 'claude code'
          || lo.startsWith('codex_cli')
          || lo.startsWith('codex-cli');
    };

    for (const c of candidates) {
      const originator = await this.readOriginator(c.path);
      if (isExcluded(originator)) continue;
      return c.path;
    }
    return null;
  }

  /** Peek the first JSONL line of a rollout to extract `session_meta.originator`.
   *  Uses a streaming readline iterator because session_meta lines can run
   *  to many KB (env_context bundles skills/prompts), and a fixed-size
   *  read was truncating them mid-string → JSON.parse failed → originator
   *  returned null → filter accepted everything (bug fixed 2026-05-25). */
  private async readOriginator(path: string): Promise<string | null> {
    const cached = this.originatorCache.get(path);
    if (cached !== undefined) return cached;
    let originator: string | null = null;
    try {
      const stream = createReadStream(path, { encoding: 'utf8' });
      const rl = createInterface({ input: stream, crlfDelay: Infinity });
      for await (const line of rl) {
        try {
          const obj = JSON.parse(line);
          const o = obj?.payload?.originator;
          if (typeof o === 'string') originator = o;
        } catch {
          /* ignore — fall through to close */
        }
        break; /* only the first line, regardless of parse outcome */
      }
      rl.close();
      stream.destroy();
    } catch {
      /* unreadable — treat as unknown, which the filter allows. */
    }
    this.originatorCache.set(path, originator);
    return originator;
  }

  private async switchToLatest(): Promise<void> {
    const path = await this.findLatestRollout();
    if (!path) {
      log.warn('rollout watcher: no rollout files found under ' + this.sessionsRoot);
      this.emit({ kind: 'status', state: 'idle', title: '暂无会话', body: 'Codex 还没有任何记录' });
      return;
    }
    if (this.current?.path === path) return; // already watching it
    if (this.current) log.info({ from: this.current.path, to: path }, 'switching rollout');
    await this.attachTo(path);
  }

  private async maybeSwitch(): Promise<void> {
    if (this.stopped) return;
    try {
      const path = await this.findLatestRollout();
      if (path && path !== this.current?.path) {
        log.info({ from: this.current?.path, to: path }, 'newer rollout detected, switching');
        await this.attachTo(path);
      }
    } catch (err) {
      log.warn({ err: String(err) }, 'maybeSwitch failed');
    }
  }

  // ---------- attach & tail ----------

  private async attachTo(path: string): Promise<void> {
    this.closeWatcher();
    this.current = { path, threadId: extractThreadId(path) };
    this.offset = 0;
    this.fileBuf = '';
    // Drop activity from the previous thread when we switch.
    this.activity = [];
    /* Clear thread-scoped session_info fields. Otherwise the old
     * rollout's thread_title leaks into the new thread until (or
     * unless) the new one explicitly publishes its own. cwd /
     * git_branch / source get rewritten during the next replay. */
    if (this.lastSessionInfo) {
      delete this.lastSessionInfo.thread_title;
    }

    // Replay existing content silently to seed metadata + activity buffer
    // without flashing every historical status/usage on the device's pet.
    // (onEventMsg auto-promotes entries <60 s old to live emissions, so
    // a task_started written between mtime-detection and our attach
    // still reaches the device.)
    this.replayingHistory = true;
    this.emittedDuringReplay = false;
    await this.readToEnd();
    this.replayingHistory = false;

    // Emit a clean "now following" status — but only if nothing more
    // specific was already emitted during replay. Otherwise we'd clobber
    // a freshly-woken "thinking" with a stale "idle / 跟随会话".
    if (!this.emittedDuringReplay) {
      this.onEvent?.({
        kind: 'status',
        state: 'idle',
        title: '跟随会话',
        body: this.current.cwd?.split('/').slice(-1)[0] ?? this.current.source ?? undefined,
      });
    }
    if (this.current.modelContextWindow) {
      this.onEvent?.({
        kind: 'usage',
        session: { model_context_window: this.current.modelContextWindow },
      });
    }

    // Now tail.
    try {
      this.fsWatcher = watch(path, { persistent: false }, (eventType) => {
        if (eventType === 'change' || eventType === 'rename') void this.readToEnd();
      });
    } catch (err) {
      log.warn({ err: String(err), path }, 'fs.watch failed; falling back to interval poll');
    }
    log.info(
      { path, threadId: this.current.threadId, source: this.current.source, cwd: this.current.cwd, replayed: this.activity.length },
      'rollout watcher attached',
    );
  }

  private closeWatcher(): void {
    if (this.fsWatcher) {
      try { this.fsWatcher.close(); } catch { /* ignore */ }
      this.fsWatcher = null;
    }
  }

  /**
   * Read every new byte from this.offset to EOF, emit events for complete
   * JSONL lines. Coalesces overlapping calls (fs.watch can fire many times
   * for a single append on macOS).
   */
  private async readToEnd(): Promise<void> {
    if (this.readingTail) {
      this.pendingRead = true;
      return;
    }
    this.readingTail = true;
    try {
      if (!this.current) return;
      const path = this.current.path;
      const st = await stat(path).catch(() => null);
      if (!st || st.size < this.offset) {
        // file truncated/rotated — restart from beginning
        this.offset = 0;
        this.fileBuf = '';
      }
      if (!st) return;
      if (st.size === this.offset) return;

      const start = this.offset;
      const end = st.size;
      this.offset = end;

      await new Promise<void>((resolve, reject) => {
        const stream = createReadStream(path, { start, end: end - 1, encoding: 'utf8' });
        const rl = createInterface({ input: stream, crlfDelay: Infinity });
        let lastLine = '';
        rl.on('line', (line) => {
          if (lastLine) this.handleLine(this.fileBuf + lastLine);
          this.fileBuf = '';
          lastLine = line;
        });
        rl.on('close', () => {
          // The final piece may be incomplete (no \n yet) — keep it buffered.
          // We can't tell trivially whether it ends with \n, so we treat the
          // last "line" emitted by readline as complete when the file ends
          // at a newline. Heuristic: if buf had content and the file size
          // didn't end on \n, save it as partial.
          // We err on safe side: try to parse, if it fails keep buffering.
          if (lastLine) {
            if (looksLikeCompleteJson(lastLine)) {
              this.handleLine(this.fileBuf + lastLine);
              this.fileBuf = '';
            } else {
              this.fileBuf = this.fileBuf + lastLine;
            }
          }
          resolve();
        });
        rl.on('error', reject);
      });
    } catch (err) {
      log.warn({ err: String(err) }, 'rollout tail read failed');
    } finally {
      this.readingTail = false;
      if (this.pendingRead) {
        this.pendingRead = false;
        void this.readToEnd();
      }
    }
  }

  // ---------- line → BridgeEvent ----------

  private handleLine(line: string): void {
    if (!line.trim()) return;
    let entry: RolloutEntry;
    try { entry = JSON.parse(line) as RolloutEntry; }
    catch { return; }
    if (!entry || typeof entry !== 'object') return;

    switch (entry.type) {
      case 'session_meta':
        this.onSessionMeta(entry.payload as SessionMetaPayload);
        return;
      case 'event_msg':
        this.onEventMsg(entry.payload as EventMsgPayload, entry.timestamp);
        return;
      case 'turn_context':
        this.onTurnContext(entry.payload as TurnContextPayload);
        return;
      case 'response_item':
        return;
    }
  }

  private onSessionMeta(p: SessionMetaPayload): void {
    if (!this.current) return;
    if (p.cwd) this.current.cwd = p.cwd;
    if (p.originator) this.current.source = p.originator;
    if (p.id && !this.current.threadId) this.current.threadId = p.id;

    this.emitSessionInfoMerge({
      ...(p.id ? { session_id: p.id } : {}),
      ...(p.originator ? { source: p.originator } : {}),
      ...(p.cwd ? { cwd: p.cwd } : {}),
      ...(p.cli_version ? { cli_version: p.cli_version } : {}),
      ...(p.git?.branch ? { git_branch: p.git.branch } : {}),
    });
    // Async: check AGENTS.md once per attached rollout.
    if (p.cwd) void this.checkAgentsMd(p.cwd);
  }

  private onTurnContext(p: TurnContextPayload): void {
    const sandboxKind = p.sandbox_policy?.type;       // "read-only" | "workspace-write" | ...
    const approval = p.approval_policy;               // "on-request" | "untrusted" | ...
    const permsLabel =
      sandboxKind === 'workspace-write' ? `Workspace (${approval ?? ''})`.trim()
        : sandboxKind === 'read-only' ? `Read-only (${approval ?? ''})`.trim()
        : sandboxKind ?? '—';

    this.emitSessionInfoMerge({
      ...(p.model ? { model: p.model } : {}),
      ...(p.effort ? { reasoning_effort: p.effort } : {}),
      ...(p.summary ? { summary_mode: p.summary } : {}),
      ...(p.cwd ? { cwd: p.cwd } : {}),
      ...(approval ? { approval_policy: approval } : {}),
      ...(sandboxKind ? { sandbox: sandboxKind } : {}),
      permissions: permsLabel,
      ...(p.collaboration_mode?.mode ? { collaboration_mode: p.collaboration_mode.mode } : {}),
    });
  }

  private async checkAgentsMd(cwd: string): Promise<void> {
    try {
      await access(join(cwd, 'AGENTS.md'), fsConstants.F_OK);
      this.emitSessionInfoMerge({ agents_md: true });
    } catch {
      this.emitSessionInfoMerge({ agents_md: false });
    }
  }

  private emitSessionInfoMerge(partial: SessionInfoLite): void {
    if (!partial || Object.keys(partial).length === 0) return;
    this.lastSessionInfo = { ...(this.lastSessionInfo ?? {}), ...partial };
    this.emit({ kind: 'session_info', ...this.lastSessionInfo });
  }

  private onEventMsg(p: EventMsgPayload, entryTs?: string): void {
    if (!p || !p.type) return;
    /* If we're in the "silent replay" window of attachTo, normally status
     * events are suppressed to avoid flashing every historic state on
     * the device. But if this entry happened within the last 60 s, it's
     * almost certainly a live conversation the user just started (typical
     * race: user types a prompt → codex writes task_started → bridge's
     * mtime poller picks the file 1-5 s later → attachTo starts replaying
     * and the user's task_started lands during replay). Promoting recent
     * events to live emissions makes the device wake up as the user
     * expects, without re-introducing the multi-day-old replay flicker. */
    if (this.replayingHistory && entryTs) {
      const tsMs = Date.parse(entryTs);
      if (!Number.isNaN(tsMs) && Date.now() - tsMs < 60_000) {
        /* Downgrade replay → live for this entry and any subsequent
         * entries (JSONL is chronological, so anything after is at
         * least as recent). */
        this.replayingHistory = false;
        this.emittedDuringReplay = true;
      }
    }
    switch (p.type) {
      case 'thread_name_updated': {
        /* Codex auto-summarises the active thread (sometimes after the
         * first user message lands) and emits a `thread_name_updated`
         * event. Surface it as `thread_title` so the device's title bar
         * shows what Codex thinks the task is about, instead of staying
         * blank between `task_started`/`task_complete`. */
        const name = (p as { thread_name?: string }).thread_name;
        if (name) {
          this.emitSessionInfoMerge({ thread_title: name });
        }
        return;
      }
      case 'task_started':
        if (p.model_context_window) {
          this.current && (this.current.modelContextWindow = p.model_context_window);
          this.emit({
            kind: 'usage',
            session: { model_context_window: p.model_context_window },
          });
        }
        this.emit({
          kind: 'status',
          state: 'thinking',
          title: 'Codex 思考中',
        });
        return;
      case 'user_message': {
        const text = extractText(p);
        this.appendActivity({
          ts: entryTs ?? new Date().toISOString(),
          kind: 'user',
          text,
          thread_id: this.current?.threadId ?? '',
          source: this.current?.source,
        });
        /* Fallback thread title: not every rollout fires
         * `thread_name_updated` (Codex Desktop only auto-summarises
         * sometimes). Without a title push, the device's top label
         * sits at "-" forever. Seed it from the first user_message
         * we see in the thread — the user's own prompt is the best
         * human-readable label we have. */
        if (text && !this.lastSessionInfo?.thread_title) {
          const seeded = text.length > 28 ? text.slice(0, 28) + '…' : text;
          this.emitSessionInfoMerge({ thread_title: seeded });
        }
        this.emit({
          kind: 'status',
          state: 'thinking',
          title: '用户提问',
          body: clip(text, 60),
        });
        return;
      }
      case 'agent_message': {
        const text = extractText(p);
        this.appendActivity({
          ts: entryTs ?? new Date().toISOString(),
          kind: 'agent',
          text,
          thread_id: this.current?.threadId ?? '',
          source: this.current?.source,
        });
        this.emit({
          kind: 'status',
          state: 'done',
          title: 'Codex 已回复',
          body: clip(text, 80),
        });
        return;
      }
      case 'task_complete':
        this.appendActivity({
          ts: entryTs ?? new Date().toISOString(),
          kind: 'task_complete',
          thread_id: this.current?.threadId ?? '',
          source: this.current?.source,
        });
        this.emit({
          kind: 'status',
          state: 'done',
          title: '任务完成',
        });
        return;
      case 'token_count': {
        const info = p.info ?? {};
        const totalUsage = info.total_token_usage ?? {};
        const lastUsage = info.last_token_usage ?? {};
        const ctxWindow = info.model_context_window;
        if (ctxWindow && this.current) this.current.modelContextWindow = ctxWindow;

        // Current context occupancy = the most recent turn's input + output.
        // Input includes all conversation history sent to the model; output
        // is what the model generated. Their sum approximates what will be
        // resident in the context for the next turn.
        const currentCtx =
          lastUsage.input_tokens !== undefined || lastUsage.output_tokens !== undefined
            ? (lastUsage.input_tokens ?? 0) + (lastUsage.output_tokens ?? 0)
            : undefined;

        const session = {
          ...(totalUsage.input_tokens !== undefined ? { input_tokens: totalUsage.input_tokens } : {}),
          ...(totalUsage.output_tokens !== undefined ? { output_tokens: totalUsage.output_tokens } : {}),
          ...(totalUsage.cached_input_tokens !== undefined ? { cached_tokens: totalUsage.cached_input_tokens } : {}),
          ...(totalUsage.total_tokens !== undefined ? { total_tokens: totalUsage.total_tokens } : {}),
          ...(currentCtx !== undefined ? { current_context_tokens: currentCtx } : {}),
          ...(ctxWindow !== undefined
            ? { model_context_window: ctxWindow }
            : this.current?.modelContextWindow
              ? { model_context_window: this.current.modelContextWindow }
              : {}),
        };

        // rate_limits is delivered alongside token_count in rollout — extract
        // plan_type + per-window usage so the device's left screen has it.
        const rl = p.rate_limits;
        const limits = rl
          ? [
              rl.primary ? buildLimit('primary', rl.primary) : null,
              rl.secondary ? buildLimit('secondary', rl.secondary) : null,
            ].filter((x): x is UsageLimitOut => x !== null)
          : undefined;

        this.emit({
          kind: 'usage',
          session,
          ...(limits && limits.length ? { limits } : {}),
          ...(rl?.plan_type ? { plan_type: rl.plan_type } : {}),
        });
        return;
      }
      // ignore everything else (tool_call_*, exec_start_failed, etc.)
    }
  }

  private appendActivity(entry: ActivityEntry): void {
    this.activity.push(entry);
    while (this.activity.length > ACTIVITY_BUFFER_SIZE) this.activity.shift();
  }

  // ---------- helpers ----------

  private emit(e: BridgeEvent): void {
    if (this.stopped) return;
    if (e.kind === 'usage') {
      // Cache the merged latest snapshot so a freshly-connected device
      // gets the current numbers immediately, not after the next turn.
      this.lastUsage = {
        session: { ...(this.lastUsage?.session ?? {}), ...(e.session ?? {}) },
        limits: e.limits ?? this.lastUsage?.limits,
        plan_type: e.plan_type ?? this.lastUsage?.plan_type,
      };
    }
    // During replay, suppress status flicker but still cache + forward usage / session_info.
    if (this.replayingHistory && e.kind === 'status') return;
    this.onEvent?.(e);
  }
}

interface UsagePayloadLite {
  session?: {
    input_tokens?: number;
    output_tokens?: number;
    cached_tokens?: number;
    total_tokens?: number;
    current_context_tokens?: number;
    model_context_window?: number;
  };
  limits?: UsageLimitOut[];
  plan_type?: string;
}

// ---------- helpers ----------

function extractThreadId(path: string): string {
  /* Anchor at the basename so directories containing "rollout-" in their
   * name (e.g. an OS tempdir like /tmp/doudou-rollout-XXXX) don't seed
   * the match in the wrong place. Without the anchor + basename, the
   * captured group would silently absorb path segments. */
  const m = basename(path).match(/^rollout-[^-]+-[^-]+-[^-]+-[^-]+-([0-9a-f-]+)\.jsonl$/);
  return m?.[1] ?? path;
}

function looksLikeCompleteJson(s: string): boolean {
  const t = s.trim();
  if (!t.startsWith('{')) return false;
  let depth = 0;
  let inStr = false;
  let esc = false;
  for (let i = 0; i < t.length; i++) {
    const c = t[i];
    if (esc) { esc = false; continue; }
    if (c === '\\') { esc = true; continue; }
    if (c === '"') { inStr = !inStr; continue; }
    if (inStr) continue;
    if (c === '{') depth++;
    else if (c === '}') depth--;
  }
  return depth === 0 && t.endsWith('}');
}

function clip(s: string, n: number): string {
  if (s.length <= n) return s;
  return s.slice(0, n - 1) + '…';
}

function extractText(p: EventMsgPayload): string {
  // payloads vary by shape — try a few common fields
  if (typeof p.message === 'string') return p.message;
  if (typeof p.text === 'string') return p.text;
  if (typeof p.content === 'string') return p.content;
  if (Array.isArray(p.content)) {
    return p.content
      .map((c) => (typeof c === 'string' ? c : (c?.text ?? '')))
      .join('')
      .trim();
  }
  return '';
}

// ---------- minimal typings for rollout JSONL ----------

interface RolloutEntry {
  timestamp?: string;
  type?: string;
  payload?: unknown;
}

interface SessionMetaPayload {
  id?: string;
  cwd?: string;
  originator?: string;
  cli_version?: string;
  git?: { branch?: string; commit_hash?: string };
}

interface TurnContextPayload {
  model?: string;
  effort?: string;
  summary?: string;
  cwd?: string;
  approval_policy?: string;
  sandbox_policy?: { type?: string };
  collaboration_mode?: { mode?: string };
  personality?: string;
}

interface SessionInfoLite {
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

interface TokenUsageBucket {
  input_tokens?: number;
  output_tokens?: number;
  cached_input_tokens?: number;
  reasoning_output_tokens?: number;
  total_tokens?: number;
}

interface TokenCountInfo {
  total_token_usage?: TokenUsageBucket;
  last_token_usage?: TokenUsageBucket;
  model_context_window?: number;
}

interface EventMsgPayload {
  type?: string;
  // common fields
  message?: string;
  text?: string;
  content?: string | Array<{ text?: string } | string>;
  // task_started
  model_context_window?: number;
  // token_count
  info?: TokenCountInfo;
  rate_limits?: RateLimits;
}
