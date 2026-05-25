/**
 * CodexFeed backed by a real `codex app-server` subprocess.
 *
 * Owns the lifecycle: spawn → initialize → thread/start → relay events.
 *
 * Translates Codex events into Bridge events:
 *   thread/status/changed | turn/started | turn/completed  →  status
 *   item/started (command / file_change / tool_call)        →  (no UI yet — see audit)
 *   item/commandExecution/requestApproval (server request)  →  question (risk-classified)
 *   item/fileChange/requestApproval         (server request)→  question
 *   item/tool/requestUserInput              (server request)→  question
 *
 * The promise returned to Codex for each *requestApproval is held open until
 * the bridge calls `acceptReply()` with the device's decision.
 */
import { randomUUID } from 'node:crypto';

import { log } from '../log.js';
import {
  classifyCommand,
  classifyFileChange,
  classifyToolCall,
  type RiskDecision,
} from '../risk.js';
import { JsonRpcStdioClient } from './jsonRpcClient.js';
import type { BridgeEvent, CodexFeed, ReplyDecision } from '../codexFeed.js';

// Local alias matching device-protocol UsageLimit (lifted here to avoid extra import noise).
type UsageLimitOut = {
  id: string;
  label?: string;
  used_pct?: number;
  window_minutes?: number;
  resets_at?: number;
};

// ---------- shapes we care about (intentionally loose to ride codex version drift) ----------

interface ThreadStartResult {
  threadId?: string;
  thread?: { id?: string };
}

interface ThreadSummary {
  id: string;
  ephemeral?: boolean;
  preview?: string | null;
  updatedAt?: number;
  cwd?: string | null;
  source?: string | null;
  path?: string | null;
  status?: { type?: string } | null;
}

interface ThreadListResult {
  data?: ThreadSummary[];
}

interface ThreadResumeResult {
  thread?: ThreadSummary;
}

interface CommandApprovalParams {
  threadId: string;
  turnId?: string;
  itemId?: string;
  approvalId?: string | null;
  reason?: string | null;
  /** Codex hands us a full shell command line as a single string, e.g. "git status" or "/bin/zsh -lc 'git status'". */
  command?: string | null;
  cwd?: string | null;
  /** Best-effort parsed actions; each .command is a *string* not an array. */
  commandActions?: Array<{ command?: string | null; cmd?: string | null; type?: string }>;
}

interface FileChangeApprovalParams {
  threadId: string;
  itemId?: string;
  reason?: string | null;
}

interface ToolInputParams {
  threadId: string;
  itemId?: string;
  questions?: Array<{ text?: string; prompt?: string }>;
}

interface ThreadStatusChangedParams {
  threadId?: string;
  status?: string; // notLoaded | idle | active | systemError
}

interface TurnStartedParams {
  threadId?: string;
  turnId?: string;
}
interface TurnCompletedParams {
  threadId?: string;
  turnId?: string;
  status?: 'completed' | 'interrupted' | 'failed';
  error?: { message?: string };
}

interface ItemStartedParams {
  threadId?: string;
  itemId?: string;
  item?: { type?: string; [k: string]: unknown };
}

interface TokenUsageNotifParams {
  threadId?: string;
  tokenUsage?: {
    last?: { inputTokens?: number; outputTokens?: number; cachedInputTokens?: number; totalTokens?: number };
    total?: { inputTokens?: number; outputTokens?: number; cachedInputTokens?: number; totalTokens?: number };
    modelContextWindow?: number;
  };
}

interface RateLimitsNotifParams {
  rateLimits?: {
    primary?: { usedPercent?: number; windowDurationMins?: number; resetsAt?: number } | null;
    secondary?: { usedPercent?: number; windowDurationMins?: number; resetsAt?: number } | null;
    planType?: string | null;
  };
}

interface ItemCompletedParams {
  threadId?: string;
  itemId?: string;
  item?: {
    type?: string;
    text?: string;
    phase?: string;
    status?: string;
    exitCode?: number;
    [k: string]: unknown;
  };
}

