/**
 * Doudou simulator — three-screen pet terminal speaking device protocol v1.
 * Same wire format as real firmware.
 */
import {
  PROTOCOL_VERSION,
  parseFromBridge,
  type AnyMessage,
  type DeviceToBridge,
  type ErrorMsg,
  type Question,
  type SessionInfo,
  type Status,
  type ThreadList,
  type Usage,
  type Welcome,
} from '@doudou/device-protocol';

import { logLine } from './ui/log.js';
import { Pet } from './ui/pet.js';
import { ScreenManager } from './ui/screens.js';
import { renderQuestion, hideQuestion } from './ui/question.js';
import { renderUsage } from './ui/usage.js';
import { renderInfo } from './ui/info.js';
import { ThreadListView } from './ui/threadList.js';

const DEVICE_ID = 'doudou-sim';
const FW_VERSION = '0.0.2-sim';
const PAIRING_TOKEN = 'dev-token-change-me';
const RECONNECT_DELAY_MS = 1500;

class SimulatorClient {
  private ws: WebSocket | null = null;
  private outSeq = 1;
  private serverTimeAnchor = 0;
  private localAnchorMs = 0;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  private pet: Pet;
  private screens: ScreenManager;
  private threads: ThreadListView;
  private lastUsage: Usage | null = null;
  private lastInfo: SessionInfo | null = null;

  // local question queue
  private current: Question | null = null;
  private queue: Question[] = [];

  constructor(private readonly url: string) {
    this.pet = new Pet();
    this.screens = new ScreenManager({
      onChange: (s) => logLine(`◎ screen → ${s}`),
    });
    this.threads = new ThreadListView({
      onSelect: (threadId) => this.sendFollowThread(threadId),
    });
    renderUsage(null);
    renderInfo(null);
  }

  start(): void {
    this.connect();
  }
  reconnect(): void {
    this.cleanup();
    this.connect();
  }

  // ---------- WS plumbing ----------

  private connect(): void {
    setConnState('connecting');
    logLine(`→ connecting ${this.url}`);
    const ws = new WebSocket(this.url);
    this.ws = ws;

    ws.addEventListener('open', () => {
      logLine('✓ socket open, sending hello');
      this.send({
        v: PROTOCOL_VERSION,
        type: 'hello',
        seq: this.nextSeq(),
        ts: 0,
        device_id: DEVICE_ID,
        fw_version: FW_VERSION,
        pairing_token: PAIRING_TOKEN,
      });
    });
    ws.addEventListener('message', (ev) => this.onRaw(String(ev.data)));
    ws.addEventListener('close', (ev) => {
      logLine(`✗ closed ${ev.code} ${ev.reason}`);
      setConnState('offline');
      this.scheduleReconnect();
    });
    ws.addEventListener('error', () => logLine('✗ socket error'));
  }

