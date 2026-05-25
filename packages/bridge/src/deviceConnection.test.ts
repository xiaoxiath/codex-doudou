/**
 * deviceConnection.ts — WS lifecycle tests.
 *
 * The WS class only touches its WebSocket via `on`/`send`/`close` +
 * `readyState`. We stand up a minimal EventEmitter that quacks like
 * `ws.WebSocket`, exercise the full hello → welcome → reply → close
 * path, and assert at each step.
 */
import { EventEmitter } from 'node:events';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { DeviceConnection } from './deviceConnection.js';
import { DeviceRegistry, type QuestionPayload } from './deviceRegistry.js';

/** Stand-in for `ws.WebSocket` — only the surface deviceConnection uses. */
class MockWebSocket extends EventEmitter {
  static readonly OPEN = 1;
  static readonly CLOSED = 3;
  readonly OPEN = MockWebSocket.OPEN;
  readonly CLOSED = MockWebSocket.CLOSED;
  readyState = MockWebSocket.OPEN;
  sent: string[] = [];

  send(data: string): void {
    this.sent.push(data);
  }
  close(_code?: number, _reason?: string): void {
    this.readyState = MockWebSocket.CLOSED;
    this.emit('close');
  }
  /** Simulate the device sending us a JSON line. */
  recv(payload: object): void {
    this.emit('message', Buffer.from(JSON.stringify(payload), 'utf-8'));
  }
}

function sentEnvelopes(ws: MockWebSocket): Array<Record<string, unknown>> {
  return ws.sent.map((l) => JSON.parse(l) as Record<string, unknown>);
}