// ---------- options ----------

export interface RealCodexFeedOptions {
  /** Absolute path to the codex binary. Defaults to "codex" from PATH. */
  codexBin?: string;
  /** Extra args to pass to `codex app-server`. */
  extraArgs?: string[];
  /** Working directory passed in thread/start; defaults to process.cwd(). */
  workspace?: string;
  /** Approval policy — see codex docs. Defaults to "on-request". */
  approvalPolicy?: 'untrusted' | 'on-failure' | 'on-request' | 'never';
  /**
   * Source mode:
   *  - `follow`  : list all persistent threads (incl. Codex Desktop / VS Code),
   *                resume the most recently-updated one, switch automatically
   *                as the user opens new threads elsewhere. We never call
   *                turn/start on followed threads — the host app drives them.
   *  - `own`     : start our own thread; /api/prompt drives turns. We will be
   *                the only client of this thread; we get approval routing.
   *  - `auto`    : try follow, fall back to own if list is empty.
   * Default: `auto`.
   */
  source?: 'follow' | 'own' | 'auto';
  /** How often to poll thread/list to detect new desktop threads (ms). */
  pollIntervalMs?: number;
}

interface PendingApproval {
  /** "decision" payload the bridge must hand back to Codex. */
  resolve: (decision: unknown) => void;
  /** Maps choice_id from the device → Codex decision. */
  toDecision: (choiceId: string) => unknown;
  kind: 'command' | 'file_change' | 'tool_input';
}

const HIGH_RISK_TIMEOUT_DECISION = 'cancel';
const DEFAULT_TIMEOUT_DECISION = 'decline';

export class RealCodexFeed implements CodexFeed {
  private rpc: JsonRpcStdioClient | null = null;
  private threadId: string | null = null;
  private threadSource: 'own' | 'follow' | null = null;
  private onEvent: ((e: BridgeEvent) => void) | null = null;
  private pendingApprovals = new Map<string, PendingApproval>();
  private pollTimer: ReturnType<typeof setInterval> | null = null;

  constructor(private readonly opts: RealCodexFeedOptions = {}) {}

  async start(handlers: { onEvent: (e: BridgeEvent) => void }): Promise<void> {
    this.onEvent = handlers.onEvent;

    const bin = this.opts.codexBin ?? 'codex';
    const args = ['app-server', ...(this.opts.extraArgs ?? [])];

    const rpc = new JsonRpcStdioClient(bin, args);
    this.rpc = rpc;
    await rpc.start();

    this.wireHandlers(rpc);

    log.info({ bin, args }, 'codex app-server starting');

    const initResult = await rpc.request<{ userAgent?: string; codexHome?: string }>(
      'initialize',
      {
        clientInfo: { name: 'doudou-bridge', version: '0.1.0' },
        capabilities: null,
      },
    );
    log.info({ codexHome: initResult.codexHome, userAgent: initResult.userAgent }, 'codex initialized');
    rpc.notify('initialized');

    const mode = this.opts.source ?? 'auto';
    if (mode === 'own') {
      await this.startOwnThread();
    } else {
      const followed = await this.tryFollowLatest();
      if (followed) {
        this.startPollingForNewThreads();
      } else if (mode === 'auto') {
        log.info('no followable thread found, falling back to own thread mode');
        await this.startOwnThread();
      } else {
        log.warn('source=follow but no thread to follow; idle');
        this.emitStatus('idle', '没有可跟随的会话', '在 Desktop 里发起一段对话');
      }
    }
  }

  // ---------- thread source strategies ----------

  private async startOwnThread(): Promise<void> {
    const startResult = await this.rpc!.request<ThreadStartResult>('thread/start', {
      cwd: this.opts.workspace ?? process.cwd(),
      approvalPolicy: this.opts.approvalPolicy ?? 'on-request',
    });
    const tid = startResult.threadId ?? startResult.thread?.id ?? null;
    if (!tid) throw new Error(`thread/start returned no threadId: ${JSON.stringify(startResult)}`);
    this.threadId = tid;
    this.threadSource = 'own';
    log.info({ threadId: tid }, 'codex thread started (own mode)');
    this.emitStatus('idle', 'Codex 已就绪', '从输入框给我提示');
  }

