#!/usr/bin/env bash
# Regenerate the firmware's CJK font subset.
#
# Default coverage: GB2312 一级 (3755 most-common Chinese chars) + ASCII.
# Hits ~99% of Chinese the device sees from Codex (thread titles,
# error messages, command output snippets).
#
# Why not a Unicode range like 0x4E00-0x5CAB?
#   The CJK ideograph block is sorted by Kangxi radical, NOT frequency.
#   "0x4E00 + 3755 codepoints" leaves you with 啟劵 but no 成网工设系统
#   etc. — exactly the high-frequency chars you actually need. So we
#   enumerate the real GB2312 一级 codepoints via Python's codec table
#   instead.
#
# Requires:
#   * `lv_font_conv` (npm install -g lv_font_conv)
#   * Python 3 (bundled with macOS)
#   * A TTF/OTF source font that includes Chinese — `Arial Unicode.ttf`
#     is bundled with macOS.
set -euo pipefail

FONT_SRC="${FONT_SRC:-/Library/Fonts/Arial Unicode.ttf}"
SIZE="${SIZE:-14}"
BPP="${BPP:-2}"
OUT="${OUT:-$(dirname "$0")/../packages/firmware/main/font_cjk/lv_font_cjk_14.c}"
SYM_FILE="${SYM_FILE:-$(mktemp -t doudou_cjk_chars.XXXXXX)}"

if ! command -v lv_font_conv >/dev/null; then
  echo "lv_font_conv not found. Install with:  npm install -g lv_font_conv" >&2
  exit 1
fi

python3 - <<'PY' > "$SYM_FILE"
# GB2312 一级 lives at hi byte 0xB0..0xD7. Decoded → 3755 unique chars.
import sys
chars = set()
for hi in range(0xB0, 0xD8):
    for lo in range(0xA1, 0xFF):
        try:
            chars.add(bytes([hi, lo]).decode('gb2312'))
        except UnicodeDecodeError:
            pass
sys.stdout.write(''.join(sorted(chars)))
PY

count=$(python3 -c "print(len(open('$SYM_FILE','r',encoding='utf-8').read()))")
echo "covering $count Chinese chars + ASCII 0x20-0x7E"

mkdir -p "$(dirname "$OUT")"
lv_font_conv \
  --font "$FONT_SRC" \
  --size "$SIZE" \
  --bpp "$BPP" \
  --format lvgl \
  --no-compress \
  --lv-include lvgl.h \
  --lv-font-name lv_font_cjk_14 \
  --range 0x20-0x7E \
  --symbols "$(cat "$SYM_FILE")" \
  -o "$OUT"

rm -f "$SYM_FILE"
src_kb=$(($(wc -c < "$OUT") / 1024))
echo "wrote $OUT (${src_kb} KB source — compiles to ~$((src_kb / 8)) KB binary)"
