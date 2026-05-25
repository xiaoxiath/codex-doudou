import { describe, expect, it } from 'vitest';

import {
  parseFromBridge,
  parseFromDevice,
  PROTOCOL_VERSION,
} from './index.js';

const envelope = { v: PROTOCOL_VERSION, seq: 1, ts: 0 };

describe('parseFromBridge — new message types', () => {
  it('accepts session_info with all current fields', () => {
    const r = parseFromBridge({
      ...envelope,
      type: 'session_info',
      session_id: '019e4a99-...',
      thread_title: '修复 Codex 桌面版启动缓慢',
      source: 'Codex Desktop',
      model: 'gpt-5.5',
      reasoning_effort: 'xhigh',
      summary_mode: 'none',
      cwd: '/Users/me/proj',
      permissions: 'Workspace (on-request)',
      account_email: 'a@b.com',
      plan_type: 'pro',
      agents_md: true,
      git_branch: 'main',
    });
    expect(r.ok).toBe(true);
    if (r.ok && r.value.type === 'session_info') {
      expect(r.value.thread_title).toBe('修复 Codex 桌面版启动缓慢');
      expect(r.value.agents_md).toBe(true);
    } else {
      throw new Error('unexpected');
    }
  });

  it('accepts thread_list', () => {
    const r = parseFromBridge({
      ...envelope,
      type: 'thread_list',
      threads: [
        { id: 'a', title: 'first', source: 'vscode', active: true, updated_at: 1000 },
        { id: 'b', title: 'second' },
      ],
    });
    expect(r.ok).toBe(true);
    if (r.ok && r.value.type === 'thread_list') {
      expect(r.value.threads).toHaveLength(2);
      expect(r.value.threads[0]?.active).toBe(true);
    }
  });

  it('accepts usage with multi-model group_label', () => {
    const r = parseFromBridge({
      ...envelope,
      type: 'usage',
      session: { total_tokens: 100, current_context_tokens: 50, model_context_window: 1000 },
      limits: [
        { id: 'codex_primary', label: '5h', used_pct: 5 },
        { id: 'spark_primary', label: '5h', group_label: 'GPT-5.3-Codex-Spark', used_pct: 0 },
      ],
      plan_type: 'pro',
    });
    expect(r.ok).toBe(true);
    if (r.ok && r.value.type === 'usage') {
      expect(r.value.session?.current_context_tokens).toBe(50);
      expect(r.value.limits?.[1]?.group_label).toBe('GPT-5.3-Codex-Spark');
    }
  });

  it('accepts question with queue_total', () => {
    const r = parseFromBridge({
      ...envelope,
      type: 'question',
      id: 'q1',
      risk: 'high',
      action_type: 'run_command',
      title: 'run x',
      choices: [{ id: 'a', label: 'A' }],
      expires_at: 9999,
      require_confirm: true,
      queue_total: 3,
    });
    expect(r.ok).toBe(true);
    if (r.ok && r.value.type === 'question') {
      expect(r.value.queue_total).toBe(3);
    }
  });

  it('rejects malformed payloads', () => {
    const r = parseFromBridge({ v: PROTOCOL_VERSION, seq: 1, ts: 0, type: 'session_info', agents_md: 'yes-please' });
    expect(r.ok).toBe(false);
  });

  it('rejects mismatched protocol version', () => {
    const r = parseFromBridge({
      v: 99,
      type: 'status',
      seq: 1,
      ts: 0,
      state: 'idle',
      title: 'x',
    });
    expect(r.ok).toBe(false);
  });
});

describe('parseFromDevice — follow_thread', () => {
  it('accepts a valid follow_thread', () => {
    const r = parseFromDevice({
      ...envelope,
      type: 'follow_thread',
      thread_id: '019e4a99',
    });
    expect(r.ok).toBe(true);
    if (r.ok && r.value.type === 'follow_thread') {
      expect(r.value.thread_id).toBe('019e4a99');
    }
  });

  it('rejects empty thread_id', () => {
    const r = parseFromDevice({ ...envelope, type: 'follow_thread', thread_id: '' });
    expect(r.ok).toBe(false);
  });
});
