/**
 * display.h — minimal GC9A01 panel control surface for MVP-1a bring-up.
 *
 * This layer wraps the managed `esp_lcd_gc9a01` driver to expose a
 * deliberately tiny API: init the panel, blit a single solid color,
 * blit a 16bpp RGB565 buffer. LVGL integration lands in MVP-1a step 3.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t doudou_display_init(void);

/** Fill the entire panel with a single RGB565 color. */
esp_err_t doudou_display_fill(uint16_t rgb565);

/** Blit a buffer into a rectangle. Buffer is RGB565 little-endian. */
esp_err_t doudou_display_blit(int x, int y, int w, int h, const uint16_t *pixels);

/** Set the backlight duty (0..255). Off = 0, full = 255. */
esp_err_t doudou_display_backlight(uint8_t duty);

#ifdef __cplusplus
}
#endif
