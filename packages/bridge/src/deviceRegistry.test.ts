import { describe, expect, it } from 'vitest';

import {
  DeviceRegistry,
  DeviceState,
  type DeviceTransport,
  type QuestionPayload,
  type StatusPayload,
} from './deviceRegistry.js';

/** In-memory transport that records what got sent. */
class FakeTransport implements DeviceTransport {
  outbox: Array<{ type: string; [k: string]: unknown }> = [];
  closed = false;
  seq = 0;
  resetSeq(): void {
    this.seq = 0;
  }
  sendStatus(p: StatusPayload): number {
    this.outbox.push({ type: 'status', ...p });
    return ++this.seq;
  }
  sendQuestion(p: QuestionPayload, expires_at: number, queue_total: number): number {
    this.outbox.push({ type: 'question', expires_at, id: p.id, risk: p.risk, queue_total });
    return ++this.seq;
  }
  sendError(p: { code: string; title: string }): number {
    this.outbox.push({ type: 'error', ...p });
    return ++this.seq;
  }
  sendUsage(p: unknown): number {
    this.outbox.push({ type: 'usage', payload: p });
    return ++this.seq;
  }
  sendSessionInfo(p: unknown): number {
    this.outbox.push({ type: 'session_info', payload: p });
    return ++this.seq;
  }
  sendThreadList(p: unknown): number {
    this.outbox.push({ type: 'thread_list', payload: p });
    return ++this.seq;
  }
  sendAck(ackForSeq: number): void {
    this.outbox.push({ type: 'ack', ack_for_seq: ackForSeq });
  }
  sendPing(): void {
    this.outbox.push({ type: 'ping' });
  }
  sendPong(pongForSeq: number): void {
    this.outbox.push({ type: 'pong', pong_for_seq: pongForSeq });
  }
  close(): void {
    this.closed = true;
  }
}

function makeQuestion(id: string, expiresMs = 30_000): QuestionPayload {
  return {
    id,
    risk: 'low',
    action_type: 'run_command',
    title: 'q',
    choices: [{ id: 'accept', label: 'A' }, { id: 'decline', label: 'D' }],
    expires_in_ms: expiresMs,
    require_confirm: false,
  };
}

describe('DeviceRegistry', () => {
  it('getOrCreate returns same instance for same id', () => {
    const reg = new DeviceRegistry();
    const a = reg.getOrCreate('doudou-1');
    const b = reg.getOrCreate('doudou-1');
    expect(a).toBe(b);
  });

  it('all() iterates known devices', () => {
    const reg = new DeviceRegistry();
    reg.getOrCreate('a');
    reg.getOrCreate('b');
    const ids = [...reg.all()].map((d) => d.deviceId).sort();
    expect(ids).toEqual(['a', 'b']);
  });
});

describe('DeviceState reconnect resume', () => {
  it('replays unexpired inflight questions on attach', async () => {
    const state = new DeviceState('d1');
    const t1 = new FakeTransport();
    state.attach(t1, 0);

    const q1 = makeQuestion('q1', 60_000);
    const q2 = makeQuestion('q2', 60_000);
    void state.pushQuestion(q1);
    void state.pushQuestion(q2);

    expect(t1.outbox.filter((m) => m.type === 'question').length).toBe(2);

    // disconnect
    state.detach();

    // new transport reconnects
    const t2 = new FakeTransport();
    state.attach(t2, 0);

    const replayed = t2.outbox.filter((m) => m.type === 'question').map((m) => m.id);
    expect(replayed.sort()).toEqual(['q1', 'q2']);
  });

  it('drops expired questions during replay and rejects their promise', async () => {
    const state = new DeviceState('d2');
    const t1 = new FakeTransport();
    state.attach(t1, 0);

    const q = makeQuestion('q-expire', 50);
    const promise = state.pushQuestion(q);

    state.detach();

    await new Promise((r) => setTimeout(r, 80));

    const t2 = new FakeTransport();
    state.attach(t2, 0);

    expect(t2.outbox.filter((m) => m.type === 'question').length).toBe(0);
    await expect(promise).rejects.toThrow(/expired/);
  });

  it('replays current status on reattach', () => {
    const state = new DeviceState('d3');
    const t1 = new FakeTransport();
    state.attach(t1, 0);
    state.pushStatus({ state: 'thinking', title: 'T' });
    state.detach();

    const t2 = new FakeTransport();
    state.attach(t2, 0);
    expect(t2.outbox[0]).toMatchObject({ type: 'status', state: 'thinking' });
  });
});

describe('DeviceState reply handling', () => {
  it('matches inflight reply, acks, resolves promise', async () => {
    const state = new DeviceState('d4');
    const t = new FakeTransport();
    state.attach(t, 0);

    const q = makeQuestion('qx');
    const p = state.pushQuestion(q);

    const result = state.onReply({
      v: 1,
      type: 'reply',
      seq: 10,
      ts: 100,
      id: 'qx',
      choice_id: 'accept',
      device_id: 'd4',
    });

    expect(result.kind).toBe('matched');
    expect(t.outbox.some((m) => m.type === 'ack' && m.ack_for_seq === 10)).toBe(true);
    const audited = await p;
    expect(audited.reply.choice_id).toBe('accept');
    expect(audited.latency_ms).toBeGreaterThanOrEqual(0);
  });

  it('deduplicates a second reply with same id, still acks', () => {
    const state = new DeviceState('d5');
    const t = new FakeTransport();
    state.attach(t, 0);

    void state.pushQuestion(makeQuestion('q-dup'));

    state.onReply({ v: 1, type: 'reply', seq: 1, ts: 0, id: 'q-dup', choice_id: 'accept', device_id: 'd5' });
    const second = state.onReply({ v: 1, type: 'reply', seq: 2, ts: 0, id: 'q-dup', choice_id: 'accept', device_id: 'd5' });

    expect(second.kind).toBe('duplicate');
    expect(t.outbox.filter((m) => m.type === 'ack').length).toBeGreaterThanOrEqual(2);
  });

  it('orphan reply (no matching inflight) is acked, returns orphan', () => {
    const state = new DeviceState('d6');
    const t = new FakeTransport();
    state.attach(t, 0);

    const r = state.onReply({ v: 1, type: 'reply', seq: 99, ts: 0, id: 'never-sent', choice_id: 'accept', device_id: 'd6' });
    expect(r.kind).toBe('orphan');
    expect(t.outbox.some((m) => m.type === 'ack' && m.ack_for_seq === 99)).toBe(true);
  });
});

describe('DeviceState detached behavior', () => {
  it('pushQuestion while detached holds inflight, then sends on reattach', async () => {
    const state = new DeviceState('d7');
    // no transport yet
    const q = makeQuestion('q-detached', 60_000);
    void state.pushQuestion(q);
    expect(state.inflightSize).toBe(1);

    const t = new FakeTransport();
    state.attach(t, 0);
    expect(t.outbox.some((m) => m.type === 'question' && m.id === 'q-detached')).toBe(true);
  });

  it('pushStatus while detached is held, replayed on reconnect', () => {
    const state = new DeviceState('d8');
    state.pushStatus({ state: 'executing', title: 'X' });
    const t = new FakeTransport();
    state.attach(t, 0);
    expect(t.outbox[0]).toMatchObject({ type: 'status', state: 'executing' });
  });
});
