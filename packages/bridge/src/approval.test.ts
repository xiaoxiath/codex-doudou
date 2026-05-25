/**
 * Tests for the Codex PermissionRequest → Doudou bridge HTTP entrypoint.
 *
 * Three angles:
 *   1. classifyHookPayload picks the right RiskDecision for canonical
 *      tool_name + tool_input shapes.
 *   2. handleApprovalRequest returns `{}` when there are no devices
 *      (graceful fall-back per the spec doc).
 *   3. handleApprovalRequest translates a device `allow` / `deny` reply
 *      into the expected hook JSON.
 */
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { PassThrough } from 'node:stream';
import { describe, expect, it, beforeEach, afterEach } from 'vitest';

import {
  classifyHookPayload,
  handleApprovalRequest,
  type PermissionRequestHookPayload,
} from './approval.js';
import { AuditLog } from './audit.js';
import { DeviceRegistry } from './deviceRegistry.js';

/** Tiny in-memory IncomingMessage + ServerResponse stand-ins. */
function makeReq(body: unknown, method = 'POST') {
  const stream = new PassThrough() as PassThrough & {
    method?: string; url?: string; headers?: Record<string, string>;
  };
  stream.method = method;
  stream.url = '/approval/request';
  stream.headers = { 'content-type': 'application/json' };
  stream.end(JSON.stringify(body));
  return stream;
}

function makeRes() {
  const chunks: Buffer[] = [];
  let status = 0;
  let headers: Record<string, string> = {};
  const res = {
    writeHead(s: number, h?: Record<string, string>) {
      status = s;
      if (h) headers = { ...headers, ...h };
      return res;
    },
    end(buf?: string | Buffer) {
      if (buf) chunks.push(Buffer.isBuffer(buf) ? buf : Buffer.from(buf, 'utf-8'));
    },
    get statusCode() { return status; },
    get headersOut() { return headers; },
    get body() { return Buffer.concat(chunks).toString('utf-8'); },
  };
  return res;
}

describe('classifyHookPayload', () => {
  it('shell with rm -rf is high-risk run_command', () => {
    const d = classifyHookPayload({
      tool_name: 'shell',
      tool_input: { command: ['rm', '-rf', '/tmp/x'] },
    });
    expect(d.risk).toBe('high');
    expect(d.action_type).toBe('run_command');
  });

  it('shell with ls is low-risk run_command', () => {
    const d = classifyHookPayload({
      tool_name: 'shell',
      tool_input: { command: ['ls', '-la'] },
    });
    expect(d.risk).toBe('low');
    expect(d.action_type).toBe('run_command');
  });

  it('apply_patch sensitive file is high-risk modify_file', () => {
    const d = classifyHookPayload({
      tool_name: 'apply_patch',
      tool_input: { changes: [{ path: '.env', op: 'modify' }] },
    });
    expect(d.risk).toBe('high');
    expect(d.action_type).toBe('modify_file');
  });

  it('apply_patch with create maps op→kind correctly', () => {
    const d = classifyHookPayload({
      tool_name: 'apply_patch',
      tool_input: { changes: [{ path: 'src/new.ts', op: 'add' }] },
    });
    expect(d.action_type).toBe('modify_file');
    expect(d.risk).toBe('low'); // create is low-risk per risk.ts
  });

  it('unknown tool falls through to tool_call classifier', () => {
    const d = classifyHookPayload({
      tool_name: 'send_email',
      tool_input: {},
    });
    expect(d.action_type).toBe('tool_call');
  });

  it('empty payload yields a sensible default', () => {
    const d = classifyHookPayload({} as PermissionRequestHookPayload);
    expect(d.action_type).toBeDefined();
    expect(['low', 'medium', 'high']).toContain(d.risk);
  });
});

