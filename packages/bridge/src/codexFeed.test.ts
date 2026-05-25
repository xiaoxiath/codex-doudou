import { afterEach, describe, expect, it, vi } from 'vitest';

import { StubCodexFeed, type BridgeEvent } from './codexFeed.js';

describe('StubCodexFeed', () => {
  afterEach(() => vi.useRealTimers());

  it('emits scripted events at scheduled intervals after start()', async () => {
    vi.useFakeTimers();
    const feed = new StubCodexFeed();
    const events: BridgeEvent[] = [];
    await feed.start({ onEvent: (e) => events.push(e) });

    // Drive the timer loop forward enough to flush several SCRIPT entries.
    await vi.advanceTimersByTimeAsync(15_000);
    expect(events.length).toBeGreaterThan(0);

    // Every event must have a recognised kind from the BridgeEvent union.
    for (const e of events) {
      expect(['status', 'question', 'session_info', 'thread_list']).toContain(e.kind);
    }

    await feed.stop();
  });

  it('stop() cancels pending scheduled events', async () => {
    vi.useFakeTimers();
    const feed = new StubCodexFeed();
    const events: BridgeEvent[] = [];
    await feed.start({ onEvent: (e) => events.push(e) });
    // Let one fire, then stop.
    await vi.advanceTimersByTimeAsync(2_000);
    const before = events.length;
    await feed.stop();
    // No further events should be queued even after long time.
    await vi.advanceTimersByTimeAsync(60_000);
    expect(events.length).toBe(before);
  });

  it('mints fresh question ids each loop so devices see distinct requests', async () => {
    vi.useFakeTimers();
    const feed = new StubCodexFeed();
    const ids: string[] = [];
    await feed.start({
      onEvent: (e) => { if (e.kind === 'question') ids.push(e.id); },
    });
    // Long enough to wrap the SCRIPT a couple of times.
    await vi.advanceTimersByTimeAsync(180_000);
    await feed.stop();
    // At least two questions fired, and no duplicate ids in that run.
    expect(ids.length).toBeGreaterThanOrEqual(2);
    const unique = new Set(ids);
    expect(unique.size).toBe(ids.length);
  });

  it('acceptReply on an unknown id is a no-op (no throw)', async () => {
    const feed = new StubCodexFeed();
    await expect(
      feed.acceptReply({ request_id: 'nope', choice_id: 'allow' }),
    ).resolves.toBeUndefined();
  });
});