  private async tryFollowLatest(): Promise<boolean> {
    const target = await this.findLatestFollowableThread();
    if (!target) return false;
    await this.resumeThread(target);
    return true;
  }

  private async findLatestFollowableThread(): Promise<ThreadSummary | null> {
    const list = await this.rpc!.request<ThreadListResult>('thread/list', { limit: 20 });
    const candidates = (list.data ?? []).filter(
      (t) => !t.ephemeral && t.updatedAt !== undefined,
    );
    candidates.sort((a, b) => (b.updatedAt ?? 0) - (a.updatedAt ?? 0));
    return candidates[0] ?? null;
  }

  private async resumeThread(target: ThreadSummary): Promise<void> {
    const r = await this.rpc!.request<ThreadResumeResult>('thread/resume', { threadId: target.id });
    this.threadId = target.id;
    this.threadSource = 'follow';
    log.info(
      {
        threadId: target.id,
        preview: target.preview?.slice(0, 40),
        cwd: target.cwd,
        source: target.source,
        status: r.thread?.status?.type,
      },
      'following thread',
    );
    this.emitStatus(
      'idle',
      `跟随 ${target.source ?? '会话'}`,
      target.preview ? target.preview.slice(0, 30) : undefined,
    );
  }

  private startPollingForNewThreads(): void {
    const interval = this.opts.pollIntervalMs ?? 5_000;
    this.pollTimer = setInterval(() => {
      void this.pollOnce();
    }, interval);
    this.pollTimer.unref?.();
  }

  private async pollOnce(): Promise<void> {
    if (!this.rpc || this.threadSource === 'own') return;
    try {
      const target = await this.findLatestFollowableThread();
      if (!target) return;
      if (target.id === this.threadId) return;
      log.info({ from: this.threadId, to: target.id }, 'switching followed thread');
      // Decline any pending approvals from old thread to avoid orphans.
      for (const [id, p] of this.pendingApprovals) {
        p.resolve({ decision: DEFAULT_TIMEOUT_DECISION });
        log.warn({ id }, 'auto-declined approval during thread switch');
      }
      this.pendingApprovals.clear();
      await this.resumeThread(target);
    } catch (err) {
      log.warn({ err: String(err) }, 'pollOnce failed');
    }
  }

  /** Inject a user prompt into the current thread — used by `/api/prompt`. */
  async sendPrompt(text: string): Promise<void> {
    if (!this.rpc || !this.threadId) throw new Error('codex not ready');
    if (this.threadSource === 'follow') {
      log.warn('sendPrompt called while following an external thread — will share with that thread');
    }
    this.emitStatus('thinking', 'Codex 正在思考', text.slice(0, 40));
    await this.rpc.request('turn/start', {
      threadId: this.threadId,
      input: [{ type: 'text', text, text_elements: [] }],
    });
  }

  async acceptReply(decision: ReplyDecision): Promise<void> {
    const pending = this.pendingApprovals.get(decision.request_id);
    if (!pending) {
      log.debug({ id: decision.request_id }, 'reply for unknown approval (already resolved)');
      return;
    }
    this.pendingApprovals.delete(decision.request_id);
    const codexDecision = pending.toDecision(decision.choice_id);
    log.info({ id: decision.request_id, choice: decision.choice_id, codexDecision }, 'forwarding decision to codex');
    pending.resolve({ decision: codexDecision });
  }

  async stop(): Promise<void> {
    if (this.pollTimer) {
      clearInterval(this.pollTimer);
      this.pollTimer = null;
    }
    if (!this.rpc) return;
    // Decline any pending approvals so codex doesn't hang on shutdown.
    for (const [id, pending] of this.pendingApprovals) {
      pending.resolve({ decision: DEFAULT_TIMEOUT_DECISION });
      log.warn({ id }, 'auto-declined approval on shutdown');
    }
    this.pendingApprovals.clear();
    await this.rpc.stop();
    this.rpc = null;
  }

