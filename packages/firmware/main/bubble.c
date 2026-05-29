/**
 * bubble.c — bubble-pop game.
 *
 * Up to MAX_BUBBLES live bubbles at a time. Each is a translucent
 * circular lv_obj with its own LV_EVENT_CLICKED handler — LVGL
 * dispatches taps to the bubble at the touched coords, so we don't
 * need to do hit-testing in app code.
 */
#include "bubble.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "lv_font_cjk_14.h"
#include "lvgl_port.h"

static const char *TAG = "bubble";

#define MAX_BUBBLES   6
#define MIN_R         12
#define MAX_R         22
#define MIN_VY        0.7f
#define MAX_VY        1.6f
#define SPAWN_MS      650        /* base spawn interval — jittered */
#define TICK_MS       33
#define BG_COLOR      0x081428   /* deep dusk-blue */

typedef struct {
    lv_obj_t *obj;
    float     x, y;
    float     vy;
    int       r;
    bool      alive;
} bubble_t;

static lv_obj_t   *s_root        = NULL;
static lv_obj_t   *s_score_label = NULL;
static lv_obj_t   *s_hint_label  = NULL;
static lv_timer_t *s_tick        = NULL;

static bubble_t s_bubbles[MAX_BUBBLES];
static bool     s_active = false;
static int      s_score  = 0;
static int      s_best   = 0;
static int64_t  s_next_spawn_us = 0;

static int randint(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(esp_random() % (uint32_t)(hi - lo));
}

static float randf(float lo, float hi)
{
    return lo + ((float)(esp_random() & 0xffff) / 65535.0f) * (hi - lo);
}

static void show_hint(const char *text)
{
    if (!s_hint_label) return;
    lv_label_set_text(s_hint_label, text);
    lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
}
static void hide_hint(void)
{
    if (s_hint_label) lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
}

static void destroy_bubble(int i)
{
    if (i < 0 || i >= MAX_BUBBLES) return;
    if (s_bubbles[i].obj) {
        lv_obj_delete(s_bubbles[i].obj);
        s_bubbles[i].obj = NULL;
    }
    s_bubbles[i].alive = false;
}

static void on_bubble_click(lv_event_t *e)
{
    if (!s_active) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MAX_BUBBLES) return;
    if (!s_bubbles[idx].alive) return;
    destroy_bubble(idx);
    s_score++;
    if (s_score > s_best) s_best = s_score;
    if (s_score_label) lv_label_set_text_fmt(s_score_label, "%d", s_score);
    hide_hint();
}

static bool circles_overlap(int x, int y, int r)
{
    /* Avoid spawning a new bubble directly on top of an existing one. */
    for (int i = 0; i < MAX_BUBBLES; i++) {
        if (!s_bubbles[i].alive) continue;
        int dx = (int)s_bubbles[i].x - x;
        int dy = (int)s_bubbles[i].y - y;
        int sum = s_bubbles[i].r + r + 4;
        if (dx * dx + dy * dy < sum * sum) return true;
    }
    return false;
}

static void spawn_bubble(void)
{
    int slot = -1;
    for (int i = 0; i < MAX_BUBBLES; i++) {
        if (!s_bubbles[i].alive) { slot = i; break; }
    }
    if (slot < 0) return;

    int r = randint(MIN_R, MAX_R);
    /* Tight x range so the bubble stays inside the inscribed circle
     * as it drifts up. At y≈20 (near the top of the screen) a bubble
     * at x=180 sits √(60²+100²) ≈ 117 px from screen centre — still
     * inside the 120-px round mask. Wider spawn x would have bubbles
     * disappearing behind the bezel before they reached the top. */
    int x = randint(60, 180);
    int y = 240 + r;
    int tries = 0;
    while (circles_overlap(x, y, r) && ++tries < 6) {
        x = randint(60, 180);
    }

    lv_obj_t *b = lv_obj_create(s_root);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, r * 2, r * 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    /* Soft pastel colour, semi-transparent so overlap looks bubbly. */
    static const uint32_t palette[] = {
        0x7ec7ff, 0xa8e6cf, 0xffd3b6, 0xffaaa5, 0xd7baff, 0xfff5a8,
    };
    uint32_t color = palette[esp_random() % (sizeof(palette)/sizeof(palette[0]))];
    lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_border_opa(b, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_pos(b, x - r, y - r);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_add_event_cb(b, on_bubble_click, LV_EVENT_CLICKED,
                        (void *)(intptr_t)slot);

    s_bubbles[slot].obj = b;
    s_bubbles[slot].x   = x;
    s_bubbles[slot].y   = y;
    s_bubbles[slot].vy  = randf(MIN_VY, MAX_VY);
    s_bubbles[slot].r   = r;
    s_bubbles[slot].alive = true;
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;
    int64_t now = esp_timer_get_time();

    /* Drift bubbles up, destroy when they exit the top. */
    for (int i = 0; i < MAX_BUBBLES; i++) {
        if (!s_bubbles[i].alive) continue;
        s_bubbles[i].y -= s_bubbles[i].vy;
        if (s_bubbles[i].y + s_bubbles[i].r < 0) {
            destroy_bubble(i);
            continue;
        }
        if (s_bubbles[i].obj) {
            lv_obj_set_pos(s_bubbles[i].obj,
                           (int)s_bubbles[i].x - s_bubbles[i].r,
                           (int)s_bubbles[i].y - s_bubbles[i].r);
        }
    }

    /* Spawn on schedule, with some jitter. */
    if (now >= s_next_spawn_us) {
        spawn_bubble();
        s_next_spawn_us = now + (int64_t)randint(SPAWN_MS - 200, SPAWN_MS + 200) * 1000;
    }
}

esp_err_t doudou_bubble_init(struct _lv_obj_t *pet_screen)
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

    s_score_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_score_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_score_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(s_score_label, LV_ALIGN_TOP_MID, 0, 14);
    lv_label_set_text(s_score_label, "0");

    s_hint_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_hint_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_hint_label, "戳泡泡\nhold to exit");
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_CLICKABLE);

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "bubble ready");
    return ESP_OK;
}

void doudou_bubble_start(void)
{
    if (s_active) return;
    if (!s_root) return;
    if (!doudou_lvgl_lock(200)) return;

    s_score = 0;
    if (s_score_label) lv_label_set_text(s_score_label, "0");
    for (int i = 0; i < MAX_BUBBLES; i++) destroy_bubble(i);
    s_next_spawn_us = esp_timer_get_time() + 300 * 1000;
    show_hint("戳泡泡\nhold to exit");
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    if (!s_tick) s_tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    s_active = true;

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "started (best=%d)", s_best);
}

void doudou_bubble_stop(void)
{
    if (!s_active) return;
    if (!doudou_lvgl_lock(200)) return;
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    for (int i = 0; i < MAX_BUBBLES; i++) destroy_bubble(i);
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_active = false;
    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "stopped score=%d best=%d", s_score, s_best);
}

bool doudou_bubble_active(void)
{
    return s_active;
}
