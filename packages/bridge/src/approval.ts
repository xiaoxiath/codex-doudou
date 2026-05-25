/**
 * approval.ts — HTTP entrypoint for Codex `PermissionRequest` hooks.
 *
 * Codex Desktop fires a hook on each tool-permission request. The hook
 * (a tiny Python script — `scripts/permission_request_hook.py`) forwards
 * the payload to `POST /approval/request` here. We:
 *
 *   1. classify the action via `risk.ts`,
 *   2. turn it into a `question` payload (same shape used by the device
 *      protocol — see `docs/device-protocol.md` and `device-protocol.md`'s
 *      Question schema),
 *   3. broadcast to every known device + race the first reply,
 *   4. translate `allow` / `deny` into the JSON shape Codex expects on
 *      hook stdout (see docs/esp32-codex-approval-bridge.md).
 *
 * On timeout, missing devices, or any error, we respond `{}` so Codex
 * falls back to its own approval flow (the documented graceful-degrade
 * behaviour).
 */
import { randomUUID } from 'node:crypto';
import type { IncomingMessage, ServerResponse } from 'node:http';

import type { AuditLog } from './audit.js';
import { log } from './log.js';
import type { DeviceRegistry, QuestionPayload } from './deviceRegistry.js';
import {
  classifyCommand,
  classifyFileChange,
  classifyToolCall,
  type FileChange,
  type RiskDecision,
} from './risk.js';

/** Codex hook payload — `cwd` / `tool_name` / `tool_input` / `description`
 *  per https://developers.openai.com/codex/hooks (PermissionRequest). */
export interface PermissionRequestHookPayload {
  cwd?: string;
  tool_name?: string;
  tool_input?: unknown;
  description?: string;
}

export interface ApprovalHookResponse {
  hookSpecificOutput?: {
    hookEventName: 'PermissionRequest';
    decision: { behavior: 'allow' | 'deny'; message?: string };
  };
}

/** Map a Codex hook payload into a RiskDecision via the existing
 *  classifiers in `risk.ts`. Tool names normalised to lowercase. */
export function classifyHookPayload(p: PermissionRequestHookPayload): RiskDecision {
  const tool = (p.tool_name ?? '').toLowerCase();
  const input = (p.tool_input ?? {}) as Record<string, unknown>;

  // Shell / command execution → classify the command argv.
  if (tool === 'shell' || tool === 'run_command' || tool === 'execute' || tool === 'bash') {
    const command: string[] = Array.isArray(input.argv)
      ? (input.argv as string[])
      : Array.isArray(input.command)
      ? (input.command as string[])
      : typeof input.command === 'string'
      ? [input.command as string]
      : [];
    const rawCwd = typeof input.cwd === 'string' ? (input.cwd as string) : p.cwd;
    return classifyCommand({ command, cwd: rawCwd ?? null });
  }

  // File patches / writes → classify the affected paths.
  if (tool === 'apply_patch' || tool === 'modify_file' || tool === 'edit_file' || tool === 'write_file') {
    const raw = Array.isArray(input.changes) ? (input.changes as unknown[]) : [];
    const changes: FileChange[] = raw
      .filter((c): c is Record<string, unknown> => typeof c === 'object' && c !== null)
      .map((c) => ({
        path: typeof c.path === 'string' ? c.path : '',
        kind: ((): FileChange['kind'] => {
          // Hook payloads vary — accept Codex's "op" field or our own "kind".
          const v = typeof c.kind === 'string'
            ? c.kind
            : typeof c.op === 'string'
            ? c.op
            : '';
          if (v === 'create' || v === 'add')            return 'create';
          if (v === 'delete' || v === 'remove')         return 'delete';
          return 'modify';
        })(),
      }))
      .filter((c) => c.path !== '');
    return classifyFileChange(changes);
  }

  return classifyToolCall({ toolName: tool || 'unknown' });
}

/** Short body for the device (`<=` ~140 bytes per protocol budget). */
function summarize(p: PermissionRequestHookPayload, decision: RiskDecision): string {
  const parts: string[] = [];
  if (p.cwd) {
    const tail = p.cwd.split('/').filter(Boolean).slice(-2).join('/');
    parts.push(`目录: ${tail || p.cwd}`);
  }
  if (p.tool_name) parts.push(`工具: ${p.tool_name}`);
  if (decision.reason) parts.push(decision.reason);
  if (p.description) parts.push(p.description);
  return parts.join('\n').slice(0, 140);
}

