/**
 * feed.h — feed-the-doudou interaction.
 *
 * Mini-doudou sits in the middle of the screen. A food token sits at
 * the bottom, draggable. When you drop a food on doudou's mouth he
 * eats it, the eye flicks to "happy", and a new food spawns. Counter
 * top-centre. Long-press to exit.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

struct _lv_obj_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t doudou_feed_init(struct _lv_obj_t *pet_screen);
void      doudou_feed_start(void);
void      doudou_feed_stop(void);
bool      doudou_feed_active(void);

#ifdef __cplusplus
}
#endif
