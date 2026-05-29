/**
 * feed.c — feed-the-doudou.
 *
 * Reuses pet_ui's exact face layout (body 200×200 centred, eyes +
 * mouth as children at the same offsets) so doudou faces the kid
 * head-on. Food token sits below the body and can be dragged up to
 * the mouth. On contact: count + briefly swap eyes/mouth to happy
 * + respawn the food at home.
 */
#include "feed.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "lv_font_cjk_14.h"
#include "lvgl_port.h"
#include "pet_art.h"
#include "touch.h"

static const char *TAG = "feed";

#define BG_COLOR         0x0e1418
#define BODY_W           200
#define BODY_H           200
/* The body sits centred on a 240×240 screen → top-left (20, 20).
 * Face feature coords below are relative to body's local 200×200
 * space (children of s_body), copied from pet_ui.c so the face
 * proportions match the main pet exactly. */
#define EYE_L_X          48
#define EYE_R_X          104
#define EYE_Y            84
#define MOUTH_X          ((BODY_W - 48) / 2)
#define MOUTH_Y          120

/* Mouth bbox in screen coords (body offset (20,20) + face-relative). */
#define MOUTH_CX_SCREEN  (20 + 100)
#define MOUTH_CY_SCREEN  (20 + MOUTH_Y + 16)
#define MOUTH_HIT_R      36

#define FOOD_R           14
#define FOOD_HOME_X      120
#define FOOD_HOME_Y      218
#define TICK_MS          30

static lv_obj_t   *s_root        = NULL;
static lv_obj_t   *s_body        = NULL;
static lv_obj_t   *s_eye_l       = NULL;
static lv_obj_t   *s_eye_r       = NULL;
static lv_obj_t   *s_mouth       = NULL;
static lv_obj_t   *s_food        = NULL;
static lv_obj_t   *s_count_label = NULL;
static lv_obj_t   *s_hint_label  = NULL;
static lv_timer_t *s_tick        = NULL;

static bool   s_active   = false;
static bool   s_dragging = false;
static int    s_food_x   = FOOD_HOME_X;
static int    s_food_y   = FOOD_HOME_Y;
static int    s_count    = 0;
static int64_t s_happy_until_us = 0;

static const uint32_t FOOD_PALETTE[] = {
    0xff8c5a,   /* carrot */
    0xff5a8c,   /* berry  */
    0xffd35a,   /* cookie */
    0x6acf6a,   /* apple  */
};

static void update_food_pos(void)
{
    if (s_food) lv_obj_set_pos(s_food, s_food_x - FOOD_R, s_food_y - FOOD_R);
}

static void update_count_label(void)
{
    if (s_count_label) lv_label_set_text_fmt(s_count_label, "%d", s_count);
}

static void show_hint(const char *t)
{
    if (!s_hint_label) return;
    lv_label_set_text(s_hint_label, t);
    lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
}
static void hide_hint(void)
{
    if (s_hint_label) lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
}

static void set_eyes(const lv_image_dsc_t *eye)
{
    if (s_eye_l) lv_image_set_src(s_eye_l, eye);
    if (s_eye_r) lv_image_set_src(s_eye_r, eye);
}

static void set_mouth(const lv_image_dsc_t *m)
{
    if (s_mouth) lv_image_set_src(s_mouth, m);
}

static void respawn_food(void)
{
    s_food_x = FOOD_HOME_X;
    s_food_y = FOOD_HOME_Y;
    uint32_t c = FOOD_PALETTE[esp_random() % (sizeof(FOOD_PALETTE)/sizeof(FOOD_PALETTE[0]))];
    if (s_food) lv_obj_set_style_bg_color(s_food, lv_color_hex(c), LV_PART_MAIN);
    update_food_pos();
}

static bool food_over_mouth(void)
{
    int dx = s_food_x - MOUTH_CX_SCREEN;
    int dy = s_food_y - MOUTH_CY_SCREEN;
    return dx * dx + dy * dy < MOUTH_HIT_R * MOUTH_HIT_R;
}

