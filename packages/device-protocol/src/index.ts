/**
 * Doudou device protocol v1.
 *
 * Wire format: UTF-8 JSON over WebSocket, one message per frame.
 * Authoritative spec: docs/device-protocol.md
 *
 * Design invariants enforced here:
 *  - every message has `v`, `type`, `seq`, `ts`
 *  - unknown fields are stripped (zod default), not errored — forward compat
 *  - sizes capped per spec (title ≤ 30B, body ≤ 150B, choices ≤ 4)
 */

import { z } from 'zod';

export const PROTOCOL_VERSION = 1 as const;

// ---------- Enums ----------

export const StateSchema = z.enum([
  'idle',
  'thinking',
  'executing',
  'waiting_input',
  'waiting_approval',
  'done',
  'error',
]);
export type State = z.infer<typeof StateSchema>;

export const RiskSchema = z.enum(['low', 'medium', 'high']);
export type Risk = z.infer<typeof RiskSchema>;

export const ActionTypeSchema = z.enum([
  'run_command',
  'modify_file',
  'network_access',
  'user_input',
  'tool_call',
  'other',
]);
export type ActionType = z.infer<typeof ActionTypeSchema>;

export const ErrorCodeSchema = z.enum([
  'request_expired',
  'bridge_disconnected',
  'codex_unreachable',
  'pairing_invalid',
  'protocol_version_mismatch',
  'reply_dropped',
  'rate_limited',
  'internal',
]);
export type ErrorCode = z.infer<typeof ErrorCodeSchema>;

// ---------- Constraints ----------

const Title = z.string().refine((s) => byteLen(s) <= 30, {
  message: 'title exceeds 30 bytes',
});
const Body = z.string().refine((s) => byteLen(s) <= 150, {
  message: 'body exceeds 150 bytes',
});
const ChoiceLabel = z.string().refine((s) => charLen(s) <= 8, {
  message: 'choice label exceeds 8 characters',
});

function byteLen(s: string): number {
  return new TextEncoder().encode(s).length;
}
function charLen(s: string): number {
  return [...s].length;
}

// ---------- Shared envelope ----------

const Envelope = z.object({
  v: z.literal(PROTOCOL_VERSION),
  type: z.string(),
  seq: z.number().int().nonnegative(),
  ts: z.number().int().nonnegative(),
});

// ---------- Bridge → Device ----------

export const WelcomeSchema = Envelope.extend({
  type: z.literal('welcome'),
  server_time_ms: z.number().int(),
  session_id: z.string(),
  heartbeat_interval_ms: z.number().int().positive(),
  max_question_choices: z.number().int().positive().max(4),
  features: z.array(z.enum(['ack', 'device_status'])).default([]),
});
export type Welcome = z.infer<typeof WelcomeSchema>;

export const StatusSchema = Envelope.extend({
  type: z.literal('status'),
  state: StateSchema,
  title: Title,
  body: Body.optional(),
  updated_at: z.number().int().optional(),
});
export type Status = z.infer<typeof StatusSchema>;

export const ChoiceSchema = z.object({
  id: z.string().min(1),
  label: ChoiceLabel,
});
export type Choice = z.infer<typeof ChoiceSchema>;

export const QuestionSchema = Envelope.extend({
  type: z.literal('question'),
  id: z.string().min(1),
  risk: RiskSchema,
  action_type: ActionTypeSchema,
  title: Title,
  body: Body.optional(),
  choices: z.array(ChoiceSchema).min(1).max(4),
  expires_at: z.number().int().positive(),
  require_confirm: z.boolean().default(false),
  /** Bridge-side total of inflight questions including this one, ≥ 1. */
  queue_total: z.number().int().min(1).optional(),
});
export type Question = z.infer<typeof QuestionSchema>;

export const ErrorMsgSchema = Envelope.extend({
  type: z.literal('error'),
  code: ErrorCodeSchema,
  title: Title,
  body: Body.optional(),
  related_id: z.string().optional(),
});
export type ErrorMsg = z.infer<typeof ErrorMsgSchema>;

// ---------- Session info (left-far screen) ----------

export const SessionInfoSchema = Envelope.extend({
  type: z.literal('session_info'),
  session_id: z.string().optional(),
  /** AI-generated thread title (from thread/list.name, falls back to preview). */
  thread_title: z.string().optional(),
  /** "Codex Desktop" / "vscode" / "doudou-bridge" — who created/owns the thread. */
  source: z.string().optional(),
  model: z.string().optional(),
  reasoning_effort: z.string().optional(),         // "high" / "xhigh" / "auto" ...
  summary_mode: z.string().optional(),             // "auto" / "none" / "concise" ...
  cwd: z.string().optional(),
  /** Combined "Workspace (on-request)" style label. */
  permissions: z.string().optional(),
  approval_policy: z.string().optional(),
  sandbox: z.string().optional(),
  collaboration_mode: z.string().optional(),
  account_email: z.string().optional(),
  plan_type: z.string().optional(),
  agents_md: z.boolean().optional(),
  git_branch: z.string().optional(),
  cli_version: z.string().optional(),
});
export type SessionInfo = z.infer<typeof SessionInfoSchema>;

// ---------- Thread list (right-far screen) ----------

export const ThreadSummarySchema = z.object({
  id: z.string(),
  /** AI-generated title; falls back to first user message preview. */
  title: z.string(),
  source: z.string().optional(),
  /** Whether this is the thread doudou is currently following. */
  active: z.boolean().optional(),
  /** Epoch ms when last updated. */
  updated_at: z.number().int().optional(),
});
export type ThreadSummary = z.infer<typeof ThreadSummarySchema>;

