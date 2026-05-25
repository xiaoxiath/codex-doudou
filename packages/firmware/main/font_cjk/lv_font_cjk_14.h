/**
 * 14-px CJK font for the Doudou UI.
 *
 * Coverage: GB2312 一级 (the 3755 most-common Chinese characters —
 * enumerated via Python's gb2312 codec, NOT a contiguous Unicode range
 * since CJK ideographs are sorted by radical, not frequency) plus
 * ASCII 0x20-0x7E. Renders ~99% of normal Chinese text the device can
 * receive from Codex (thread titles, error messages, commit summaries…).
 *
 * Encoding: 2bpp anti-aliased — 4-level greyscale. On the 1.28" 240px
 * panel, indistinguishable from 4bpp to the eye, but ~2× smaller.
 *
 * Source: /Library/Fonts/Arial Unicode.ttf
 * Binary cost: ~195 KB of doudou.bin (out of 1.5 MB ota_0 partition).
 *
 * Regenerate via `scripts/build_font.sh` after editing the UI strings
 * or if you need to change the source font / coverage / bpp.
 *
 * Use this instead of `lv_font_montserrat_14` for any label that can
 * contain Chinese. Montserrat is Latin-only and Chinese chars fall back
 * to ▢ (missing-glyph box).
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t lv_font_cjk_14;

#ifdef __cplusplus
}
#endif
