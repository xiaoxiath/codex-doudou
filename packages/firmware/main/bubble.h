/**
 * bubble.h — bubble-pop game for the 240x240 round LCD.
 *
 * Bubbles drift up from the bottom; tap one to pop it. Missed bubbles
 * float out the top and are lost (no penalty). Endless / score-based.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

struct _lv_obj_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t doudou_bubble_init(struct _lv_obj_t *pet_screen);
void      doudou_bubble_start(void);
void      doudou_bubble_stop(void);
bool      doudou_bubble_active(void);

#ifdef __cplusplus
}
#endif