static void on_feed(void)
{
    s_count++;
    update_count_label();
    set_eyes(&doudou_eye_happy);
    set_mouth(&doudou_mouth_grin);
    s_happy_until_us = esp_timer_get_time() + 700 * 1000;
    hide_hint();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    /* Revert "happy" face after the celebration window. */
    if (s_happy_until_us > 0 && esp_timer_get_time() > s_happy_until_us) {
        s_happy_until_us = 0;
        set_eyes(&doudou_eye_normal);
        set_mouth(&doudou_mouth_smile);
    }

    doudou_touch_event_t e = {0};
    if (doudou_touch_read(&e) != ESP_OK) return;
    if (!e.pressed) {
        if (s_dragging) {
            if (food_over_mouth()) on_feed();
            respawn_food();
            s_dragging = false;
        }
        return;
    }
    if (e.x >= 240 || e.y >= 240) return;

    if (!s_dragging) {
        int dx = (int)e.x - s_food_x, dy = (int)e.y - s_food_y;
        int r_pick = FOOD_R + 10;
        if (dx * dx + dy * dy < r_pick * r_pick) {
            s_dragging = true;
            hide_hint();
        } else {
            return;
        }
    }
    s_food_x = e.x;
    s_food_y = e.y;
    update_food_pos();
}

esp_err_t doudou_feed_init(struct _lv_obj_t *pet_screen)
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

    /* Body — centred on the 240×240 root → top-left (20, 20). */
    s_body = lv_image_create(s_root);
    lv_image_set_src(s_body, &doudou_body_glossy);
    lv_obj_center(s_body);

    /* Eyes + mouth as children of the body, using the same offsets as
     * the main pet_ui face. Position is body-local (untransformed). */
    s_eye_l = lv_image_create(s_body);
    lv_image_set_src(s_eye_l, &doudou_eye_normal);
    lv_obj_set_pos(s_eye_l, EYE_L_X, EYE_Y);

    s_eye_r = lv_image_create(s_body);
    lv_image_set_src(s_eye_r, &doudou_eye_normal);
    lv_obj_set_pos(s_eye_r, EYE_R_X, EYE_Y);

    s_mouth = lv_image_create(s_body);
    lv_image_set_src(s_mouth, &doudou_mouth_smile);
    lv_obj_set_pos(s_mouth, MOUTH_X, MOUTH_Y);

    /* Food pill — parented to root, drawn after body so it sits on top. */
    s_food = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_food);
    lv_obj_set_size(s_food, FOOD_R * 2, FOOD_R * 2);
    lv_obj_set_style_radius(s_food, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_food, lv_color_hex(FOOD_PALETTE[0]), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_food, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_food, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_food, lv_color_hex(0x442200), LV_PART_MAIN);
    lv_obj_clear_flag(s_food, LV_OBJ_FLAG_CLICKABLE);
    update_food_pos();

    /* Count badge — top centre, small dark pill so it doesn't fight
     * the doudou face for visual focus. */
    s_count_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_count_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_count_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(s_count_label, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(s_count_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_count_label, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_count_label, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_count_label, 6, LV_PART_MAIN);
    lv_label_set_text(s_count_label, "0");

    s_hint_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(s_hint_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_hint_label, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_hint_label, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_hint_label, 6, LV_PART_MAIN);
    lv_label_set_text(s_hint_label, "拖食物给我");
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_CLICKABLE);

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "feed ready");
    return ESP_OK;
}

void doudou_feed_start(void)
{
    if (s_active) return;
    if (!s_root) return;
    if (!doudou_lvgl_lock(200)) return;
    s_count = 0;
    s_dragging = false;
    update_count_label();
    set_eyes(&doudou_eye_normal);
    set_mouth(&doudou_mouth_smile);
    respawn_food();
    show_hint("拖食物给我");
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    if (!s_tick) s_tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    s_active = true;
    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "started");
}

void doudou_feed_stop(void)
{
    if (!s_active) return;
    if (!doudou_lvgl_lock(200)) return;
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_active = false;
    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "stopped count=%d", s_count);
}

bool doudou_feed_active(void)
{
    return s_active;
}
