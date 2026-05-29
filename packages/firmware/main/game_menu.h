/**
 * game_menu.h — vertical-scroll game list rendered inside the GAMES
 * screen. Tap a row to switch to PET and start that game.
 *
 * Lives as a child of pet_ui's GAMES screen rather than a modal
 * overlay — that way it follows the standard swipe navigation and
 * benefits from LVGL's built-in scroll affordances.
 */
#pragma once

#include "esp_err.h"

struct _lv_obj_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Build the list as a child of the GAMES screen. Call once during
 * pet_ui init, after the screen itself exists. */
esp_err_t doudou_game_menu_build(struct _lv_obj_t *games_screen);

#ifdef __cplusplus
}
#endif
