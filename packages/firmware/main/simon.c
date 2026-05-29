/**
 * simon.c — colour-memory (Simon Says) for the 240×240 round LCD.
 *
 * Four quarter-circle quadrants (NW/NE/SW/SE), each with a colour.
 * The game plays a sequence of N flashes; the kid taps the quadrants
 * back in order. Correct → sequence grows by one; incorrect → game
 * over with score = sequence reached. Driven by lv_timer state
 * machine so flashing doesn't block other LVGL work.
 */
#include "simon.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "lv_font_cjk_14.h"
#include "lvgl_port.h"

static const char *TAG = "simon";

#define N_QUADS         4
#define MAX_SEQUENCE    32
#define BG_COLOR        0x14171c
#define FLASH_ON_MS     480
#define FLASH_GAP_MS    220
#define WRONG_FLASH_MS  900

/* Quadrant base + lit colours. Bright pastels for kid appeal. */
static const uint32_t QUAD_DIM[N_QUADS]  = { 0x3a7d3a, 0xb05050, 0x4060a8, 0xb09020 };
static const uint32_t QUAD_LIT[N_QUADS]  = { 0x6cdc6c, 0xff7878, 0x80a0ff, 0xffd84a };

typedef enum {
    PHASE_IDLE = 0,
    PHASE_PLAYBACK,    /* device is showing the sequence */
    PHASE_INPUT,       /* waiting for player taps */
    PHASE_WRONG,       /* show wrong-flash, then idle */
} phase_t;

static lv_obj_t   *s_root        = NULL;
static lv_obj_t   *s_quads[N_QUADS];
static lv_obj_t   *s_score_label = NULL;
static lv_obj_t   *s_hint_label  = NULL;
static lv_timer_t *s_tick        = NULL;

static bool    s_active = false;
static phase_t s_phase  = PHASE_IDLE;
static int     s_seq[MAX_SEQUENCE];
static int     s_seq_len    = 0;
static int     s_play_idx   = 0;    /* index into s_seq during playback */
static int     s_input_idx  = 0;    /* index into s_seq during input */
static int64_t s_phase_next_us = 0;
static bool    s_quad_lit_state = false;   /* current playback step on/off */
static int     s_best       = 0;

static void paint_quad(int i, bool lit)
{
    if (i < 0 || i >= N_QUADS) return;
    uint32_t c = lit ? QUAD_LIT[i] : QUAD_DIM[i];
    lv_obj_set_style_bg_color(s_quads[i], lv_color_hex(c), LV_PART_MAIN);
}

static void paint_all_dim(void)
{
    for (int i = 0; i < N_QUADS; i++) paint_quad(i, false);
}

static void update_score(void)
{
    if (s_score_label) lv_label_set_text_fmt(s_score_label, "%d", s_seq_len);
}

static void show_hint(const char *t, uint32_t color)
{
    if (!s_hint_label) return;
    lv_label_set_text(s_hint_label, t);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
}
static void hide_hint(void)
{
    if (s_hint_label) lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
}

static void grow_sequence_and_play(void)
{
    if (s_seq_len < MAX_SEQUENCE) {
        s_seq[s_seq_len++] = (int)(esp_random() % N_QUADS);
    }
    s_play_idx = 0;
    s_input_idx = 0;
    s_quad_lit_state = false;
    s_phase = PHASE_PLAYBACK;
    /* Brief pause then start lighting up. */
    s_phase_next_us = esp_timer_get_time() + 500 * 1000;
    paint_all_dim();
    update_score();
    hide_hint();
}

