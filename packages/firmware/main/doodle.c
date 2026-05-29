/**
 * doodle.c — touch-drag finger painting on a 240x240 round LCD.
 *
 * Rendering: a FIFO ring of small (4-5 px) circular lv_objs. Each tick
 * we sample doudou_touch_read(); if the finger is pressed and has
 * moved far enough from the last sample we spawn a new dot. When the
 * ring is full the oldest dot is recycled — keeps heap bounded.
 *
 * Color cycles every few seconds so a single stroke can shade as
 * you draw, giving kids a rainbow effect for free.
 */
#include "doodle.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "lv_font_cjk_14.h"
#include "lvgl_port.h"
#include "touch.h"

static const char *TAG = "doodle";

#define MAX_DOTS       100
#define DOT_R          3
#define DOT_DIAM       (DOT_R * 2)
#define MIN_STEP_PX    5          /* min distance between consecutive dots */
#define MAX_STEPS_TICK 12         /* cap interpolated dots placed per tick */
#define TICK_MS        33
#define BG_COLOR       0x14171c   /* matches the doudou system bg */

static lv_obj_t   *s_root        = NULL;
static lv_obj_t   *s_hint_label  = NULL;
static lv_timer_t *s_tick        = NULL;
static lv_obj_t   *s_dots[MAX_DOTS];
static int         s_next_slot   = 0;   /* circular index for FIFO eviction */
static bool        s_active      = false;
static int         s_last_x      = -1;
static int         s_last_y      = -1;

/* Rotating palette — every ~150 ticks (3 s) the colour advances. */
static const uint32_t PALETTE[] = {
    0xffd83d, 0xff6b9d, 0x7ec7ff, 0xa8e6cf,
    0xffaaa5, 0xd7baff, 0xffffff, 0x3ec27a,
};
#define N_COLORS (int)(sizeof(PALETTE) / sizeof(PALETTE[0]))
static int s_color_idx = 0;
static int s_tick_count = 0;

static void hide_hint(void)
{
    if (s_hint_label) lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
}

static void show_hint(const char *t)
{
    if (!s_hint_label) return;
    lv_label_set_text(s_hint_label, t);
    lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_dot(int x, int y, uint32_t color)
{
    lv_obj_t *d = lv_obj_create(s_root);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, DOT_DIAM, DOT_DIAM);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(d, x - DOT_R, y - DOT_R);
    /* Dots must not steal touch from doudou's other widgets. */
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    return d;
}

static void place_dot(int x, int y)
{
    uint32_t color = PALETTE[s_color_idx];
    int slot = s_next_slot;
    s_next_slot = (s_next_slot + 1) % MAX_DOTS;
    if (s_dots[slot]) {
        /* Reuse the oldest dot by repositioning + recolouring. */
        lv_obj_set_pos(s_dots[slot], x - DOT_R, y - DOT_R);
        lv_obj_set_style_bg_color(s_dots[slot], lv_color_hex(color), LV_PART_MAIN);
    } else {
        s_dots[slot] = make_dot(x, y, color);
    }
}

static void interpolate(int x0, int y0, int x1, int y1)
{
    /* Bridge the gap between two touch samples — at 50 Hz polling +
     * fast finger movement the gap can be 20-30 px, leaving visible
     * dashes. Step every MIN_STEP_PX along the line so the trail
     * stays continuous. */
    int dx = x1 - x0, dy = y1 - y0;
    int dist2 = dx * dx + dy * dy;
    if (dist2 < MIN_STEP_PX * MIN_STEP_PX) {
        place_dot(x1, y1);
        return;
    }
    /* approx int sqrt */
    int dist = 1;
    while (dist * dist < dist2) dist++;
    int steps = dist / MIN_STEP_PX;
    if (steps < 1) steps = 1;
    if (steps > MAX_STEPS_TICK) steps = MAX_STEPS_TICK;
    for (int i = 1; i <= steps; i++) {
        int x = x0 + (dx * i) / steps;
        int y = y0 + (dy * i) / steps;
        place_dot(x, y);
    }
}

void doudou_doodle_clear(void)
{
    if (!s_active) return;
    if (!doudou_lvgl_lock(100)) return;
    for (int i = 0; i < MAX_DOTS; i++) {
        if (s_dots[i]) { lv_obj_delete(s_dots[i]); s_dots[i] = NULL; }
    }
    s_next_slot = 0;
    s_last_x = -1;
    s_last_y = -1;
    show_hint("拖动画画\n双击清空\nhold to exit");
    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "cleared");
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;
    s_tick_count++;
    if ((s_tick_count % 150) == 0) {
        s_color_idx = (s_color_idx + 1) % N_COLORS;
    }
    doudou_touch_event_t e = {0};
    if (doudou_touch_read(&e) != ESP_OK) return;
    if (!e.pressed) { s_last_x = -1; s_last_y = -1; return; }
    if (e.x >= 240 || e.y >= 240) return;

    if (s_last_x < 0) {
        place_dot(e.x, e.y);
        hide_hint();
    } else {
        int dx = (int)e.x - s_last_x, dy = (int)e.y - s_last_y;
        if (dx * dx + dy * dy >= MIN_STEP_PX * MIN_STEP_PX) {
            interpolate(s_last_x, s_last_y, e.x, e.y);
        }
    }
    s_last_x = e.x;
    s_last_y = e.y;
}

esp_err_t doudou_doodle_init(struct _lv_obj_t *pet_screen)
{
    if (!pet_screen) return ESP_ERR_INVALID_ARG;
    if (s_root) return ESP_OK;
    if (!doudou_lvgl_lock(500)) return ESP_ERR_TIMEOUT;

    s_root = lv_obj_create(pet_screen);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 240, 240);
    lv_obj_set_pos(s_root, -10, -10);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    s_hint_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x8a909c), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_hint_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_hint_label, "拖动画画\n双击清空\nhold to exit");
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_CLICKABLE);

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "doodle ready");
    return ESP_OK;
}

void doudou_doodle_start(void)
{
    if (s_active) return;
    if (!s_root) return;
    if (!doudou_lvgl_lock(200)) return;
    s_next_slot = 0;
    s_last_x = -1;
    s_last_y = -1;
    s_color_idx = (int)(esp_random() % (uint32_t)N_COLORS);
    show_hint("拖动画画\n双击清空\nhold to exit");
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    if (!s_tick) s_tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    s_active = true;
    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "started");
}

void doudou_doodle_stop(void)
{
    if (!s_active) return;
    if (!doudou_lvgl_lock(200)) return;
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    for (int i = 0; i < MAX_DOTS; i++) {
        if (s_dots[i]) { lv_obj_delete(s_dots[i]); s_dots[i] = NULL; }
    }
    s_next_slot = 0;
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_active = false;
    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "stopped");
}

bool doudou_doodle_active(void)
{
    return s_active;
}