  // ---------- handler wiring ----------

  private wireHandlers(rpc: JsonRpcStdioClient): void {
    // status changes / lifecycle
    rpc.onNotification('thread/status/changed', (p) => {
      const params = p as ThreadStatusChangedParams;
      const mapping: Record<string, ['idle' | 'thinking' | 'error', string]> = {
        idle: ['idle', '空闲'],
        active: ['thinking', 'Codex 工作中'],
        systemError: ['error', 'Codex 异常'],
      };
      const m = mapping[params.status ?? 'idle'];
      if (m) this.emitStatus(m[0], m[1]);
    });

    rpc.onNotification('turn/started', (p) => {
      const params = p as TurnStartedParams;
      log.debug({ turnId: params.turnId }, 'turn started');
      this.emitStatus('thinking', 'Codex 正在处理');
    });

    rpc.onNotification('turn/completed', (p) => {
      const params = p as TurnCompletedParams;
      if (params.status === 'completed') {
        this.emitStatus('done', '任务完成');
      } else if (params.status === 'interrupted') {
        this.emitStatus('idle', '已中断');
      } else if (params.status === 'failed') {
        this.emitStatus('error', '任务失败', params.error?.message?.slice(0, 60));
      }
    });

    rpc.onNotification('item/started', (p) => {
      const params = p as ItemStartedParams;
      const itemType = params.item?.type;
      if (itemType === 'commandExecution') {
        this.emitStatus('executing', '正在执行命令');
      } else if (itemType === 'fileChange') {
        this.emitStatus('executing', '正在修改文件');
      }
    });

    rpc.onNotification('thread/tokenUsage/updated', (p) => {
      const params = p as TokenUsageNotifParams;
      const t = params.tokenUsage?.total ?? params.tokenUsage?.last;
      const cw = params.tokenUsage?.modelContextWindow;
      if (!t && cw === undefined) return;
      this.onEvent?.({
        kind: 'usage',
        session: {
          ...(t?.inputTokens !== undefined ? { input_tokens: t.inputTokens } : {}),
          ...(t?.outputTokens !== undefined ? { output_tokens: t.outputTokens } : {}),
          ...(t?.cachedInputTokens !== undefined ? { cached_tokens: t.cachedInputTokens } : {}),
          ...(t?.totalTokens !== undefined ? { total_tokens: t.totalTokens } : {}),
          ...(cw !== undefined ? { model_context_window: cw } : {}),
        },
      });
    });

    rpc.onNotification('account/rateLimits/updated', (p) => {
      const params = p as RateLimitsNotifParams;
      const rl = params.rateLimits;
      if (!rl) return;
      const limits: UsageLimitOut[] = [];
      if (rl.primary) {
        limits.push({
          id: 'primary',
          label: rl.primary.windowDurationMins ? `${Math.round(rl.primary.windowDurationMins / 60)}h` : 'primary',
          used_pct: rl.primary.usedPercent,
          window_minutes: rl.primary.windowDurationMins,
          resets_at: rl.primary.resetsAt !== undefined ? rl.primary.resetsAt * 1000 : undefined,
        });
      }
      if (rl.secondary) {
        limits.push({
          id: 'secondary',
          label: rl.secondary.windowDurationMins
            ? `${Math.round(rl.secondary.windowDurationMins / 60 / 24)}d`
            : 'secondary',
          used_pct: rl.secondary.usedPercent,
          window_minutes: rl.secondary.windowDurationMins,
          resets_at: rl.secondary.resetsAt !== undefined ? rl.secondary.resetsAt * 1000 : undefined,
        });
      }
      this.onEvent?.({
        kind: 'usage',
        limits,
        ...(rl.planType ? { plan_type: rl.planType } : {}),
      });
    });

    rpc.onNotification('item/completed', (p) => {
      const params = p as ItemCompletedParams;
      const item = params.item;
      if (!item) return;
      // Codex doesn't always emit turn/completed — the agent's final answer
      // arrives as an item/completed with phase "final_answer". Use it as
      // the "turn done" signal so the device doesn't sit on "thinking".
      if (item.type === 'agentMessage' && item.phase === 'final_answer') {
        const text = typeof item.text === 'string' ? item.text.trim() : '';
        this.emitStatus('done', 'Codex 已回复', text || undefined);
      } else if (item.type === 'commandExecution' && item.status === 'completed') {
        // Brief flicker back to "thinking" so user knows we're not done with the turn
        this.emitStatus('thinking', '命令已完成');
      }
    });

    // Server-initiated approval requests — these are the headline integration.
    rpc.onServerRequest(
      'item/commandExecution/requestApproval',
      async (params) => this.handleCommandApproval(params as CommandApprovalParams),
    );
    rpc.onServerRequest(
      'item/fileChange/requestApproval',
      async (params) => this.handleFileChangeApproval(params as FileChangeApprovalParams),
    );
    rpc.onServerRequest('item/tool/requestUserInput', async (params) =>
      this.handleToolInput(params as ToolInputParams),
    );
    // Legacy approval methods (older codex versions used these).
    rpc.onServerRequest('execCommandApproval', async (params) =>
      this.handleCommandApproval(params as CommandApprovalParams),
    );
    rpc.onServerRequest('applyPatchApproval', async (params) =>
      this.handleFileChangeApproval(params as FileChangeApprovalParams),
    );

    rpc.setFallbackNotification((params) => {
      log.debug({ params }, 'unhandled codex notification');
    });
    rpc.setFallbackServerRequest(async (params, _id, method) => {
      log.warn({ method, params }, 'unhandled codex server request → declining');
      return { decision: DEFAULT_TIMEOUT_DECISION };
    });
  }