describe('handleApprovalRequest', () => {
  let auditDir: string;
  let audit: AuditLog;

  beforeEach(async () => {
    auditDir = mkdtempSync(join(tmpdir(), 'doudou-approval-test-'));
    audit = new AuditLog(auditDir);
    await audit.init();
  });

  afterEach(() => {
    rmSync(auditDir, { recursive: true, force: true });
  });

  it('returns 405 for non-POST', async () => {
    const req = makeReq({}, 'GET');
    const res = makeRes();
    const registry = new DeviceRegistry();
    await handleApprovalRequest(req as never, res as never, { registry, audit });
    expect(res.statusCode).toBe(405);
  });

  it('returns 400 for bad JSON', async () => {
    const stream = new PassThrough() as PassThrough & { method?: string; url?: string };
    stream.method = 'POST';
    stream.end('not json');
    const res = makeRes();
    const registry = new DeviceRegistry();
    await handleApprovalRequest(stream as never, res as never, { registry, audit });
    expect(res.statusCode).toBe(400);
  });

  it('returns {} when no devices are registered (graceful fallback)', async () => {
    const req = makeReq({ tool_name: 'shell', tool_input: { command: ['ls'] } });
    const res = makeRes();
    const registry = new DeviceRegistry();
    await handleApprovalRequest(req as never, res as never, { registry, audit });
    expect(res.statusCode).toBe(200);
    expect(JSON.parse(res.body)).toEqual({});
  });

  it('returns allow JSON when a device picks "allow"', async () => {
    const req = makeReq({
      tool_name: 'shell',
      tool_input: { command: ['ls', '-la'] },
      description: 'List files',
    });
    const res = makeRes();
    const registry = new DeviceRegistry();
    const dev = registry.getOrCreate('doudou-test');
    // Auto-reply with "allow" the moment a question is pushed.
    const originalPush = dev.pushQuestion.bind(dev);
    dev.pushQuestion = async (p) => {
      const promise = originalPush(p);
      // simulate the device tapping the choice via the wire-format Reply
      setTimeout(() => {
        dev.onReply({
          v: 1, type: 'reply', seq: 1, ts: 0,
          id: p.id, choice_id: 'allow', device_id: 'doudou-test',
        });
      }, 5);
      return promise;
    };

    await handleApprovalRequest(req as never, res as never, {
      registry,
      audit,
      defaultTimeoutMs: 2000,
    });

    expect(res.statusCode).toBe(200);
    const parsed = JSON.parse(res.body);
    expect(parsed.hookSpecificOutput?.decision?.behavior).toBe('allow');
    expect(parsed.hookSpecificOutput?.hookEventName).toBe('PermissionRequest');
  });

  it('returns deny JSON when a device picks "deny"', async () => {
    const req = makeReq({
      tool_name: 'shell',
      tool_input: { command: ['rm', '-rf', '/tmp/x'] },
    });
    const res = makeRes();
    const registry = new DeviceRegistry();
    const dev = registry.getOrCreate('doudou-test');
    const originalPush = dev.pushQuestion.bind(dev);
    dev.pushQuestion = async (p) => {
      const promise = originalPush(p);
      setTimeout(() => {
        dev.onReply({
          v: 1, type: 'reply', seq: 1, ts: 0,
          id: p.id, choice_id: 'deny', device_id: 'doudou-test',
        });
      }, 5);
      return promise;
    };

    await handleApprovalRequest(req as never, res as never, {
      registry,
      audit,
      defaultTimeoutMs: 2000,
    });

    const parsed = JSON.parse(res.body);
    expect(parsed.hookSpecificOutput?.decision?.behavior).toBe('deny');
    expect(parsed.hookSpecificOutput?.decision?.message).toBeDefined();
  });

  it('returns {} when device never replies (timeout)', async () => {
    const req = makeReq({ tool_name: 'shell', tool_input: { command: ['ls'] } });
    const res = makeRes();
    const registry = new DeviceRegistry();
    registry.getOrCreate('doudou-test');   // exists but never replies

    await handleApprovalRequest(req as never, res as never, {
      registry,
      audit,
      defaultTimeoutMs: 80,
    });
    expect(res.statusCode).toBe(200);
    expect(JSON.parse(res.body)).toEqual({});
  });
});
