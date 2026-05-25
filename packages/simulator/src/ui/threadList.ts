/**
 * Right-swipe screen: list of recent Codex threads (with AI-generated
 * titles), pulled from `thread_list` WS messages. Active thread bolded
 * and marked with a small dot.
 */
import type { ThreadSummary } from '@doudou/device-protocol';

import { applyMarquee } from './marquee.js';

export interface ThreadListHandlers {
  onSelect?: (threadId: string) => void;
}

export class ThreadListView {
  private host: HTMLElement;
  private last: ThreadSummary[] = [];
  private handlers: ThreadListHandlers = {};

  constructor(handlers: ThreadListHandlers = {}) {
    this.host = document.getElementById('slide-history-content') as HTMLElement;
    this.handlers = handlers;
    this.render([]);
  }

  update(threads: ThreadSummary[]): void {
    this.last = threads;
    this.render(threads);
  }

  private render(threads: ThreadSummary[]): void {
    this.host.replaceChildren();

    if (threads.length === 0) {
      const e = document.createElement('div');
      e.className = 'history-empty';
      e.textContent = '暂无会话\n(等待 thread/list 数据…)';
      e.style.whiteSpace = 'pre-line';
      this.host.appendChild(e);
      return;
    }

    const heading = document.createElement('div');
    heading.className = 'usage-heading';
    heading.style.width = '100%';
    heading.textContent = '会话列表';
    this.host.appendChild(heading);

    const ul = document.createElement('ul');
    ul.className = 'history-list thread-list';
    for (const t of threads.slice(0, 8)) {
      const li = document.createElement('li');
      li.className = `history-item${t.active ? ' active' : ''} clickable`;
      // Tap to follow that thread on the Bridge side.
      li.addEventListener('click', () => {
        if (t.active) return; // already active, no-op
        this.handlers.onSelect?.(t.id);
      });

      const dot = document.createElement('span');
      dot.className = `history-dot ${t.active ? 'high' : 'low'}`;

      const text = document.createElement('div');
      text.className = 'history-text';

      // Title line — apply marquee if overflow
      const titleEl = document.createElement('div');
      titleEl.className = 'thread-title' + (t.active ? ' active' : '');
      applyMarquee(titleEl, t.title || t.id.slice(0, 8));
      text.appendChild(titleEl);

      // Meta line: source · relative time
      const meta = document.createElement('div');
      meta.className = 'history-meta';
      meta.textContent = [t.source ?? '', t.updated_at ? relTime(t.updated_at) : '']
        .filter(Boolean)
        .join(' · ');
      text.appendChild(meta);

      li.append(dot, text);
      ul.appendChild(li);
    }
    this.host.appendChild(ul);
  }
}

function relTime(epochMs: number): string {
  const diff = Date.now() - epochMs;
  if (diff < 60_000) return '刚刚';
  if (diff < 3_600_000) return `${Math.round(diff / 60_000)}m`;
  if (diff < 86_400_000) return `${Math.round(diff / 3_600_000)}h`;
  return `${Math.round(diff / 86_400_000)}d`;
}
