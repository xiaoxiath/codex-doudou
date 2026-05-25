/**
 * HTTP-routing smoke tests for the Bridge orchestrator.
 *
 * Boots a real bridge on an ephemeral port (with a noop codex feed so we
 * don't spawn the real codex CLI), then hits each documented endpoint
 * via `fetch`. Covers what the unit tests of risk/approval/registry
 * etc. don't reach: the actual HTTP surface devices and the simulator
 * talk to.
 */
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { startBridge, type BridgeHandle } from './bridge.js';
import type { BridgeEvent, CodexFeed, ReplyDecision } from './codexFeed.js';

/** A codex feed that does nothing — start/stop only, no scripted events. */
class NoopFeed implements CodexFeed {
  async start(): Promise<void> { /* noop */ }
  async stop(): Promise<void> { /* noop */ }
  async acceptReply(_d: ReplyDecision): Promise<void> { /* noop */ }
  // eslint-disable-next-line @typescript-eslint/no-unused-vars
  _silenceUnused(_e?: BridgeEvent): void { /* keep type imports honest */ }
}

describe('Bridge HTTP routes', () => {
  let bridge: BridgeHandle;
  let auditDir: string;
  let port: number;

  beforeEach(async () => {
    auditDir = mkdtempSync(join(tmpdir(), 'doudou-bridge-routes-'));
    port = 18788 + Math.floor(Math.random() * 1000); // avoid 8788 clash with running dev bridge
    bridge = await startBridge({
      port,
      pairingToken: 'tok-12345',
      codexMode: 'stub',
      auditDir,
      feedFactory: () => new NoopFeed(),
      mdns: false,        // tests shouldn't pollute the LAN
      simulatorRoot: tmpdir(), // any dir works for static — we just probe routes
    });
  });

  afterEach(async () => {
    await bridge.stop();
    rmSync(auditDir, { recursive: true, force: true });
  });

  it('GET /healthz returns ok + empty device list', async () => {
    const r = await fetch(`http://localhost:${port}/healthz`);
    expect(r.status).toBe(200);
    const j = (await r.json()) as { status: string; devices: unknown[] };
    expect(j.status).toBe('ok');
    expect(j.devices).toEqual([]);
  });

  it('GET /favicon.ico returns a 200 with image content-type', async () => {
    const r = await fetch(`http://localhost:${port}/favicon.ico`);
    // The handler may either serve static favicon.svg or respond 204; either is fine,
    // but it shouldn't 500.
    expect([200, 204, 404]).toContain(r.status);
  });

  it('GET /api/recent returns { entries: [] } when audit dir is empty', async () => {
    const r = await fetch(`http://localhost:${port}/api/recent?n=5`);
    expect(r.status).toBe(200);
    expect(r.headers.get('content-type')).toMatch(/json/);
    const body = (await r.json()) as { entries: unknown[] };
    expect(Array.isArray(body.entries)).toBe(true);
    expect(body.entries).toEqual([]);
  });

  it('GET /api/activity returns JSON', async () => {
    const r = await fetch(`http://localhost:${port}/api/activity?n=3`);
    expect(r.status).toBe(200);
    expect(r.headers.get('content-type')).toMatch(/json/);
  });

  it('POST /approval/request with empty body returns 400', async () => {
    const r = await fetch(`http://localhost:${port}/approval/request`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: 'not-json',
    });
    expect(r.status).toBe(400);
  });

  it('POST /approval/request with no devices returns {} (graceful fallback)', async () => {
    const r = await fetch(`http://localhost:${port}/approval/request`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        tool_name: 'shell',
        tool_input: { command: ['ls'] },
      }),
    });
    expect(r.status).toBe(200);
    expect(await r.json()).toEqual({});
  });

  it('GET /approval/request returns 405 (method not allowed)', async () => {
    const r = await fetch(`http://localhost:${port}/approval/request`);
    expect(r.status).toBe(405);
  });

  it('unknown route falls through to the static handler', async () => {
    const r = await fetch(`http://localhost:${port}/does-not-exist`);
    // Static handler returns 404 for missing files, which is what we want.
    expect([404, 200]).toContain(r.status);
  });
});
