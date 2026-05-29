/**
 * doodle.h — finger-painting scratchpad.
 *
 * Drag your finger to draw a colored trail. Double-tap clears the
 * canvas. Long-press exits (handled in main.c). The "canvas" is a
 * FIFO ring of small lv_obj dots — when it's full the oldest dot
 * gets recycled, capping heap use.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

struct _lv_obj_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t doudou_doodle_init(struct _lv_obj_t *pet_screen);
void      doudou_doodle_start(void);
void      doudou_doodle_stop(void);
bool      doudou_doodle_active(void);
/* main.c calls this on a synthesized double-tap to clear the canvas. */
void      doudou_doodle_clear(void);

#ifdef __cplusplus
}
#endif
