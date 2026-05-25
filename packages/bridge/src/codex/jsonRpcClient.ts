/**
 * Minimal JSON-RPC 2.0 client over a child process's stdin/stdout.
 *
 * Wire format matches what codex app-server emits with `--listen stdio://`:
 * one JSON object per line, with the `"jsonrpc":"2.0"` envelope omitted.
 *
 * Handles three flows:
 *   - we request, server responds  → request()
 *   - server notifies, we observe  → onNotification()
 *   - server requests, we respond  → onServerRequest()
 */
import { spawn, type ChildProcess } from 'node:child_process';
import { createInterface } from 'node:readline';

import { log } from '../log.js';

type RequestId = number | string;

interface PendingRequest {
  resolve: (result: unknown) => void;
  reject: (err: Error) => void;
}

export type NotificationHandler = (params: unknown) => void;
export type ServerRequestHandler = (
  params: unknown,
  id: RequestId,
  method: string,
) => Promise<unknown>;

export class JsonRpcStdioClient {
  private child: ChildProcess | null = null;
  private nextId = 1;
  private pending = new Map<RequestId, PendingRequest>();
  private notificationHandlers = new Map<string, NotificationHandler>();
  private serverRequestHandlers = new Map<string, ServerRequestHandler>();
  private fallbackNotification: NotificationHandler | null = null;
  private fallbackServerRequest: ServerRequestHandler | null = null;
  private closed = false;
  private exitWaiter: Promise<number | null> | null = null;

  constructor(
    private readonly bin: string,
    private readonly args: string[],
    private readonly env: NodeJS.ProcessEnv = process.env,
  ) {}

  async start(): Promise<void> {
    if (this.child) throw new Error('already started');
    const child = spawn(this.bin, this.args, {
      stdio: ['pipe', 'pipe', 'pipe'],
      env: this.env,
    });
    this.child = child;

    const rl = createInterface({ input: child.stdout!, crlfDelay: Infinity });
    rl.on('line', (line) => this.onLine(line));
    rl.on('close', () => {
      if (!this.closed) log.warn('jsonrpc stdout closed unexpectedly');
    });

    child.stderr?.on('data', (chunk: Buffer) => {
      const text = chunk.toString('utf8').trimEnd();
      if (text) log.debug({ codex: text }, 'codex stderr');
    });

    this.exitWaiter = new Promise((resolveExit) => {
      child.once('exit', (code) => {
        this.closed = true;
        const err = new Error(`codex app-server exited (code=${code})`);
        for (const p of this.pending.values()) p.reject(err);
        this.pending.clear();
        log.info({ code }, 'codex app-server exited');
        resolveExit(code);
      });
    });

    child.once('error', (err) => {
      log.error({ err: String(err) }, 'failed to spawn codex app-server');
    });
  }

  onNotification(method: string, handler: NotificationHandler): void {
    this.notificationHandlers.set(method, handler);
  }

  /** Catch-all for notifications without a specific handler. */
  setFallbackNotification(handler: NotificationHandler | null): void {
    this.fallbackNotification = handler;
  }

  onServerRequest(method: string, handler: ServerRequestHandler): void {
    this.serverRequestHandlers.set(method, handler);
  }

  setFallbackServerRequest(handler: ServerRequestHandler | null): void {
    this.fallbackServerRequest = handler;
  }

  /** Send a request, return a promise resolving to the result. */
  request<TResult = unknown>(method: string, params?: unknown): Promise<TResult> {
    if (!this.child || this.closed) {
      return Promise.reject(new Error('client not running'));
    }
    const id = this.nextId++;
    const payload: Record<string, unknown> = { id, method };
    if (params !== undefined) payload.params = params;
    return new Promise<TResult>((resolve, reject) => {
      this.pending.set(id, {
        resolve: (r) => resolve(r as TResult),
        reject,
      });
      this.writeLine(payload);
    });
  }

  /** Fire-and-forget notification. */
  notify(method: string, params?: unknown): void {
    if (!this.child || this.closed) return;
    const payload: Record<string, unknown> = { method };
    if (params !== undefined) payload.params = params;
    this.writeLine(payload);
  }

  async stop(signal: NodeJS.Signals = 'SIGTERM'): Promise<void> {
    if (!this.child || this.closed) return;
    this.closed = true;
    try {
      this.child.stdin?.end();
    } catch {
      /* ignore */
    }
    this.child.kill(signal);
    await this.exitWaiter;
  }

  /** True if the child process exited (either intentionally or crashed). */
  isDead(): boolean {
    return this.closed;
  }

  // ---------- internal ----------

  private writeLine(obj: unknown): void {
    const line = JSON.stringify(obj) + '\n';
    this.child!.stdin!.write(line);
  }

  private respondResult(id: RequestId, result: unknown): void {
    this.writeLine({ id, result });
  }
  private respondError(id: RequestId, code: number, message: string): void {
    this.writeLine({ id, error: { code, message } });
  }

  private onLine(line: string): void {
    if (!line.trim()) return;
    let parsed: unknown;
    try {
      parsed = JSON.parse(line);
    } catch (err) {
      log.warn({ line: line.slice(0, 200), err: String(err) }, 'invalid jsonl from codex');
      return;
    }
    if (typeof parsed !== 'object' || parsed === null) {
      log.warn({ line: line.slice(0, 200) }, 'non-object jsonl from codex');
      return;
    }
    const msg = parsed as Record<string, unknown>;

    // Response (has id and result/error, no method)
    if ('id' in msg && !('method' in msg)) {
      const id = msg.id as RequestId;
      const pending = this.pending.get(id);
      if (!pending) {
        log.warn({ id }, 'response for unknown request id');
        return;
      }
      this.pending.delete(id);
      if ('error' in msg && msg.error) {
        const err = msg.error as { code?: number; message?: string };
        pending.reject(new Error(`jsonrpc ${err.code}: ${err.message ?? 'unknown'}`));
      } else {
        pending.resolve(msg.result);
      }
      return;
    }

    // Server-initiated request (has id and method)
    if ('id' in msg && 'method' in msg) {
      const id = msg.id as RequestId;
      const method = msg.method as string;
      const handler =
        this.serverRequestHandlers.get(method) ?? this.fallbackServerRequest;
      if (!handler) {
        log.warn({ method }, 'unhandled server request');
        this.respondError(id, -32601, `no handler for ${method}`);
        return;
      }
      void Promise.resolve()
        .then(() => handler(msg.params, id, method))
        .then(
          (result) => this.respondResult(id, result),
          (err) => {
            log.warn({ method, err: String(err) }, 'server request handler threw');
            this.respondError(id, -32603, String(err?.message ?? err));
          },
        );
      return;
    }

    // Notification (method, no id)
    if ('method' in msg) {
      const method = msg.method as string;
      const handler =
        this.notificationHandlers.get(method) ?? this.fallbackNotification;
      try {
        handler?.(msg.params);
      } catch (err) {
        log.warn({ method, err: String(err) }, 'notification handler threw');
      }
      return;
    }

    log.warn({ line: line.slice(0, 200) }, 'jsonrpc message with neither id nor method');
  }
}
