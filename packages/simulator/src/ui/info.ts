/**
 * Far-left screen: session metadata pulled from rollout's session_meta /
 * turn_context plus Codex account info. Mirrors the panel Codex Desktop
 * shows you (account / model / dir / perms / session id / rate-limit plan).
 */
import type { SessionInfo } from '@doudou/device-protocol';

import { applyMarquee } from './marquee.js';

export function renderInfo(info: SessionInfo | null): void {
  const host = document.getElementById('slide-info-content') as HTMLElement;
  host.replaceChildren();

  if (!info || isEmpty(info)) {
    host.appendChild(empty('暂无会话\n(在 Codex Desktop\n 里打开一段对话)'));
    return;
  }

  // Thread title — the headline (above everything else).
  if (info.thread_title) {
    const block = section();
    const titleEl = lineEmpty();
    block.appendChild(titleEl);
    applyMarquee(titleEl, info.thread_title);
    host.appendChild(block);
  }

  // Account block
  if (info.account_email || info.plan_type) {
    const block = section();
    if (info.account_email) {
      const el = lineEmpty();
      applyMarquee(el, info.account_email);
      block.appendChild(el);
    }
    const planLine = [info.plan_type?.toUpperCase(), info.collaboration_mode ? `· ${capitalize(info.collaboration_mode)}` : '']
      .filter(Boolean).join(' ');
    if (planLine) block.appendChild(faint(planLine));
    host.appendChild(block);
  }

  // Model + reasoning block
  if (info.model || info.reasoning_effort || info.summary_mode) {
    const block = section();
    if (info.model) block.appendChild(line(info.model));
    const subs = [
      info.reasoning_effort ? `推理 ${info.reasoning_effort}` : null,
      info.summary_mode ? `摘要 ${info.summary_mode}` : null,
    ].filter(Boolean).join('  ·  ');
    if (subs) block.appendChild(faint(subs));
    host.appendChild(block);
  }

  // Permissions + workspace
  if (info.permissions || info.cwd) {
    const block = section();
    if (info.permissions) block.appendChild(line(info.permissions));
    if (info.cwd) {
      const el = faintEmpty();
      applyMarquee(el, `📁 ${shortDir(info.cwd)}`);
      block.appendChild(el);
    }
    if (info.agents_md !== undefined) {
      block.appendChild(faint(`AGENTS.md ${info.agents_md ? '✓' : '—'}`));
    }
    host.appendChild(block);
  }

  // Source + session id
  if (info.source || info.session_id) {
    const block = section();
    if (info.source) block.appendChild(faint(`来自 ${info.source}`));
    if (info.session_id) block.appendChild(faint(`id ${shortId(info.session_id)}`));
    host.appendChild(block);
  }
}

function lineEmpty(): HTMLElement {
  const el = document.createElement('div');
  el.className = 'info-line';
  return el;
}
function faintEmpty(): HTMLElement {
  const el = document.createElement('div');
  el.className = 'info-faint';
  return el;
}

function isEmpty(i: SessionInfo): boolean {
  return Object.keys(i).filter((k) => k !== 'v' && k !== 'type' && k !== 'seq' && k !== 'ts').length === 0;
}

function section(): HTMLElement {
  const el = document.createElement('div');
  el.className = 'info-block';
  return el;
}

function line(text: string): HTMLElement {
  const el = document.createElement('div');
  el.className = 'info-line';
  el.textContent = text;
  return el;
}

function faint(text: string): HTMLElement {
  const el = document.createElement('div');
  el.className = 'info-faint';
  el.textContent = text;
  return el;
}

function empty(text: string): HTMLElement {
  const e = document.createElement('div');
  e.className = 'history-empty';
  e.textContent = text;
  e.style.whiteSpace = 'pre-line';
  return e;
}

function emailShort(e: string): string {
  if (e.length <= 22) return e;
  const at = e.indexOf('@');
  if (at < 0) return e.slice(0, 22) + '…';
  return e.slice(0, Math.max(8, at)) + '…@' + e.slice(at + 1, at + 12);
}

function capitalize(s: string): string {
  return s[0] ? s[0].toUpperCase() + s.slice(1) : s;
}

function shortDir(p: string): string {
  // Replace HOME with ~ and keep last 2 segments when long.
  const home = '/Users/';
  let s = p;
  if (p.startsWith(home)) {
    const after = p.slice(home.length);
    const slash = after.indexOf('/');
    s = slash < 0 ? '~' : '~' + after.slice(slash);
  }
  const parts = s.split('/').filter(Boolean);
  if (parts.length <= 2) return s;
  return '…/' + parts.slice(-2).join('/');
}

function shortId(id: string): string {
  // UUIDs are long; show first 8 chars.
  return id.length > 12 ? id.slice(0, 8) : id;
}
