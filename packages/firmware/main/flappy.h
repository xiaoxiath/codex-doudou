/**
 * flappy.h — minimal Flappy-Bird port for the doudou hardware.
 *
 * Lives as an overlay on top of the PET screen. Started/stopped from
 * main.c's touch task in offline / toy mode only — the device IS the
 * toy then, so a small game is welcome; in online mode the PET screen
 * is reserved for Codex status display.
 *
 * Game loop runs from an lv_timer at ~30 FPS. All public functions
 * acquire the LVGL lock internally.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-time setup. Creates the hidden game container as a child of
 * `pet_screen` and parks it in front of the pet sprites. */
esp_err_t doudou_flappy_init(lv_obj_t *pet_screen);

/* Show the overlay + reset + start the tick timer. No-op if already
 * running. */
void doudou_flappy_start(void);

/* Stop the tick timer + hide the overlay. The pet underneath is
 * unchanged so the kid lands right back where they were. */
void doudou_flappy_stop(void);

/* Feed a tap from the touch task: jumps the bird, or restarts after
 * Game Over. No-op when the game is not active. */
void doudou_flappy_flap(void);

bool doudou_flappy_active(void);

#ifdef __cplusplus
}
#endif
