/**
 * RolloutWatcherFeed tests.
 *
 * Strategy: spin up a temp sessions-root with a hand-authored
 * rollout-*.jsonl, point the watcher at it, drive its `start()` and
 * assert the BridgeEvents it emits. Avoids fs.watch races by writing
 * the whole file first and letting the watcher's historical replay
 * path produce the events.
 */
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { RolloutWatcherFeed } from './rolloutWatcher.js';
import type { BridgeEvent } from '../codexFeed.js';

/** Build a YYYY/MM/DD/rollout-XXX-{uuid}.jsonl path inside `root` and write `entries`.
 *  Filename must match `extractThreadId`'s regex:
 *    rollout-<a>-<b>-<c>-<d>-<HEX-DASH UUID>.jsonl
 *  i.e. five hyphen-separated chunks after `rollout-`, last is the captured id. */
function writeRollout(root: string, threadIdHex: string, entries: object[]): string {
  const today = new Date();
  const y = String(today.getFullYear());
  const m = String(today.getMonth() + 1).padStart(2, '0');
  const d = String(today.getDate()).padStart(2, '0');
  const dir = join(root, y, m, d);
  mkdirSync(dir, { recursive: true });
  // Five chunks: rollout-YYYY-MM-DDThh-mmss-<id>.jsonl (synthetic but valid).
  const ts = `${y}-${m}-${d}T10-${Math.floor(Math.random() * 9999).toString().padStart(4, '0')}`;
  const file = join(dir, `rollout-${ts}-${threadIdHex}.jsonl`);
  writeFileSync(file, entries.map((e) => JSON.stringify(e)).join('\n') + '\n');
  return file;
}

/** Make a uuid-shaped thread id (hex + dashes) that survives the
 *  watcher's filename-derived threadId extraction. */
function uuid(label: string): string {
  // Cheap deterministic pad — turns "aaaa" into "aaaa1111-2222-3333-4444-5555aaaa".
  return `${label}1111-2222-3333-4444-5555${label}`;
}

/** Convenience: minimal session_meta envelope as the watcher expects. */
function sessionMeta(threadId: string, extra: Record<string, unknown> = {}) {
  return {
    type: 'session_meta',
    timestamp: new Date().toISOString(),
    payload: {
      id: threadId,
      cwd: '/tmp/demo',
      originator: 'codex-cli',
      cli_version: '0.131.0',
      ...extra,
    },
  };
}

function eventMsg(payloadType: string, payload: Record<string, unknown> = {}) {
  return {
    type: 'event_msg',
    timestamp: new Date().toISOString(),
    payload: { type: payloadType, ...payload },
  };
}

