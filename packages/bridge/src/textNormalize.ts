/**
 * textNormalize.ts — fold full-width / CJK punctuation that isn't in
 * the device's GB2312 一级 font subset down to ASCII equivalents.
 *
 * Why: extending the font subset costs ~5–10 KB of flash per few dozen
 * glyphs, and our OTA slot has ~1% headroom. ASCII fallbacks on a
 * 240×240 round LCD with ≤10-char titles are visually fine. Done at
 * the device send boundary (`deviceConnection.rawSend`) so it's a
 * single chokepoint — every outgoing field, every message type.
 *
 * Rules of thumb:
 *  - Keep the meaning, lose the typographic flourish.
 *  - Don't touch CJK letters — those ARE in the subset.
 *  - Pure ASCII strings: fast-path, no allocation.
 */
const PUNCT_MAP: Record<string, string> = {
  // CJK sentence-ending / list / pause
  '。': '.',   // 。
  '，': ',',   // ，
  '、': ',',   // 、
  '；': ';',   // ；
  '：': ':',   // ：
  '！': '!',   // ！
  '？': '?',   // ？
  // Brackets / quotes
  '「': '"',   // 「
  '」': '"',   // 」
  '『': '"',   // 『
  '』': '"',   // 』
  '《': '<',   // 《
  '》': '>',   // 》
  '〈': '<',   // 〈
  '〉': '>',   // 〉
  '（': '(',   // （
  '）': ')',   // ）
  '［': '[',   // ［
  '］': ']',   // ］
  '｛': '{',   // ｛
  '｝': '}',   // ｝
  '【': '[',   // 【
  '】': ']',   // 】
  // Smart / curly quotes
  '‘': "'",   // ‘
  '’': "'",   // ’
  '“': '"',   // “
  '”': '"',   // ”
  // Dashes / ellipsis
  '—': '-',   // — (em dash; pair "——" → "--" via per-char map)
  '–': '-',   // – (en dash)
  '…': '...', // …
  '·': '.',   // ·
  '・': '.',   // ・
  // Full-width digits / latin (kept rare — collapse for safety)
  '～': '~',   // ～
  '／': '/',   // ／
  '＼': '\\',  // ＼
  '｜': '|',   // ｜
  '＆': '&',   // ＆
  '＠': '@',   // ＠
  '＃': '#',   // ＃
  '＄': '$',   // ＄
  '％': '%',   // ％
  '＊': '*',   // ＊
  '＋': '+',   // ＋
  '＝': '=',   // ＝
  '＜': '<',   // ＜
  '＞': '>',   // ＞
};

const NEEDS_NORMALIZE_RE = /[‐-‧　-〿・＀-～·]/;

export function normalizeForDevice(s: string): string {
  if (!NEEDS_NORMALIZE_RE.test(s)) return s;
  let out = '';
  for (const ch of s) {
    out += PUNCT_MAP[ch] ?? ch;
  }
  return out;
}

/**
 * Deep-walk the outbound JSON envelope, normalizing every string value
 * in place. Numbers, booleans, arrays-of-objects, nested structures
 * (e.g. `usage.limits[].label`, `question.choices[].label`) are all
 * covered with one pass — no per-field allowlist to maintain.
 */
export function normalizeEnvelope<T>(value: T): T {
  if (typeof value === 'string') {
    return normalizeForDevice(value) as unknown as T;
  }
  if (Array.isArray(value)) {
    for (let i = 0; i < value.length; i++) {
      value[i] = normalizeEnvelope(value[i]);
    }
    return value;
  }
  if (value && typeof value === 'object') {
    const obj = value as Record<string, unknown>;
    for (const k of Object.keys(obj)) {
      obj[k] = normalizeEnvelope(obj[k]);
    }
    return value;
  }
  return value;
}
