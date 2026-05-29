/**
 * game_menu.c — vertical scrollable list of games on the GAMES screen.
 *
 * Tap a row → switch to the PET screen and start that game (every
 * game runs as an overlay on PET, so the screen swap is what brings
 * the new overlay into view). Game registry is hardcoded.
 */
#include "game_menu.h"

#include "esp_log.h"
#include "lvgl.h"

#include "lv_font_cjk_14.h"
#include "lvgl_port.h"
#include "pet_ui.h"

#include "whack.h"
#include "squash.h"
#include "bubble.h"
#include "feed.h"
#include "flappy.h"
#include "simon.h"

static const char *TAG = "menu";

typedef void (*game_start_fn)(void);

typedef struct {
    const char    *label;
    game_start_fn  start;
} entry_t;

static const entry_t ENTRIES[] = {
    { "打地鼠",      doudou_whack_start  },
    { "弹球",        doudou_squash_start },
    { "戳泡泡",      doudou_bubble_start },
    { "喂 doudou",   doudou_feed_start   },
    { "小鸟飞",      doudou_flappy_start },
    { "颜色跟随",    doudou_simon_start  },
};
#define N_ENTRIES ((int)(sizeof(ENTRIES) / sizeof(ENTRIES[0])))

static void on_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= N_ENTRIES) return;
    /* The games render on the PET screen — flip back there first so
     * the overlay we're about to show is on the active screen. */
    doudou_screen_show(DOUDOU_SCREEN_PET);
    ENTRIES[idx].start();
}

esp_err_t doudou_game_menu_build(struct _lv_obj_t *games_screen)
{
    if (!games_screen) return ESP_ERR_INVALID_ARG;
    if (!doudou_lvgl_lock(500)) return ESP_ERR_TIMEOUT;

    /* Header */
    lv_obj_t *hdr = lv_label_create(games_screen);
    lv_obj_set_style_text_font(hdr, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
    lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(hdr, lv_pct(80));
    lv_label_set_text(hdr, "选个游戏");
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 14);

    /* Scrollable list panel sized to leave room for the header above
     * and the round mask top/bottom. */
    lv_obj_t *list = lv_obj_create(games_screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 180, 170);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 12);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x1d2128), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, lv_color_hex(0x2f343d), LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < N_ENTRIES; i++) {
        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_size(row, lv_pct(100), 28);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2e36), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(row);
        lv_obj_set_style_text_font(l, &lv_font_cjk_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
        lv_label_set_text(l, ENTRIES[i].label);
        lv_obj_center(l);
    }

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "game menu built (%d entries)", N_ENTRIES);
    return ESP_OK;
}
