/**
 * Abstraction over the source of Codex events.
 *
 * MVP-0 ships a `StubCodexFeed` that scripts canned events so the device
 * connection / UI / risk policy / audit code can be developed without a
 * running `codex app-server`. The real adapter lands in a follow-up
 * iteration once we can sit the bridge in front of `codex app-server`
 * over stdio.
 */
import { randomUUID } from 'node:crypto';

import type {
  ActionType,
  Risk,
  State,
} from '@doudou/device-protocol';

interface UsageLimit {
  id: string;
  label?: string;
  group_label?: string;
  used_pct?: number;
  window_minutes?: number;
  resets_at?: number;
}

/** Normalized event the bridge router consumes. Mirrors what the device sees,
 *  minus envelope fields (seq/ts/v) that the per-device connection assigns. */
export type BridgeEvent =
  | {
      kind: 'status';
      state: State;
      title: string;
      body?: string;
    }
  | {
      kind: 'question';
      id: string;
      risk: Risk;
      action_type: ActionType;
      title: string;
      body?: string;
      choices: Array<{ id: string; label: string }>;
      expires_in_ms: number;
      require_confirm: boolean;
    }
  | {
      kind: 'usage';
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
  | {
      kind: 'session_info';
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
  | {
      kind: 'thread_list';
      threads: Array<{
        id: string;
        title: string;
        source?: string;
        active?: boolean;
        updated_at?: number;
      }>;
    };

/** A reply we forward back to the Codex side. */
export interface ReplyDecision {
  request_id: string;
  choice_id: string;
}

export interface CodexFeed {
  start(handlers: {
    onEvent: (e: BridgeEvent) => void;
  }): Promise<void>;
  acceptReply(decision: ReplyDecision): Promise<void>;
  stop(): Promise<void>;
}

// ---------- Stub ----------

interface ScriptStep {
  delayMs: number;
  event: BridgeEvent;
}

const SCRIPT: ScriptStep[] = [
  {
    delayMs: 500,
    event: { kind: 'status', state: 'thinking', title: 'Codex 启动中', body: '加载会话上下文' },
  },
  {
    delayMs: 3000,
    event: { kind: 'status', state: 'executing', title: '正在分析项目', body: '扫描文件结构' },
  },
  {
    delayMs: 5000,
    event: {
      kind: 'question',
      id: 'demo_low_1',
      risk: 'low',
      action_type: 'run_command',
      title: '运行 git status?',
      body: '只读检查工作区状态',
      choices: [
        { id: 'accept', label: '同意' },
        { id: 'decline', label: '拒绝' },
      ],
      expires_in_ms: 60_000,
      require_confirm: false,
    },
  },
  {
    delayMs: 5000,
    event: { kind: 'status', state: 'thinking', title: 'Codex 思考中' },
  },
  {
    delayMs: 6000,
    event: {
      kind: 'question',
      id: 'demo_high_1',
      risk: 'high',
      action_type: 'run_command',
      title: '执行 rm -rf?',
      body: 'Codex 想清理 build 目录,此操作不可逆',
      choices: [
        { id: 'accept', label: '同意' },
        { id: 'decline', label: '拒绝' },
      ],
      expires_in_ms: 45_000,
      require_confirm: true,
    },
  },
  {
    delayMs: 3000,
    event: { kind: 'status', state: 'done', title: '任务完成', body: '所有变更已提交' },
  },
];

export class StubCodexFeed implements CodexFeed {
  private timer: ReturnType<typeof setTimeout> | null = null;
  private replyResolvers = new Map<string, (choice: string) => void>();
  private onEvent: ((e: BridgeEvent) => void) | null = null;

  async start(handlers: { onEvent: (e: BridgeEvent) => void }): Promise<void> {
    this.onEvent = handlers.onEvent;
    this.scheduleLoop();
  }

  async acceptReply(decision: ReplyDecision): Promise<void> {
    const resolve = this.replyResolvers.get(decision.request_id);
    if (resolve) {
      this.replyResolvers.delete(decision.request_id);
      resolve(decision.choice_id);
    }
  }

  async stop(): Promise<void> {
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    this.replyResolvers.clear();
  }

  private scheduleLoop(idx = 0): void {
    if (!this.onEvent) return;
    const step = SCRIPT[idx % SCRIPT.length];
    if (!step) return;
    this.timer = setTimeout(() => {
      // freshen question IDs each loop so the device sees distinct requests
      const event =
        step.event.kind === 'question'
          ? { ...step.event, id: `${step.event.id}_${randomUUID().slice(0, 8)}` }
          : step.event;
      this.onEvent?.(event);
      if (event.kind === 'question') {
        void new Promise<string>((resolve) => {
          this.replyResolvers.set(event.id, resolve);
        });
      }
      this.scheduleLoop(idx + 1);
    }, step.delayMs);
  }
}
