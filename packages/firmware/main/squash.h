/**
 * squash.h — single-player wall-pong for the 240x240 round LCD.
 *
 * Same lifecycle pattern as flappy.h: hidden overlay on the PET screen,
 * tick timer at ~30 FPS, all public functions take the LVGL lock
 * internally. Paddle is controlled by dragging your finger along the
 * panel — the game polls doudou_touch_read() inside the tick. No
 * explicit "flap" callback is needed.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

struct _lv_obj_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t doudou_squash_init(struct _lv_obj_t *pet_screen);
void      doudou_squash_start(void);
void      doudou_squash_stop(void);
bool      doudou_squash_active(void);
/* Forward a tap from the touch task — restarts the round after a miss
 * and hides the "drag to play" banner on first tap. */
void      doudou_squash_tap(void);

#ifdef __cplusplus
}
#endif
