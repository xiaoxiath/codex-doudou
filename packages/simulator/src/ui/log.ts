const MAX_LINES = 200;

export function logLine(text: string): void {
  const el = document.getElementById('log');
  if (!el) return;
  const stamp = new Date().toLocaleTimeString('zh-CN', { hour12: false });
  el.textContent = `[${stamp}] ${text}\n${el.textContent ?? ''}`;
  const lines = (el.textContent ?? '').split('\n');
  if (lines.length > MAX_LINES) {
    el.textContent = lines.slice(0, MAX_LINES).join('\n');
  }
}
