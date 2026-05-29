/**
 * whack.h — 3x3 whack-a-mole for the 240x240 round LCD.
 *
 * Each hole is its own LVGL clickable obj; the game listens to LVGL
 * click events rather than the raw touch task, so missed taps (in
 * between holes) just do nothing. Long-press exits via the touch
 * task (see main.c).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

struct _lv_obj_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t doudou_whack_init(struct _lv_obj_t *pet_screen);
void      doudou_whack_start(void);
void      doudou_whack_stop(void);
bool      doudou_whack_active(void);

#ifdef __cplusplus
}
#endif
