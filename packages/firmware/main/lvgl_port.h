/**
 * lvgl_port.h — LVGL v9 integration glue.
 *
 * Owns: the LVGL tick timer, the LVGL handler task, the display + indev
 * registration. After `doudou_lvgl_init()` returns, the rest of the app
 * can create lv_obj_t widgets normally — as long as it holds the LVGL
 * lock (`doudou_lvgl_lock` / `doudou_lvgl_unlock`) around any LVGL call.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize LVGL + register display flush + touch indev + start tasks.
 *  Call AFTER doudou_display_init() and doudou_touch_init(). */
esp_err_t doudou_lvgl_init(void);

/** Lock LVGL for the calling task. Mandatory before any lv_*** call from
 *  outside the lvgl handler task. Blocks up to `timeout_ms` (use -1 for forever). */
bool doudou_lvgl_lock(int timeout_ms);
void doudou_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