describe('DeviceConnection (WS)', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it('accepts a valid hello, sends welcome, fires onReady', () => {
    const ws = new MockWebSocket();
    const registry = new DeviceRegistry();
    const onReady = vi.fn();
    new DeviceConnection(ws as never, {
      registry, pairingToken: 'tok-12345',
      onReply: async () => undefined,
      onReady,
    });

    ws.recv({
      v: 1, type: 'hello', seq: 1, ts: 0,
      device_id: 'doudou-1', fw_version: '0.1.0', pairing_token: 'tok-12345',
    });

    const env = sentEnvelopes(ws);
    expect(env).toHaveLength(1);
    expect(env[0]!.type).toBe('welcome');
    expect(env[0]!.session_id).toMatch(/^sess_/);
    expect(onReady).toHaveBeenCalledTimes(1);
    expect(registry.getOrCreate('doudou-1').isConnected).toBe(true);
  });

  it('rejects a hello with mismatched pairing_token and closes', () => {
    const ws = new MockWebSocket();
    const registry = new DeviceRegistry();
    new DeviceConnection(ws as never, {
      registry, pairingToken: 'right-tok',
      onReply: async () => undefined,
    });

    ws.recv({
      v: 1, type: 'hello', seq: 1, ts: 0,
      device_id: 'evil-device', fw_version: 'x', pairing_token: 'wrong-tok',
    });

    expect(ws.readyState).toBe(MockWebSocket.CLOSED);
    // No welcome should have been sent.
    expect(sentEnvelopes(ws).find((e) => e.type === 'welcome')).toBeUndefined();
  });

  it('closes when hello does not arrive within the timeout', () => {
    const ws = new MockWebSocket();
    new DeviceConnection(ws as never, {
      registry: new DeviceRegistry(),
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });

    // Hello timeout is 2_000ms in the implementation.
    vi.advanceTimersByTime(2_100);
    expect(ws.readyState).toBe(MockWebSocket.CLOSED);
  });

  it('routes a matching reply back through DeviceState.onReply', async () => {
    const ws = new MockWebSocket();
    const registry = new DeviceRegistry();
    new DeviceConnection(ws as never, {
      registry, pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    ws.recv({
      v: 1, type: 'hello', seq: 1, ts: 0,
      device_id: 'doudou-1', fw_version: 'x', pairing_token: 'tok-12345',
    });
    const state = registry.getOrCreate('doudou-1');

    const q: QuestionPayload = {
      id: 'q-1',
      risk: 'low',
      action_type: 'run_command',
      title: 'demo',
      choices: [{ id: 'allow', label: 'OK' }],
      expires_in_ms: 60_000,
      require_confirm: false,
    };
    const pushed = state.pushQuestion(q);

    ws.recv({
      v: 1, type: 'reply', seq: 2, ts: 0,
      id: 'q-1', device_id: 'doudou-1', choice_id: 'allow',
    });

    const audited = await pushed;
    expect(audited.reply.choice_id).toBe('allow');
  });

  it('invokes opts.onFollowThread when the device asks to switch threads', () => {
    const ws = new MockWebSocket();
    const onFollow = vi.fn();
    const registry = new DeviceRegistry();
    new DeviceConnection(ws as never, {
      registry, pairingToken: 'tok-12345',
      onReply: async () => undefined,
      onFollowThread: onFollow,
    });
    ws.recv({
      v: 1, type: 'hello', seq: 1, ts: 0,
      device_id: 'doudou-1', fw_version: 'x', pairing_token: 'tok-12345',
    });

    ws.recv({
      v: 1, type: 'follow_thread', seq: 2, ts: 0, thread_id: 'thr-42',
    });

    expect(onFollow).toHaveBeenCalledTimes(1);
    expect(onFollow.mock.calls[0]![0]).toBe('thr-42');
  });

  it('sendStatus serialises a v1 envelope with stamped seq/ts', () => {
    const ws = new MockWebSocket();
    const conn = new DeviceConnection(ws as never, {
      registry: new DeviceRegistry(),
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    conn.sendStatus({ state: 'thinking', title: 'analyzing', body: undefined });

    const env = sentEnvelopes(ws).at(-1)!;
    expect(env.type).toBe('status');
    expect(env.state).toBe('thinking');
    expect(env.title).toBe('analyzing');
    expect(env.v).toBe(1);
    expect(typeof env.seq).toBe('number');
    expect(typeof env.ts).toBe('number');
  });

  it('sendQuestion stamps expires_at + queue_total + drops expires_in_ms', () => {
    const ws = new MockWebSocket();
    const conn = new DeviceConnection(ws as never, {
      registry: new DeviceRegistry(),
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    const q: QuestionPayload = {
      id: 'q-7', risk: 'high', action_type: 'run_command', title: 't',
      choices: [{ id: 'a', label: 'A' }], expires_in_ms: 30_000, require_confirm: true,
    };
    conn.sendQuestion(q, 1_700_000_000_000, 3);

    const env = sentEnvelopes(ws).at(-1)!;
    expect(env.type).toBe('question');
    expect(env.id).toBe('q-7');
    expect(env.expires_at).toBe(1_700_000_000_000);
    expect(env.queue_total).toBe(3);
    expect(env.expires_in_ms).toBeUndefined();
  });

  it('rawSend returns -1 when the socket is no longer open', () => {
    const ws = new MockWebSocket();
    ws.readyState = MockWebSocket.CLOSED;
    const conn = new DeviceConnection(ws as never, {
      registry: new DeviceRegistry(),
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    const seq = conn.sendStatus({ state: 'idle', title: '' });
    expect(seq).toBe(-1);
  });

  it('detaches the DeviceState from the registry on ws close', () => {
    const ws = new MockWebSocket();
    const registry = new DeviceRegistry();
    new DeviceConnection(ws as never, {
      registry, pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    ws.recv({
      v: 1, type: 'hello', seq: 1, ts: 0,
      device_id: 'doudou-1', fw_version: 'x', pairing_token: 'tok-12345',
    });
    const state = registry.getOrCreate('doudou-1');
    expect(state.isConnected).toBe(true);

    ws.close();
    expect(state.isConnected).toBe(false);
  });

  it('treats malformed JSON as a no-op (does not throw or close)', () => {
    const ws = new MockWebSocket();
    new DeviceConnection(ws as never, {
      registry: new DeviceRegistry(),
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });

    ws.emit('message', Buffer.from('not-json', 'utf-8'));

    expect(ws.readyState).toBe(MockWebSocket.OPEN);
  });

  it('treats a non-conforming envelope (zod fail) as a no-op', () => {
    const ws = new MockWebSocket();
    new DeviceConnection(ws as never, {
      registry: new DeviceRegistry(),
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    ws.recv({ v: 1, type: 'hello' /* missing required fields */ });
    expect(ws.readyState).toBe(MockWebSocket.OPEN);
  });

  it('ignores reply for a stale question id without crashing', () => {
    const ws = new MockWebSocket();
    const registry = new DeviceRegistry();
    new DeviceConnection(ws as never, {
      registry, pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    ws.recv({
      v: 1, type: 'hello', seq: 1, ts: 0,
      device_id: 'doudou-1', fw_version: 'x', pairing_token: 'tok-12345',
    });
    ws.recv({
      v: 1, type: 'reply', seq: 2, ts: 0,
      id: 'never-asked', device_id: 'doudou-1', choice_id: 'allow',
    });
    // Nothing to assert beyond "we're still alive".
    expect(ws.readyState).toBe(MockWebSocket.OPEN);
  });

  it('answers an inbound ping with a matching pong', () => {
    const ws = new MockWebSocket();
    new DeviceConnection(ws as never, {
      registry: new DeviceRegistry(),
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    ws.recv({
      v: 1, type: 'hello', seq: 1, ts: 0,
      device_id: 'doudou-1', fw_version: 'x', pairing_token: 'tok-12345',
    });
    ws.recv({ v: 1, type: 'ping', seq: 99, ts: 0 });

    const pong = sentEnvelopes(ws).find((e) => e.type === 'pong');
    expect(pong).toBeDefined();
    expect((pong as { pong_for_seq: number }).pong_for_seq).toBe(99);
  });

  it('explicit close() shuts down idempotently', () => {
    const ws = new MockWebSocket();
    const conn = new DeviceConnection(ws as never, {
      registry: new DeviceRegistry(),
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    conn.close(1000, 'bye');
    conn.close(1000, 'second bye');     // must not throw
    expect(ws.readyState).toBe(MockWebSocket.CLOSED);
  });
});