export const ThreadListSchema = Envelope.extend({
  type: z.literal('thread_list'),
  threads: z.array(ThreadSummarySchema).max(20),
});
export type ThreadList = z.infer<typeof ThreadListSchema>;

// ---------- Usage (token / quota snapshot) ----------

export const UsageLimitSchema = z.object({
  id: z.string(),
  label: z.string().optional(),
  /** Group/model label for stacking multiple model quotas (e.g. "GPT-5.3-Codex-Spark").
   *  When undefined the entry belongs to the main plan group. */
  group_label: z.string().optional(),
  used_pct: z.number().min(0).max(100).optional(),
  window_minutes: z.number().int().positive().optional(),
  resets_at: z.number().int().optional(),
});
export type UsageLimit = z.infer<typeof UsageLimitSchema>;

export const UsageSchema = Envelope.extend({
  type: z.literal('usage'),
  session: z
    .object({
      /** Cumulative across all turns — what you've billed to your subscription. */
      input_tokens: z.number().int().nonnegative().optional(),
      output_tokens: z.number().int().nonnegative().optional(),
      cached_tokens: z.number().int().nonnegative().optional(),
      total_tokens: z.number().int().nonnegative().optional(),
      /**
       * Tokens currently held in the model's context window for this thread.
       * Computed from the most recent turn's input + output. This is the
       * number to use against `model_context_window` for "how full is my
       * context", distinct from cumulative session billing above.
       */
      current_context_tokens: z.number().int().nonnegative().optional(),
      /** Total tokens the model accepts per turn; used to compute context-window fill %. */
      model_context_window: z.number().int().positive().optional(),
    })
    .optional(),
  limits: z.array(UsageLimitSchema).optional(),
  plan_type: z.string().optional(),
});
export type Usage = z.infer<typeof UsageSchema>;

// ---------- Device → Bridge ----------

export const HelloSchema = Envelope.extend({
  type: z.literal('hello'),
  device_id: z.string().min(1).max(64),
  fw_version: z.string().max(32),
  pairing_token: z.string().min(8).max(128),
  resume_after_seq: z.number().int().nonnegative().optional(),
});
export type Hello = z.infer<typeof HelloSchema>;

export const ReplySchema = Envelope.extend({
  type: z.literal('reply'),
  id: z.string().min(1),
  choice_id: z.string().min(1),
  device_id: z.string().min(1),
});
export type Reply = z.infer<typeof ReplySchema>;

export const FollowThreadSchema = Envelope.extend({
  type: z.literal('follow_thread'),
  thread_id: z.string().min(1),
});
export type FollowThread = z.infer<typeof FollowThreadSchema>;

export const DeviceStatusSchema = Envelope.extend({
  type: z.literal('device_status'),
  battery_pct: z.number().int().min(0).max(100).optional(),
  charging: z.boolean().optional(),
  rssi: z.number().int().optional(),
  free_heap: z.number().int().nonnegative().optional(),
  uptime_ms: z.number().int().nonnegative().optional(),
});
export type DeviceStatus = z.infer<typeof DeviceStatusSchema>;

// ---------- Bidirectional ----------

export const PingSchema = Envelope.extend({ type: z.literal('ping') });
export type Ping = z.infer<typeof PingSchema>;

export const PongSchema = Envelope.extend({
  type: z.literal('pong'),
  pong_for_seq: z.number().int().nonnegative(),
});
export type Pong = z.infer<typeof PongSchema>;

export const AckSchema = Envelope.extend({
  type: z.literal('ack'),
  ack_for_seq: z.number().int().nonnegative(),
});
export type Ack = z.infer<typeof AckSchema>;

// ---------- Union ----------

export const BridgeToDeviceSchema = z.discriminatedUnion('type', [
  WelcomeSchema,
  StatusSchema,
  QuestionSchema,
  ErrorMsgSchema,
  UsageSchema,
  SessionInfoSchema,
  ThreadListSchema,
  PingSchema,
  PongSchema,
  AckSchema,
]);
export type BridgeToDevice = z.infer<typeof BridgeToDeviceSchema>;

export const DeviceToBridgeSchema = z.discriminatedUnion('type', [
  HelloSchema,
  ReplySchema,
  FollowThreadSchema,
  DeviceStatusSchema,
  PingSchema,
  PongSchema,
  AckSchema,
]);
export type DeviceToBridge = z.infer<typeof DeviceToBridgeSchema>;

export const AnyMessageSchema = z.discriminatedUnion('type', [
  WelcomeSchema,
  StatusSchema,
  QuestionSchema,
  ErrorMsgSchema,
  UsageSchema,
  SessionInfoSchema,
  ThreadListSchema,
  HelloSchema,
  ReplySchema,
  FollowThreadSchema,
  DeviceStatusSchema,
  PingSchema,
  PongSchema,
  AckSchema,
]);
export type AnyMessage = z.infer<typeof AnyMessageSchema>;

// ---------- Parse helpers ----------

export type ParseResult<T> =
  | { ok: true; value: T }
  | { ok: false; error: string };

export function parseFromDevice(raw: unknown): ParseResult<DeviceToBridge> {
  const result = DeviceToBridgeSchema.safeParse(raw);
  if (result.success) return { ok: true, value: result.data };
  return { ok: false, error: result.error.message };
}

export function parseFromBridge(raw: unknown): ParseResult<BridgeToDevice> {
  const result = BridgeToDeviceSchema.safeParse(raw);
  if (result.success) return { ok: true, value: result.data };
  return { ok: false, error: result.error.message };
}