describe('RolloutWatcherFeed', () => {
  let root: string;
  let feed: RolloutWatcherFeed;
  let events: BridgeEvent[];

  beforeEach(() => {
    /* Note: NOT using 'rollout-' in the temp dir name, because the
     * filename-threadId extraction regex is anchored to basename. We've
     * fixed that in rolloutWatcher.ts so this no longer matters, but
     * keep the dir prefix clean anyway. */
    root = mkdtempSync(join(tmpdir(), 'doudou-watcher-'));
    events = [];
  });
  afterEach(async () => {
    if (feed) await feed.stop();
    rmSync(root, { recursive: true, force: true });
  });

  it('does not crash when sessions root is empty', async () => {
    feed = new RolloutWatcherFeed({ sessionsRoot: root });
    await feed.start({ onEvent: (e) => events.push(e) });
    // No rollout files → no thread attached. The watcher may emit a
    // benign initial status (idle/offline) which is fine — we only
    // assert it didn't pick up phantom data.
    expect(feed.getCurrentThreadId()).toBeUndefined();
    expect(feed.getLastSessionInfo()).toBeNull();
  });

  it('parses session_meta + emits a session_info event', async () => {
    const tid = uuid('aaaa');
    writeRollout(root, tid, [
      sessionMeta(tid, { cwd: '/tmp/proj', originator: 'vscode' }),
    ]);
    feed = new RolloutWatcherFeed({ sessionsRoot: root });
    await feed.start({ onEvent: (e) => events.push(e) });

    const si = events.find((e) => e.kind === 'session_info');
    expect(si).toBeDefined();
    expect(feed.getCurrentThreadId()).toBe(tid);
    expect(feed.getLastSessionInfo()).toMatchObject({
      session_id: tid,
      cwd: '/tmp/proj',
      source: 'vscode',
    });
  });

  it('replays a task_started → user_message → agent_message → task_complete arc', async () => {
    const tid = uuid('bbbb');
    writeRollout(root, tid, [
      sessionMeta(tid),
      eventMsg('task_started', { model_context_window: 32_000 }),
      eventMsg('user_message', { message: 'rename foo to bar' }),
      eventMsg('agent_message', { message: 'done, renamed foo→bar' }),
      eventMsg('task_complete'),
    ]);
    feed = new RolloutWatcherFeed({ sessionsRoot: root });
    await feed.start({ onEvent: (e) => events.push(e) });

    const kinds = events.map((e) => e.kind);
    expect(kinds).toContain('status');
    expect(kinds).toContain('usage');

    // Activity ring buffer captured the user+agent turns + task_complete.
    const activity = feed.getRecentActivity(10);
    const kinds2 = activity.map((a) => a.kind);
    expect(kinds2).toContain('user');
    expect(kinds2).toContain('agent');
    expect(kinds2).toContain('task_complete');
  });

  it('setAccountInfo emits a merged session_info update', async () => {
    const tid = uuid('cccc');
    writeRollout(root, tid, [sessionMeta(tid)]);
    feed = new RolloutWatcherFeed({ sessionsRoot: root });
    await feed.start({ onEvent: (e) => events.push(e) });

    events.length = 0;
    feed.setAccountInfo({ email: 'demo@user.com', planType: 'pro' });

    const si = events.find((e) => e.kind === 'session_info');
    expect(si).toBeDefined();
    expect(feed.getLastSessionInfo()).toMatchObject({
      account_email: 'demo@user.com',
      plan_type: 'pro',
    });
  });

  it('followThread switches to a thread that exists on disk', async () => {
    const tOld = uuid('1111');
    const tNew = uuid('2222');
    writeRollout(root, tOld, [sessionMeta(tOld)]);
    writeRollout(root, tNew, [sessionMeta(tNew)]);

    feed = new RolloutWatcherFeed({ sessionsRoot: root });
    await feed.start({ onEvent: (e) => events.push(e) });
    // One of the two is picked as "newest"; we don't care which.
    const initial = feed.getCurrentThreadId();
    expect([tOld, tNew]).toContain(initial);

    const other = initial === tOld ? tNew : tOld;
    const ok = await feed.followThread(other);
    expect(ok).toBe(true);
    expect(feed.getCurrentThreadId()).toBe(other);
  });

  it('followThread returns false for a missing thread id', async () => {
    const tid = uuid('3333');
    writeRollout(root, tid, [sessionMeta(tid)]);
    feed = new RolloutWatcherFeed({ sessionsRoot: root });
    await feed.start({ onEvent: (e) => events.push(e) });
    const ok = await feed.followThread('00000000-0000-0000-0000-deadbeef');
    expect(ok).toBe(false);
  });

  it('ignores malformed JSONL lines without aborting the read', async () => {
    const tid = uuid('dddd');
    const today = new Date();
    const dir = join(
      root,
      String(today.getFullYear()),
      String(today.getMonth() + 1).padStart(2, '0'),
      String(today.getDate()).padStart(2, '0'),
    );
    mkdirSync(dir, { recursive: true });
    // Exactly 4 chunks of `[^-]+` between `rollout-` and `${tid}`.
    const file = join(dir, `rollout-2026-05-23-1030-${tid}.jsonl`);
    writeFileSync(
      file,
      [
        JSON.stringify(sessionMeta(tid)),
        '{ broken json line',
        JSON.stringify(eventMsg('user_message', { message: 'still parses' })),
      ].join('\n') + '\n',
    );
    feed = new RolloutWatcherFeed({ sessionsRoot: root });
    await feed.start({ onEvent: (e) => events.push(e) });

    // The good lines around the broken one still produced events.
    expect(feed.getCurrentThreadId()).toBe(tid);
    expect(feed.getRecentActivity().some((a) => a.kind === 'user')).toBe(true);
  });

  it('stop() is idempotent and silences further timer callbacks', async () => {
    const tid = uuid('eeee');
    writeRollout(root, tid, [sessionMeta(tid)]);
    feed = new RolloutWatcherFeed({ sessionsRoot: root, scanIntervalMs: 50 });
    await feed.start({ onEvent: (e) => events.push(e) });
    await feed.stop();
    await feed.stop();    // second call should be a no-op

    const beforeWait = events.length;
    await new Promise((r) => setTimeout(r, 150));
    expect(events.length).toBe(beforeWait);
  });
});
