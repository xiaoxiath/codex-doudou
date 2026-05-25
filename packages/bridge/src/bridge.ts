/**
 * Bridge orchestrator: HTTP/WS server + DeviceRegistry + CodexFeed + audit.
 *
 * Routing model for MVP:
 *  - Status messages broadcast to all currently-connected devices.
 *  - Questions broadcast to every *known* device (connected or not).
 *    First-reply-wins via Promise.race; other devices' questions stay
 *    in-flight until their expiry or this process's shutdown.
 */
import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

import { WebSocketServer } from 'ws';

import { handleApprovalRequest } from './approval.js';
import { AuditLog, type AuditEntry } from './audit.js';
import { DeviceConnection } from './deviceConnection.js';
import { DeviceRegistry, type QuestionPayload, type StatusPayload } from './deviceRegistry.js';
import { StubCodexFeed, type BridgeEvent, type CodexFeed } from './codexFeed.js';
import { RealCodexFeed } from './codex/realCodexFeed.js';
import { RolloutWatcherFeed, type ActivityEntry } from './codex/rolloutWatcher.js';
import { probeAccount } from './codex/accountProbe.js';
import {
  ThreadListPoller,
  type RateLimitsSnapshot,
  type ThreadSummary,
} from './codex/threadListPoller.js';
import { announceBridge, type MdnsAnnouncer } from './mdns.js';
import { startBleScanner, type BleScannerHandle } from './ble/scanner.js';
import { PROTOCOL_VERSION } from '@doudou/device-protocol';
import { log } from './log.js';
import { makeStaticHandler } from './staticServer.js';

const __dirname = dirname(fileURLToPath(import.meta.url));

export interface BridgeOptions {
  port: number;
  pairingToken: string;
  codexMode: 'stub' | 'real';
  auditDir: string;
  simulatorRoot?: string;
  /** Override Codex feed factory (mainly for tests). */
  feedFactory?: () => CodexFeed;
  /** Workspace cwd passed to real codex thread/start. Defaults to process.cwd(). */
  workspace?: string;
  /** Codex approval policy override. Default: on-request. */
  codexApprovalPolicy?: 'untrusted' | 'on-failure' | 'on-request' | 'never';
  /** Codex thread source: follow (resume latest) / own (new thread) / auto (try follow else own). Default: auto. */
  codexSource?: 'follow' | 'own' | 'auto';
  /** Announce Bridge via mDNS as _doudou._tcp.local. Default: true. */
  mdns?: boolean;
  /** Start the BLE central scanner so the firmware can attach via GATT
   *  instead of WebSocket. Requires `@abandonware/noble` installed.
   *  Default: false. */
  ble?: boolean;
}

export interface BridgeHandle {
  port: number;
  registry: DeviceRegistry;
  stop(): Promise<void>;
}

