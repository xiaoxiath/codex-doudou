/**
 * simon.h — colour-sequence memory game.
 *
 * 4 quadrants light up in a growing sequence; the kid plays them
 * back in the same order. Each successful round adds one step.
 * Wrong tap → game over, score = sequence length reached.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

struct _lv_obj_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t doudou_simon_init(struct _lv_obj_t *pet_screen);
void      doudou_simon_start(void);
void      doudou_simon_stop(void);
bool      doudou_simon_active(void);

#ifdef __cplusplus
}
#endif