static void on_quad_click(lv_event_t *e)
{
    if (!s_active) return;
    if (s_phase != PHASE_INPUT) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= N_QUADS) return;
    /* Light the quadrant briefly as visual feedback. */
    paint_quad(idx, true);
    s_phase_next_us = esp_timer_get_time() + 180 * 1000;  /* light-up window */
    if (idx == s_seq[s_input_idx]) {
        s_input_idx++;
        if (s_input_idx >= s_seq_len) {
            /* Round cleared — grow + replay. */
            if (s_seq_len > s_best) s_best = s_seq_len;
            grow_sequence_and_play();
        }
    } else {
        /* Wrong tap → flash all red-ish then reset. */
        s_phase = PHASE_WRONG;
        s_phase_next_us = esp_timer_get_time() + WRONG_FLASH_MS * 1000;
        for (int i = 0; i < N_QUADS; i++) {
            lv_obj_set_style_bg_color(s_quads[i], lv_color_hex(0xef5350), LV_PART_MAIN);
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "Game Over\n%d  best %d", s_seq_len - 1, s_best);
        show_hint(buf, 0xef5350);
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;
    int64_t now = esp_timer_get_time();

    switch (s_phase) {
        case PHASE_PLAYBACK:
            if (now < s_phase_next_us) return;
            if (s_quad_lit_state) {
                /* End of "on" → dim it. */
                paint_quad(s_seq[s_play_idx], false);
                s_quad_lit_state = false;
                s_phase_next_us = now + FLASH_GAP_MS * 1000;
                s_play_idx++;
                if (s_play_idx >= s_seq_len) {
                    /* Playback done — wait for input. */
                    s_phase = PHASE_INPUT;
                    s_input_idx = 0;
                }
            } else {
                /* Start showing the next step. */
                paint_quad(s_seq[s_play_idx], true);
                s_quad_lit_state = true;
                s_phase_next_us = now + FLASH_ON_MS * 1000;
            }
            break;
        case PHASE_INPUT:
            /* After a tap we briefly light the tapped quad. Once
             * s_phase_next_us elapses, dim it back. */
            if (s_phase_next_us > 0 && now >= s_phase_next_us) {
                paint_all_dim();
                s_phase_next_us = 0;
            }
            break;
        case PHASE_WRONG:
            if (now < s_phase_next_us) return;
            /* Reset for another round. */
            s_seq_len = 0;
            paint_all_dim();
            grow_sequence_and_play();
            break;
        case PHASE_IDLE:
        default:
            break;
    }
}

esp_err_t doudou_simon_init(struct _lv_obj_t *pet_screen)
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

    /* Four quadrants laid out so all 4 outer corners stay inside the
     * inscribed circle (radius 120, centred at 120, 120). Square 70 px
     * positioned at (41, 41) and mirrored — TL corner at (41, 41) is
     * at distance √(79²+79²) ≈ 112 < 120. ✓ */
    struct { int x, y; } pos[N_QUADS] = {
        { 41,  41  },     /* NW (green) */
        { 129, 41  },     /* NE (red) */
        { 41,  129 },     /* SW (blue) */
        { 129, 129 },     /* SE (yellow) */
    };
    for (int i = 0; i < N_QUADS; i++) {
        lv_obj_t *q = lv_obj_create(s_root);
        lv_obj_remove_style_all(q);
        lv_obj_set_size(q, 70, 70);
        lv_obj_set_pos(q, pos[i].x, pos[i].y);
        lv_obj_set_style_radius(q, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(q, lv_color_hex(QUAD_DIM[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(q, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(q, 0, LV_PART_MAIN);
        lv_obj_add_flag(q, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(q, on_quad_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_quads[i] = q;
    }

    /* Score (top) — small white centred. Sits inside the gap between
     * the two upper quads, so we don't fight for vertical space. */
    s_score_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_score_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_score_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(s_score_label, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(s_score_label, "0");

    /* Hint (centre) */
    s_hint_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_hint_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_hint_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_hint_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_hint_label, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_hint_label, 6, LV_PART_MAIN);
    lv_label_set_text(s_hint_label, "看颜色再点");
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_CLICKABLE);

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "simon ready");
    return ESP_OK;
}

void doudou_simon_start(void)
{
    if (s_active) return;
    if (!s_root) return;
    if (!doudou_lvgl_lock(200)) return;

    s_seq_len = 0;
    paint_all_dim();
    show_hint("看颜色再点", 0xe6e9ee);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    if (!s_tick) s_tick = lv_timer_create(tick_cb, 50, NULL);
    s_active = true;
    /* Kick off round 1 after a moment so the kid sees the hint. */
    s_phase = PHASE_PLAYBACK;
    s_phase_next_us = esp_timer_get_time() + 900 * 1000;
    s_seq[s_seq_len++] = (int)(esp_random() % N_QUADS);
    s_play_idx = 0;
    s_input_idx = 0;
    s_quad_lit_state = false;
    update_score();

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "started (best=%d)", s_best);
}

void doudou_simon_stop(void)
{
    if (!s_active) return;
    if (!doudou_lvgl_lock(200)) return;
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_active = false;
    s_phase = PHASE_IDLE;
    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "stopped reached=%d best=%d", s_seq_len, s_best);
}

bool doudou_simon_active(void)
{
    return s_active;
}