/** Title for the device — short, falls back to the action_type. */
function makeTitle(p: PermissionRequestHookPayload, decision: RiskDecision): string {
  const desc = (p.description ?? '').trim();
  if (desc) return desc.slice(0, 28);
  switch (decision.action_type) {
    case 'run_command':    return 'Codex 想运行命令';
    case 'modify_file':    return 'Codex 想修改文件';
    case 'network_access': return 'Codex 想访问网络';
    case 'tool_call':      return 'Codex 想调用工具';
    case 'user_input':     return 'Codex 等你输入';
    default:               return 'Codex 请求审批';
  }
}

export interface HandleApprovalDeps {
  registry: DeviceRegistry;
  audit: AuditLog;
  /** Total hook-side timeout in ms. Default 90s; the device gets the
   *  expires_in slightly shorter so its UI clears before the hook does. */
  defaultTimeoutMs?: number;
}

type RaceResult =
  | { ok: true; choice: string; latency: number; device: string }
  | { ok: false; err: string }
  | { timeout: true };

async function readBody(req: IncomingMessage, max: number): Promise<string> {
  const chunks: Buffer[] = [];
  let bytes = 0;
  for await (const chunk of req) {
    const buf = chunk as Buffer;
    bytes += buf.length;
    if (bytes > max) throw new Error('body_too_large');
    chunks.push(buf);
  }
  return Buffer.concat(chunks).toString('utf-8');
}

export async function handleApprovalRequest(
  req: IncomingMessage,
  res: ServerResponse,
  deps: HandleApprovalDeps,
): Promise<void> {
  if (req.method !== 'POST') {
    res.writeHead(405).end();
    return;
  }

  let payload: PermissionRequestHookPayload;
  try {
    const body = await readBody(req, 64 * 1024);
    payload = JSON.parse(body) as PermissionRequestHookPayload;
  } catch (err) {
    log.warn({ err: String(err) }, 'approval: bad body');
    res.writeHead(400, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ error: 'invalid_json' }));
    return;
  }

  const decision = classifyHookPayload(payload);
  const known = [...deps.registry.all()];

  // No registered device → no chance of a decision. Return passthrough so
  // Codex Desktop falls back to its own approval prompt (per docs).
  if (known.length === 0) {
    log.info({ tool: payload.tool_name }, 'approval: no devices, falling back');
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({}));
    return;
  }

  const timeoutMs = deps.defaultTimeoutMs ?? 90_000;
  const id = `approval_${randomUUID().replace(/-/g, '').slice(0, 16)}`;
  const question: QuestionPayload = {
    id,
    risk: decision.risk,
    action_type: decision.action_type,
    title: makeTitle(payload, decision),
    body: summarize(payload, decision),
    choices: [
      { id: 'allow', label: '允许' },
      { id: 'deny',  label: '拒绝' },
    ],
    expires_in_ms: Math.max(timeoutMs - 1000, 5000),
    require_confirm: decision.risk === 'high',
  };

  log.info(
    { id, risk: decision.risk, action: decision.action_type, devices: known.length },
    'approval: dispatching question',
  );

  const racePromises: Array<Promise<RaceResult>> = known.map((d) =>
    d
      .pushQuestion(question)
      .then((r) => ({ ok: true as const, choice: r.reply.choice_id, latency: r.latency_ms, device: d.deviceId }))
      .catch((err: unknown) => ({ ok: false as const, err: String(err) })),
  );
  racePromises.push(
    new Promise<RaceResult>((resolve) => setTimeout(() => resolve({ timeout: true }), timeoutMs)),
  );

  const result = await Promise.race(racePromises);

  const response: ApprovalHookResponse = {};
  if ('timeout' in result) {
    log.warn({ id }, 'approval: timed out — falling back to Codex prompt');
  } else if (result.ok) {
    const isAllow = result.choice === 'allow' || result.choice === 'accept';
    response.hookSpecificOutput = {
      hookEventName: 'PermissionRequest',
      decision: isAllow
        ? { behavior: 'allow' }
        : { behavior: 'deny', message: 'Denied by Doudou device.' },
    };
    await deps.audit.append({
      ts: new Date().toISOString(),
      device: result.device,
      request_id: id,
      action_type: decision.action_type,
      risk: decision.risk,
      choice: result.choice,
      latency_ms: result.latency,
      command: question.title,
    });
    log.info(
      { id, choice: result.choice, latency_ms: result.latency, device: result.device },
      'approval: device decided',
    );
  } else {
    log.warn({ id, err: result.err }, 'approval: device rejected — falling back');
  }

  res.writeHead(200, { 'content-type': 'application/json' });
  res.end(JSON.stringify(response));
}
