/**
 * Right-swipe screen: live activity stream from the followed Codex thread.
 * Pulls from Bridge /api/activity every 5s, plus on-demand bumps.
 */
interface ActivityEntry {
  ts: string;
  kind: 'user' | 'agent' | 'task_complete';
  text?: string;
  thread_id: string;
  source?: string;
}

export class HistoryView {
  private host: HTMLElement;
  private timer: ReturnType<typeof setInterval> | null = null;
  private lastFetch = 0;

  constructor() {
    this.host = document.getElementById('slide-history-content') as HTMLElement;
    this.render([], 'loading');
  }

  start(): void {
    void this.refresh();
    this.timer = setInterval(() => void this.refresh(), 5_000);
  }
  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }
  /** Force a quick refresh — useful after a status event indicates new activity. */
  bump(): void {
    setTimeout(() => void this.refresh(), 300);
  }

  private async refresh(): Promise<void> {
    if (Date.now() - this.lastFetch < 400) return;
    this.lastFetch = Date.now();
    try {
      const r = await fetch('/api/activity?n=12');
      if (!r.ok) {
        this.render([], 'error');
        return;
      }
      const json = (await r.json()) as { entries?: ActivityEntry[] };
      this.render(json.entries ?? [], 'ok');
    } catch {
      this.render([], 'error');
    }
  }

  private render(entries: ActivityEntry[], state: 'ok' | 'loading' | 'error'): void {
    this.host.replaceChildren();
    if (state === 'loading') {
      this.host.appendChild(empty('加载中…'));
      return;
    }
    if (state === 'error') {
      this.host.appendChild(empty('Bridge 不可达'));
      return;
    }
    if (entries.length === 0) {
      this.host.appendChild(
        empty('暂无对话\n(在 Codex Desktop\n 里聊点什么)'),
      );
      return;
    }
    const heading = document.createElement('div');
    heading.className = 'usage-heading';
    heading.textContent = '会话活动';
    heading.style.width = '100%';
    this.host.appendChild(heading);

    const ul = document.createElement('ul');
    ul.className = 'history-list';
    for (const e of entries.slice(0, 8)) {
      const li = document.createElement('li');
      li.className = 'history-item';

      const dot = document.createElement('span');
      dot.className = `history-dot ${kindColor(e.kind)}`;

      const text = document.createElement('div');
      text.className = 'history-text';
      const top = document.createElement('div');
      top.textContent = `${kindEmoji(e.kind)} ${labelFor(e)}`;
      const meta = document.createElement('div');
      meta.className = 'history-meta';
      meta.textContent = shortTs(e.ts);
      text.append(top, meta);

      li.append(dot, text);
      ul.appendChild(li);
    }
    this.host.appendChild(ul);
  }
}

function empty(text: string): HTMLElement {
  const e = document.createElement('div');
  e.className = 'history-empty';
  e.textContent = text;
  e.style.whiteSpace = 'pre-line';
  return e;
}
function shortTs(iso: string): string {
  const d = new Date(iso);
  const hh = d.getHours().toString().padStart(2, '0');
  const mm = d.getMinutes().toString().padStart(2, '0');
  return `${hh}:${mm}`;
}
function kindEmoji(k: ActivityEntry['kind']): string {
  if (k === 'user') return '👤';
  if (k === 'agent') return '🤖';
  return '✓';
}
function kindColor(k: ActivityEntry['kind']): string {
  if (k === 'user') return 'low';        // green
  if (k === 'agent') return 'medium';    // amber
  return 'low';
}
function labelFor(e: ActivityEntry): string {
  if (e.kind === 'task_complete') return '任务完成';
  const clip = (s: string, n: number) => (s.length <= n ? s : s.slice(0, n - 1) + '…');
  // Strip newlines and code-fences so the one-line preview reads cleanly.
  const flat = (e.text ?? '').replace(/```[\s\S]*?```/g, '〔code〕').replace(/\s+/g, ' ').trim();
  return clip(flat, 28);
}