  private scheduleReconnect(): void {
    if (this.reconnectTimer) return;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, RECONNECT_DELAY_MS);
  }
  private cleanup(): void {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.ws) {
      try { this.ws.close(); } catch { /* ignore */ }
      this.ws = null;
    }
  }

  private nextSeq(): number { return this.outSeq++; }
  private localTs(): number { return Math.round(performance.now()); }

  private send(msg: DeviceToBridge): void {
    const ws = this.ws;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify(msg));
    updateSeq(msg.seq);
  }

  private onRaw(raw: string): void {
    let parsed: unknown;
    try { parsed = JSON.parse(raw); } catch { logLine(`✗ non-JSON: ${raw.slice(0, 80)}`); return; }
    const result = parseFromBridge(parsed);
    if (!result.ok) { logLine(`✗ rejected: ${result.error}`); return; }
    this.dispatch(result.value);
  }

  private dispatch(msg: AnyMessage): void {
    switch (msg.type) {
      case 'welcome':       return this.onWelcome(msg as Welcome);
      case 'status':        return this.onStatus(msg as Status);
      case 'question':      return this.onQuestion(msg as Question);
      case 'usage':         return this.onUsage(msg as Usage);
      case 'session_info':  return this.onSessionInfo(msg as SessionInfo);
      case 'thread_list':   return this.onThreadList(msg as ThreadList);
      case 'error':         return this.onError(msg as ErrorMsg);
      case 'ping':
        this.send({
          v: PROTOCOL_VERSION, type: 'pong',
          seq: this.nextSeq(), ts: this.localTs(),
          pong_for_seq: msg.seq,
        });
        return;
      case 'ack':
        logLine(`← ack for seq=${msg.ack_for_seq}`);
        return;
    }
  }

  // ---------- message handlers ----------

  private onWelcome(msg: Welcome): void {
    this.serverTimeAnchor = msg.server_time_ms;
    this.localAnchorMs = performance.now();
    setConnState('ready');
    setDeviceLabel(`${DEVICE_ID} · ${msg.session_id}`);
    logLine(`← welcome session=${msg.session_id} features=${msg.features.join(',')}`);
    // entering ready → make sure pet is showing
    if (!this.current) this.screens.setScreen('main');
  }

  private onStatus(msg: Status): void {
    logLine(`← status ${msg.state} "${msg.title}"`);
    this.pet.setState(msg.state, msg.title, msg.body);
  }

  private onUsage(msg: Usage): void {
    // Merge with previous snapshot so partial updates don't blank out the screen.
    this.lastUsage = {
      ...this.lastUsage,
      ...msg,
      session: { ...(this.lastUsage?.session ?? {}), ...(msg.session ?? {}) },
      limits: msg.limits ?? this.lastUsage?.limits,
      plan_type: msg.plan_type ?? this.lastUsage?.plan_type,
    } as Usage;
    renderUsage(this.lastUsage);
    logLine(`← usage tokens=${this.lastUsage.session?.total_tokens ?? '?'}`);
  }

  private onSessionInfo(msg: SessionInfo): void {
    this.lastInfo = { ...(this.lastInfo ?? {} as SessionInfo), ...msg };
    renderInfo(this.lastInfo);
    if (this.lastInfo.thread_title) {
      this.pet.setThreadTitle(this.lastInfo.thread_title);
    }
    logLine(`← session_info model=${this.lastInfo.model ?? '?'} src=${this.lastInfo.source ?? '?'} title="${this.lastInfo.thread_title ?? ''}"`);
  }

  private onThreadList(msg: ThreadList): void {
    this.threads.update(msg.threads);
    // Also update pet's thread title from whichever entry is marked active.
    const active = msg.threads.find((t) => t.active);
    if (active?.title) this.pet.setThreadTitle(active.title);
    logLine(`← thread_list n=${msg.threads.length}${active ? ` active="${active.title}"` : ''}`);
  }

  private onQuestion(msg: Question): void {
    const remainingMs = msg.expires_at - this.serverTimeAnchor;
    logLine(`← question ${msg.id} risk=${msg.risk} expires in ${Math.round(remainingMs / 1000)}s queue_total=${msg.queue_total ?? '?'}`);

    if (this.current) {
      // already showing one — enqueue and just refresh badge
      this.queue.push(msg);
      this.refreshQueueBadge();
      return;
    }
    this.current = msg;
    this.screens.setScreen('main');
    renderQuestion(msg, {
      onCommit: (choiceId) => this.commitReply(choiceId),
      queueTotal: this.effectiveQueueTotal(msg),
    });
  }

  private onError(msg: ErrorMsg): void {
    logLine(`← error ${msg.code} "${msg.title}"`);
    if (msg.code === 'request_expired' && msg.related_id) {
      // drop matching question from queue or current
      if (this.current?.id === msg.related_id) {
        this.current = null;
        hideQuestion();
        this.pet.setState('error', '请求已过期', '请回到电脑');
        this.advanceQueue();
      } else {
        const before = this.queue.length;
        this.queue = this.queue.filter((q) => q.id !== msg.related_id);
        if (this.queue.length !== before) this.refreshQueueBadge();
      }
      return;
    }
    // generic error → flash on pet
    this.pet.setState('error', msg.title, msg.body ?? msg.code);
  }

  // ---------- queue ----------

  private effectiveQueueTotal(msg: Question): number {
    // Prefer Bridge-supplied total; fall back to local count.
    const local = 1 + this.queue.length;
    return Math.max(msg.queue_total ?? 0, local);
  }

  private refreshQueueBadge(): void {
    if (!this.current) return;
    renderQuestion(this.current, {
      onCommit: (choiceId) => this.commitReply(choiceId),
      queueTotal: this.effectiveQueueTotal(this.current),
    });
  }

  private sendFollowThread(threadId: string): void {
    this.send({
      v: PROTOCOL_VERSION,
      type: 'follow_thread',
      seq: this.nextSeq(),
      ts: this.localTs(),
      thread_id: threadId,
    });
    logLine(`→ follow_thread ${threadId.slice(0, 8)}`);
  }

  private commitReply(choiceId: string): void {
    if (!this.current) return;
    const q = this.current;
    this.send({
      v: PROTOCOL_VERSION,
      type: 'reply',
      seq: this.nextSeq(),
      ts: this.localTs(),
      id: q.id,
      choice_id: choiceId,
      device_id: DEVICE_ID,
    });
    logLine(`→ reply ${q.id} = ${choiceId}`);
    this.current = null;
    hideQuestion();
    this.advanceQueue();
  }

  private advanceQueue(): void {
    const next = this.queue.shift();
    if (!next) return;
    // small visual breath before showing the next one
    setTimeout(() => {
      this.current = next;
      this.screens.setScreen('main');
      renderQuestion(next, {
        onCommit: (choiceId) => this.commitReply(choiceId),
        queueTotal: this.effectiveQueueTotal(next),
      });
    }, 500);
  }
}

// ---------- UI bits ----------

function setConnState(state: 'offline' | 'connecting' | 'ready'): void {
  const el = document.getElementById('conn-state');
  if (!el) return;
  el.className = `badge ${state}`;
  el.textContent = state;
}
function setDeviceLabel(text: string): void {
  const el = document.getElementById('device-id');
  if (el) el.textContent = `device: ${text}`;
}
function updateSeq(n: number): void {
  const el = document.getElementById('seq-info');
  if (el) el.textContent = `seq: ${n}`;
}

// ---------- bootstrap ----------

const wsUrl = new URL('/device', location.href);
wsUrl.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
const client = new SimulatorClient(wsUrl.toString());
client.start();

document.getElementById('btn-reconnect')?.addEventListener('click', () => client.reconnect());