  // ---------- approval translators ----------

  private async handleCommandApproval(params: CommandApprovalParams): Promise<unknown> {
    const argv = this.commandToArgv(params);
    const decision = classifyCommand({
      command: argv,
      cwd: params.cwd ?? null,
      workspace: this.opts.workspace,
    });
    return this.askDevice({
      kind: 'command',
      reason: params.reason ?? undefined,
      decision,
      title: this.shortenCommand(argv),
      body: params.reason ?? `cwd: ${this.shortenCwd(params.cwd)}`,
      toDecision: commandChoiceToDecision,
    });
  }

  private async handleFileChangeApproval(params: FileChangeApprovalParams): Promise<unknown> {
    // Without the actual change diff we conservatively grade as medium.
    // Once codex hands us patch detail in params we can call classifyFileChange.
    const decision: RiskDecision = {
      risk: 'medium',
      action_type: 'modify_file',
      reason: 'file change requested',
    };
    return this.askDevice({
      kind: 'file_change',
      reason: params.reason ?? undefined,
      decision,
      title: '修改文件?',
      body: params.reason ?? '查看电脑端 diff 后决定',
      toDecision: fileChangeChoiceToDecision,
    });
  }

  private async handleToolInput(params: ToolInputParams): Promise<unknown> {
    const decision = classifyToolCall({ toolName: params.itemId ?? 'tool' });
    const firstQ = params.questions?.[0];
    const promptText = firstQ?.prompt ?? firstQ?.text ?? '需要确认';
    return this.askDevice({
      kind: 'tool_input',
      decision,
      title: '工具询问',
      body: promptText.slice(0, 60),
      toDecision: toolInputChoiceToDecision,
    });
  }