export async function startBridge(opts: BridgeOptions): Promise<BridgeHandle> {
  const audit = new AuditLog(opts.auditDir);
  await audit.init();

  const registry = new DeviceRegistry();

  const feed: CodexFeed = (
    opts.feedFactory ??
    defaultFeedFactory(opts.codexMode, opts.workspace, opts.codexApprovalPolicy, opts.codexSource)
  )();

  const simulatorRoot =
    opts.simulatorRoot ?? resolve(__dirname, '..', '..', 'simulator', 'public');
  const staticHandler = makeStaticHandler(simulatorRoot);

  const server = createServer((req, res) => httpHandler(req, res));
  const wss = new WebSocketServer({ noServer: true });

  server.on('upgrade', (req, socket, head) => {
    if (req.url?.startsWith('/device')) {
      wss.handleUpgrade(req, socket, head, (ws) => {
        new DeviceConnection(ws, {
          registry,
          pairingToken: opts.pairingToken,
          onReply: async () => undefined, // unused — we await via pushQuestion below
          onReady: (state) => {
            // Seed the just-connected device with the latest cached snapshot
            // from the feed (e.g. usage + session_info from the watcher).
            if (feed instanceof RolloutWatcherFeed) {
              const usage = feed.getLastUsage();
              if (usage) state.pushUsage(usage);
              const sess = feed.getLastSessionInfo();
              if (sess) state.pushSessionInfo(sess);
            }
            if (lastThreadList.length) {
              state.pushThreadList({ threads: lastThreadList });
            }
          },
          onFollowThread: async (threadId, _state) => {
            if (!(feed instanceof RolloutWatcherFeed)) {
              log.warn({ threadId }, 'follow_thread: feed is not rollout watcher');
              return;
            }
            const ok = await feed.followThread(threadId);
            if (ok) {
              // Refresh the active flag on the cached list immediately.
              for (const t of lastThreadList) t.active = t.id === threadId;
              fanOut({ kind: 'thread_list', threads: lastThreadList });
            }
          },
        });
      });
    } else {
      socket.destroy();
    }
  });

  await new Promise<void>((res) => server.listen(opts.port, res));
  log.info({ port: opts.port, simulatorRoot, mode: opts.codexMode }, 'bridge listening');
  log.info({ url: `http://localhost:${opts.port}/` }, 'open simulator in browser');

  const announcer: MdnsAnnouncer | null = opts.mdns !== false
    ? announceBridge(opts.port, PROTOCOL_VERSION)
    : null;

  /* Optional BLE central — same DeviceRegistry, alternative transport. */
  let bleScanner: BleScannerHandle | null = null;
  if (opts.ble) {
    bleScanner = await startBleScanner({
      registry,
      pairingToken: opts.pairingToken,
      onReply: async () => undefined, // race resolved via pushQuestion
    });
    if (!bleScanner) {
      log.warn('ble requested but noble not loadable — continuing without BLE');
    }
  }

  // Probe Codex account in parallel with starting the feed.
  // Doesn't block: feed starts emitting events, account info is merged when ready.
  const accountPromise = probeAccount().catch((err) => {
    log.warn({ err: String(err) }, 'account probe rejected');
    return null;
  });

  await feed.start({
    onEvent: (e) => fanOut(e),
  });

  // Long-lived thread/list poller — fetches AI-generated titles every 10s.
  const lastThreadList: Array<{ id: string; title: string; source?: string; active?: boolean; updated_at?: number }> = [];
  let activeThreadId: string | undefined;
  if (feed instanceof RolloutWatcherFeed) {
    activeThreadId = feed.getCurrentThreadId();
  }
  const poller = new ThreadListPoller({ intervalMs: 10_000, limitsIntervalMs: 30_000, limit: 12 });
  void poller
    .start({
      onThreads: (threads: ThreadSummary[]) => onThreadList(threads),
      onRateLimits: (snapshot: RateLimitsSnapshot) => onRateLimits(snapshot),
    })
    .catch((err) => log.warn({ err: String(err) }, 'thread poller failed to start'));

  void accountPromise.then((info) => {
    if (!info || !feed) return;
    if (feed instanceof RolloutWatcherFeed) {
      feed.setAccountInfo(info);
    }
    log.info({ email: info.email, planType: info.planType }, 'codex account info loaded');
  });

  return {
    port: opts.port,
    registry,
    async stop() {
      log.info('stopping feed and connections');
      await announcer?.stop();
      await bleScanner?.stop();
      await poller.stop();
      await feed.stop();
      for (const state of registry.all()) state.detach();
      await new Promise<void>((res) => wss.close(() => res()));
      await new Promise<void>((res) => server.close(() => res()));
    },
  };

  function onRateLimits(snapshot: RateLimitsSnapshot): void {
    // Flatten groups → flat UsageLimit array with group_label tag.
    const limits: Array<{
      id: string;
      label?: string;
      group_label?: string;
      used_pct?: number;
      window_minutes?: number;
      resets_at?: number;
    }> = [];
    for (const g of snapshot.groups) {
      const groupLabel = g.label; // undefined for main plan
      const idPrefix = g.id;
      if (g.primary) {
        limits.push({
          id: `${idPrefix}_primary`,
          label: g.primary.window_minutes ? `${Math.round(g.primary.window_minutes / 60)}h` : 'primary',
          ...(groupLabel ? { group_label: groupLabel } : {}),
          used_pct: g.primary.used_percent,
          ...(g.primary.window_minutes !== undefined ? { window_minutes: g.primary.window_minutes } : {}),
          ...(g.primary.resets_at_ms !== undefined ? { resets_at: g.primary.resets_at_ms } : {}),
        });
      }
      if (g.secondary) {
        const days = g.secondary.window_minutes ? Math.round(g.secondary.window_minutes / 60 / 24) : 0;
        limits.push({
          id: `${idPrefix}_secondary`,
          label: days > 0 ? `${days}d` : 'secondary',
          ...(groupLabel ? { group_label: groupLabel } : {}),
          used_pct: g.secondary.used_percent,
          ...(g.secondary.window_minutes !== undefined ? { window_minutes: g.secondary.window_minutes } : {}),
          ...(g.secondary.resets_at_ms !== undefined ? { resets_at: g.secondary.resets_at_ms } : {}),
        });
      }
    }
    fanOut({
      kind: 'usage',
      limits,
      ...(snapshot.planType ? { plan_type: snapshot.planType } : {}),
    });
  }

  function onThreadList(threads: ThreadSummary[]): void {
    // Update active flag based on which thread our RolloutWatcher follows.
    if (feed instanceof RolloutWatcherFeed) {
      activeThreadId = feed.getCurrentThreadId();
    }
    // Codex `thread.id` is the canonical UUID; our watcher's threadId may be the rollout-file suffix.
    // Match by either ends-with or full equality so the active flag still lights up.
    const summaries = threads.map((t) => ({
      id: t.id,
      title: titleOf(t),
      ...(t.source ? { source: t.source } : {}),
      ...(t.updatedAt !== undefined ? { updated_at: t.updatedAt * 1000 } : {}),
      ...(activeThreadId && (t.id === activeThreadId || activeThreadId.endsWith(t.id))
        ? { active: true }
        : {}),
    }));
    lastThreadList.length = 0;
    lastThreadList.push(...summaries);

    fanOut({ kind: 'thread_list', threads: summaries });

    // Also fold the active thread's title into session_info so the pet
    // screen knows what to display under the pet.
    const active = summaries.find((s) => s.active);
    if (active?.title) {
      fanOut({ kind: 'session_info', thread_title: active.title });
    }
  }

  // ---------- routing ----------

  function fanOut(e: BridgeEvent): void {
    const known = Array.from(registry.all());
    if (known.length === 0) {
      log.debug({ kind: e.kind }, 'no devices known, dropping event');
      return;
    }
    if (e.kind === 'status') {
      const payload: StatusPayload = {
        state: e.state,
        title: e.title,
        ...(e.body !== undefined ? { body: e.body } : {}),
      };
      for (const d of known) d.pushStatus(payload);
      return;
    }
    if (e.kind === 'usage') {
      for (const d of known) {
        d.pushUsage({
          ...(e.session ? { session: e.session } : {}),
          ...(e.limits ? { limits: e.limits } : {}),
          ...(e.plan_type ? { plan_type: e.plan_type } : {}),
        });
      }
      return;
    }
    if (e.kind === 'session_info') {
      const { kind: _k, ...rest } = e;
      for (const d of known) d.pushSessionInfo(rest);
      return;
    }
    if (e.kind === 'thread_list') {
      for (const d of known) d.pushThreadList({ threads: e.threads });
      return;
    }
    // question — fan out, race for first reply
    const payload: QuestionPayload = {
      id: e.id,
      risk: e.risk,
      action_type: e.action_type,
      title: e.title,
      ...(e.body !== undefined ? { body: e.body } : {}),
      choices: e.choices,
      expires_in_ms: e.expires_in_ms,
      require_confirm: e.require_confirm,
    };
    const promises = known.map((d) =>
      d
        .pushQuestion(payload)
        .then((result) => ({ device: d, result }))
        .catch((err: unknown) => ({ device: d, err })),
    );
    void Promise.race(promises).then(async (winner) => {
      if ('err' in winner) {
        log.warn({ err: String(winner.err), id: e.id }, 'all devices failed question');
        await feed.acceptReply({ request_id: e.id, choice_id: 'decline' });
        return;
      }
      const { result, device } = winner;
      const entry: AuditEntry = {
        ts: new Date().toISOString(),
        device: device.deviceId,
        request_id: result.reply.id,
        action_type: result.payload.action_type,
        risk: result.payload.risk,
        choice: result.reply.choice_id,
        latency_ms: result.latency_ms,
        command: result.payload.title,
      };
      await audit.append(entry);
      log.info({ ...entry }, 'reply audited');
      await feed.acceptReply({
        request_id: result.reply.id,
        choice_id: result.reply.choice_id,
      });
    });
  }

  function httpHandler(req: IncomingMessage, res: ServerResponse): void {
    if (!req.url) {
      res.statusCode = 400;
      res.end('bad request');
      return;
    }
    if (req.url === '/favicon.ico') {
      // Browsers ask for /favicon.ico even when an SVG favicon is linked.
      // 204 stops the retries cleanly.
      res.statusCode = 204;
      res.end();
      return;
    }
    if (req.url === '/approval/request') {
      void handleApprovalRequest(req, res, { registry, audit });
      return;
    }
    if (req.url === '/healthz') {
      const devices = Array.from(registry.all()).map((d) => ({
        device_id: d.deviceId,
        connected: d.isConnected,
        inflight: d.inflightSize,
      }));
      res.statusCode = 200;
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ status: 'ok', devices }));
      return;
    }
    if (req.url?.startsWith('/api/activity') && (req.method === 'GET' || req.method === 'HEAD')) {
      const url = new URL(req.url, `http://localhost:${opts.port}`);
      const n = Math.min(Math.max(1, Number(url.searchParams.get('n') ?? '10')), 50);
      let entries: ActivityEntry[] = [];
      if (feed instanceof RolloutWatcherFeed) {
        entries = feed.getRecentActivity(n);
      }
      res.statusCode = 200;
      res.setHeader('content-type', 'application/json');
      res.setHeader('cache-control', 'no-cache');
      res.end(JSON.stringify({ entries }));
      return;
    }
    if (req.url?.startsWith('/api/recent') && (req.method === 'GET' || req.method === 'HEAD')) {
      const url = new URL(req.url, `http://localhost:${opts.port}`);
      const n = Math.min(Math.max(1, Number(url.searchParams.get('n') ?? '8')), 100);
      void audit.readRecent(n).then(
        (entries) => {
          /* Slim down so the on-device renderer doesn't haul raw 4KB
           * command strings, but keep enough for the browser log
           * viewer (`/log.html`) to be useful: device, latency,
           * truncated summary. */
          const slim = entries.map((e) => ({
            ts: e.ts,
            device: e.device,
            action_type: e.action_type,
            risk: e.risk,
            choice: e.choice,
            latency_ms: e.latency_ms,
            summary:
              typeof e.command === 'string' && e.command
                ? e.command.slice(0, 160)
                : `${e.action_type}`,
            request_id: e.request_id,
          }));
          res.statusCode = 200;
          res.setHeader('content-type', 'application/json');
          res.setHeader('cache-control', 'no-cache');
          res.end(JSON.stringify({ entries: slim }));
        },
        (err) => {
          res.statusCode = 500;
          res.end(JSON.stringify({ error: String(err) }));
        },
      );
      return;
    }
    void staticHandler(req, res, req.url);
  }
}

function titleOf(t: ThreadSummary): string {
  if (t.name && t.name.trim()) return t.name.trim();
  if (t.preview && t.preview.trim()) return t.preview.trim();
  return t.id.slice(0, 8);
}

function defaultFeedFactory(
  mode: 'stub' | 'real',
  workspace?: string,
  approvalPolicy?: 'untrusted' | 'on-failure' | 'on-request' | 'never',
  source?: 'follow' | 'own' | 'auto',
): () => CodexFeed {
  if (mode === 'stub') return () => new StubCodexFeed();
  // `follow` mode = read-only watch of rollout JSONL files. The most useful
  // mode when the user is already driving Codex from Desktop/CLI and just
  // wants doudou to react.
  if (source === 'follow') return () => new RolloutWatcherFeed();
  // `own` and `auto` use the in-process Codex feed (approvals routed here).
  return () => new RealCodexFeed({ workspace, approvalPolicy, source });
}

/** Convenience for tests that want to wire in a real feed. */
export type { CodexFeed, BridgeEvent } from './codexFeed.js';
export { DeviceRegistry } from './deviceRegistry.js';
