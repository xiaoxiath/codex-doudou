/**
 * Left-swipe screen: token usage + rate-limit windows.
 * Renders into #slide-usage-content. Idempotent — call any time data changes.
 */
import type { Usage } from '@doudou/device-protocol';

export function renderUsage(usage: Usage | null): void {
  const host = document.getElementById('slide-usage-content') as HTMLElement;
  host.replaceChildren();

  if (!usage || (!usage.session && !usage.limits?.length)) {
    const e = document.createElement('div');
    e.className = 'usage-empty';
    e.textContent = '尚无用量数据\n(在 real 模式下发送一次 prompt 后会出现)';
    e.style.whiteSpace = 'pre-line';
    host.appendChild(e);
    return;
  }

  if (usage.session) {
    // ---- Block A: current context occupancy ----
    const ctxBlock = document.createElement('div');
    ctxBlock.className = 'usage-block';
    const h1 = document.createElement('div');
    h1.className = 'usage-heading';
    h1.textContent = '上下文剩余';
    ctxBlock.appendChild(h1);

    const ctx = usage.session.model_context_window;
    const current = usage.session.current_context_tokens;
    if (ctx && ctx > 0 && current !== undefined) {
      const remaining = Math.max(0, ctx - current);
      const remainPct = clampPct((remaining / ctx) * 100);
      ctxBlock.appendChild(row(`${remainPct.toFixed(0)}%`, `${fmtN(remaining)} / ${fmtN(ctx)}`));
      ctxBlock.appendChild(remainingBar(remainPct));
    } else if (ctx) {
      ctxBlock.appendChild(row('窗口', fmtN(ctx)));
    } else {
      ctxBlock.appendChild(row('—', '尚未开始'));
    }
    host.appendChild(ctxBlock);

    // ---- Block B: cumulative session tokens (compact, no breakdown) ----
    const totalBilled = usage.session.total_tokens;
    if (totalBilled !== undefined && totalBilled > 0) {
      const billBlock = document.createElement('div');
      billBlock.className = 'usage-block';
      const h2 = document.createElement('div');
      h2.className = 'usage-heading';
      h2.textContent = '会话累计';
      billBlock.appendChild(h2);
      billBlock.appendChild(row('tokens', fmtN(totalBilled)));
      host.appendChild(billBlock);
    }
  }

  if (usage.limits?.length) {
    // Group limits by group_label. Undefined = main plan.
    // Filter out the Spark variant — user only cares about the main
    // plan quota, the Spark line is just noise.
    const grouped = new Map<string | undefined, typeof usage.limits>();
    for (const l of usage.limits) {
      const key = l.group_label;
      if (key && /spark/i.test(key)) continue;
      const arr = grouped.get(key) ?? [];
      arr.push(l);
      grouped.set(key, arr);
    }
    // Render main group first.
    const orderedKeys = [...grouped.keys()].sort((a, b) =>
      a === undefined ? -1 : b === undefined ? 1 : a.localeCompare(b),
    );
    for (const key of orderedKeys) {
      const limits = grouped.get(key) ?? [];
      const block = document.createElement('div');
      block.className = 'usage-block';
      const heading = document.createElement('div');
      heading.className = 'usage-heading';
      heading.textContent = key
        ? `${shortenModelLabel(key)} · 剩余`
        : (usage.plan_type ? `${usage.plan_type.toUpperCase()} · 剩余` : '配额剩余');
      block.appendChild(heading);
      for (const l of limits) {
        const label = l.label ?? l.id;
        const used = l.used_pct ?? 0;
        const remainPct = clampPct(100 - used);
        const right = `${remainPct.toFixed(0)}%${l.resets_at ? `  ${untilHM(l.resets_at)}` : ''}`;
        block.appendChild(row(label, right));
        block.appendChild(remainingBar(remainPct));
      }
      host.appendChild(block);
    }
  }
}

/** Shorten long model names like "GPT-5.3-Codex-Spark" → "Spark" when possible. */
function shortenModelLabel(s: string): string {
  // Heuristic: take last dash-separated token if it's short and meaningful.
  const parts = s.split(/[-_\s]+/).filter(Boolean);
  const last = parts[parts.length - 1];
  if (last && last.length <= 8 && last.length >= 3) return last;
  // Otherwise truncate.
  return s.length <= 14 ? s : s.slice(0, 13) + '…';
}

function remainingBar(remainPct: number): HTMLElement {
  // Warn when remaining is low (we're almost out).
  const cls =
    remainPct < 10 ? ' crit' : remainPct < 30 ? ' warn' : '';
  const bar = document.createElement('div');
  bar.className = `usage-bar${cls}`;
  const fill = document.createElement('span');
  fill.style.width = `${remainPct}%`;
  bar.appendChild(fill);
  return bar;
}

function clampPct(n: number): number {
  if (Number.isNaN(n)) return 0;
  return Math.min(100, Math.max(0, n));
}

function row(left: string, right: string): HTMLElement {
  const r = document.createElement('div');
  r.className = 'usage-row';
  const a = document.createElement('span');
  a.textContent = left;
  const b = document.createElement('span');
  b.textContent = right;
  r.append(a, b);
  return r;
}
function fmtN(n: number | undefined): string {
  if (n === undefined) return '—';
  if (n < 1000) return String(n);
  if (n < 1000_000) return `${(n / 1000).toFixed(1)}k`;
  return `${(n / 1000_000).toFixed(2)}M`;
}
/**
 * Resets-at formatter — show the WALL-CLOCK reset moment in the user's
 * local timezone, not a countdown. "今天 / 明天" prefix for adjacent
 * days; date prefix for anything further out.
 */
function untilHM(epochMs: number): string {
  const target = new Date(epochMs);
  const now = new Date();
  if (target.getTime() <= now.getTime()) return '即将重置';

  const pad2 = (n: number) => String(n).padStart(2, '0');
  const hhmm = `${pad2(target.getHours())}:${pad2(target.getMinutes())}`;

  // Day delta in calendar days (not 24h chunks).
  const startOfToday    = new Date(now.getFullYear(),    now.getMonth(),    now.getDate()).getTime();
  const startOfTarget   = new Date(target.getFullYear(), target.getMonth(), target.getDate()).getTime();
  const daysAhead = Math.round((startOfTarget - startOfToday) / 86_400_000);

  if (daysAhead === 0) return `今天 ${hhmm}`;
  if (daysAhead === 1) return `明天 ${hhmm}`;
  if (daysAhead <= 6) {
    const weekday = ['日', '一', '二', '三', '四', '五', '六'][target.getDay()];
    return `周${weekday} ${hhmm}`;
  }
  return `${target.getMonth() + 1}月${target.getDate()}日 ${hhmm}`;
}