  private askDevice(args: {
    kind: PendingApproval['kind'];
    reason?: string;
    decision: RiskDecision;
    title: string;
    body?: string;
    toDecision: (choiceId: string) => unknown;
  }): Promise<unknown> {
    const id = `cdx_${randomUUID().slice(0, 12)}`;
    log.info(
      { id, risk: args.decision.risk, action: args.decision.action_type, why: args.decision.reason },
      'codex → device approval',
    );
    return new Promise<unknown>((resolve) => {
      this.pendingApprovals.set(id, {
        kind: args.kind,
        toDecision: args.toDecision,
        resolve,
      });
      this.onEvent?.({
        kind: 'question',
        id,
        risk: args.decision.risk,
        action_type: args.decision.action_type,
        title: clamp(args.title, 30),
        body: args.body ? clamp(args.body, 150) : undefined,
        choices: [
          { id: 'accept', label: '同意' },
          { id: 'decline', label: '拒绝' },
        ],
        expires_in_ms: args.decision.risk === 'high' ? 60_000 : 90_000,
        require_confirm: args.decision.risk === 'high',
      });
      // Self-timeout in case the device never replies AND the question
      // never expires through the device path.
      const fallback = setTimeout(
        () => {
          if (this.pendingApprovals.delete(id)) {
            log.warn({ id }, 'codex approval timed out, auto-resolving');
            const auto = args.decision.risk === 'high' ? HIGH_RISK_TIMEOUT_DECISION : DEFAULT_TIMEOUT_DECISION;
            resolve({ decision: auto });
          }
        },
        args.decision.risk === 'high' ? 90_000 : 120_000,
      );
      fallback.unref?.();
    });
  }

  private emitStatus(state: 'idle' | 'thinking' | 'executing' | 'done' | 'error' | 'waiting_input' | 'waiting_approval', title: string, body?: string): void {
    this.onEvent?.({ kind: 'status', state, title: clamp(title, 30), body: body ? clamp(body, 150) : undefined });
  }

  /**
   * Codex sends full shell command lines as strings. Wrap into a single-element
   * argv so risk.ts can do its own shell-aware classification.
   */
  private commandToArgv(p: CommandApprovalParams): string[] {
    const first = p.commandActions?.[0];
    const candidate =
      (typeof first?.command === 'string' && first.command) ||
      (typeof first?.cmd === 'string' && first.cmd) ||
      (typeof p.command === 'string' && p.command) ||
      '';
    if (!candidate) return ['<unknown>'];
    // Strip wrapping shell invocation (`/bin/zsh -lc '...'`) when present so
    // classifier sees the actual user-intent command.
    const m = candidate.match(/^\/(bin|usr\/bin)\/(?:ba)?sh\s+-l?c\s+(['"])(.*)\2\s*$/);
    return [m?.[3] ?? candidate];
  }

  private shortenCommand(argv: string[]): string {
    const joined = Array.isArray(argv) ? argv.join(' ') : String(argv);
    return clamp(joined.length > 0 ? joined : '<command>', 28);
  }
  private shortenCwd(cwd: string | null | undefined): string {
    if (!cwd) return '?';
    const parts = cwd.split('/').filter(Boolean);
    return parts.length > 2 ? '…/' + parts.slice(-2).join('/') : cwd;
  }
}

function clamp(s: string, byteBudget: number): string {
  const enc = new TextEncoder();
  if (enc.encode(s).length <= byteBudget) return s;
  // truncate by characters until fits; cheap loop, strings here are tiny.
  let out = s;
  while (enc.encode(out + '…').length > byteBudget && out.length > 1) {
    out = out.slice(0, -1);
  }
  return out + '…';
}

function commandChoiceToDecision(choiceId: string): unknown {
  if (choiceId === 'accept') return 'accept';
  if (choiceId === 'accept_session') return 'acceptForSession';
  return 'decline';
}
function fileChangeChoiceToDecision(choiceId: string): unknown {
  if (choiceId === 'accept') return 'accept';
  if (choiceId === 'accept_session') return 'acceptForSession';
  return 'decline';
}
function toolInputChoiceToDecision(choiceId: string): unknown {
  // Tool input responses use a more structured payload in some versions;
  // for MVP-0 we treat accept/decline as the user-visible answer.
  if (choiceId === 'accept') return { answer: 'accept' };
  return { answer: 'decline' };
}
