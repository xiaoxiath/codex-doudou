/**
 * whack.c — whack-a-mole for the 240x240 round LCD.
 *
 * Layout:
 *   3x3 grid of holes, cell centres at {60, 120, 180}. With hole
 *   radius 22 every cell fits inside the 240-px round visible area
 *   (corner cells worst-case extend to ~(38,38) — distance 116 from
 *   centre, still inside the radius-120 mask).
 *
 * Each hole is an lv_obj with LV_EVENT_CLICKED — the game does not
 * need to forward taps from the touch task. The touch task's tap
 * dispatch still skips this game (doudou_whack_active gate in
 * main.c) so a missed tap doesn't spuriously trigger wiggle.
 */
#include "whack.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "lv_font_cjk_14.h"
#include "lvgl_port.h"

static const char *TAG = "whack";

#define N_HOLES        9
#define HOLE_R         22
#define HOLE_DIAM      (HOLE_R * 2)
#define MOLE_LIFE_US   ((int64_t)1500 * 1000)
#define SPAWN_MIN_US   ((int64_t)300 * 1000)
#define SPAWN_MAX_US   ((int64_t)800 * 1000)
#define TICK_MS        30

static const int CELL_CX[3] = {60, 120, 180};
static const int CELL_CY[3] = {60, 120, 180};

static lv_obj_t   *s_root        = NULL;
static lv_obj_t   *s_holes[N_HOLES];
static lv_obj_t   *s_score_label = NULL;
static lv_obj_t   *s_hint_label  = NULL;
static lv_timer_t *s_tick        = NULL;

static bool    s_active = false;
static int     s_active_hole = -1;     /* index of the currently-visible mole, -1 = none */
static int64_t s_active_until_us = 0;
static int64_t s_next_spawn_us = 0;
static int     s_score = 0;
static int     s_best  = 0;

/* Colour palette */
#define BG_COLOR     0x2d2a24    /* dirt brown */
#define HOLE_COLOR   0x110a05    /* deep hole */
#define MOLE_COLOR   0xffc14d    /* yellow doudou-style mole */
#define MOLE_BORDER  0x6b4400

static int randint(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(esp_random() % (uint32_t)(hi - lo));
}

static int64_t random_spawn_delay_us(void)
{
    return randint((int)SPAWN_MIN_US, (int)SPAWN_MAX_US);
}

static void update_score_label(void)
{
    if (s_score_label) lv_label_set_text_fmt(s_score_label, "%d", s_score);
}

static void show_hint(const char *text, uint32_t color)
{
    if (!s_hint_label) return;
    lv_label_set_text(s_hint_label, text);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
}

static void hide_hint(void)
{
    if (s_hint_label) lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
}

static void paint_hole(int idx, bool active)
{
    if (idx < 0 || idx >= N_HOLES || !s_holes[idx]) return;
    uint32_t bg     = active ? MOLE_COLOR  : HOLE_COLOR;
    uint32_t border = active ? MOLE_BORDER : HOLE_COLOR;
    lv_obj_set_style_bg_color(s_holes[idx], lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_holes[idx], lv_color_hex(border), LV_PART_MAIN);
}

static void hide_mole(void)
{
    if (s_active_hole >= 0) paint_hole(s_active_hole, false);
    s_active_hole = -1;
}

static void spawn_mole(void)
{
    /* Pick a hole different from the last one so consecutive spawns
     * always move — keeps the game from feeling stuck. */
    int next;
    int last = s_active_hole;
    int tries = 0;
    do {
        next = randint(0, N_HOLES);
    } while (next == last && ++tries < 6);
    s_active_hole = next;
    s_active_until_us = esp_timer_get_time() + MOLE_LIFE_US;
    paint_hole(next, true);
    hide_hint();   /* first spawn clears the start banner */
}

static void on_hole_click(lv_event_t *e)
{
    if (!s_active) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx != s_active_hole) return;          /* tapped an empty hole */
    /* Score + clear + schedule next */
    s_score++;
    if (s_score > s_best) s_best = s_score;
    update_score_label();
    hide_mole();
    s_next_spawn_us = esp_timer_get_time() + random_spawn_delay_us();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;
    int64_t now = esp_timer_get_time();
    if (s_active_hole < 0) {
        if (now >= s_next_spawn_us) spawn_mole();
    } else {
        if (now >= s_active_until_us) {
            /* Expired without a hit — no score penalty, just rotate. */
            hide_mole();
            s_next_spawn_us = now + random_spawn_delay_us();
        }
    }
}

esp_err_t doudou_whack_init(struct _lv_obj_t *pet_screen)
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

    /* 3×3 grid of clickable circular holes */
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int idx = r * 3 + c;
            lv_obj_t *h = lv_obj_create(s_root);
            lv_obj_remove_style_all(h);
            lv_obj_set_size(h, HOLE_DIAM, HOLE_DIAM);
            lv_obj_set_pos(h, CELL_CX[c] - HOLE_R, CELL_CY[r] - HOLE_R);
            lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(h, lv_color_hex(HOLE_COLOR), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(h, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(h, 3, LV_PART_MAIN);
            lv_obj_set_style_border_color(h, lv_color_hex(HOLE_COLOR), LV_PART_MAIN);
            lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
            /* Larger hit area: extend by 6 px around the circle so a
             * slightly imprecise tap still scores. */
            lv_obj_set_ext_click_area(h, 6);
            lv_obj_add_event_cb(h, on_hole_click, LV_EVENT_CLICKED,
                                (void *)(intptr_t)idx);
            s_holes[idx] = h;
        }
    }

    /* Score label top, banner centre */
    s_score_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_score_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_score_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_score_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    /* Park the score above row 0 — y=10 sits in the top mask but still
     * inside the visible circle thanks to the centred x. */
    lv_obj_align(s_score_label, LV_ALIGN_TOP_MID, 0, 14);
    lv_label_set_text(s_score_label, "0");

    s_hint_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_hint_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_hint_label, "tap moles\nhold to exit");
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    /* The hint label sits in the middle of the grid; make sure it
     * doesn't eat clicks meant for the centre hole. */
    lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_CLICKABLE);

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "whack ready (overlay on PET screen)");
    return ESP_OK;
}

void doudou_whack_start(void)
{
    if (s_active) return;
    if (!s_root) return;
    if (!doudou_lvgl_lock(200)) return;

    s_score = 0;
    s_active_hole = -1;
    s_next_spawn_us = esp_timer_get_time() + 400 * 1000;   /* short pre-roll */
    update_score_label();
    for (int i = 0; i < N_HOLES; i++) paint_hole(i, false);

    show_hint("tap moles\nhold to exit", 0xe6e9ee);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);

    if (!s_tick) s_tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    s_active = true;

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "started (best=%d)", s_best);
}

void doudou_whack_stop(void)
{
    if (!s_active) return;
    if (!doudou_lvgl_lock(200)) return;

    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    hide_mole();
    s_active = false;

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "stopped (score=%d best=%d)", s_score, s_best);
}

bool doudou_whack_active(void)
{
    return s_active;
}
