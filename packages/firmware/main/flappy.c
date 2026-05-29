/**
 * flappy.c — Flappy-Bird port for the 240x240 round LCD.
 *
 * Logic mirrors tmp/ESP32-Game-Engine/examples/FlappyBird, scaled up
 * from the original 72x40 OLED to a 240x240 round screen. The round
 * mask hides ~18 px of corner on each side; pipes that run full
 * height look fine because their tops/bottoms tuck behind the bezel.
 *
 * Rendering uses plain lv_obj rectangles + a colored circle for the
 * bird — no bitmaps. On a C3 that keeps the per-frame draw cost low
 * enough to hit a smooth 30 FPS together with the LVGL pulse timer.
 */
#include "flappy.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lv_font_cjk_14.h"
#include "lvgl_port.h"

static const char *TAG = "flappy";

/* ----- play-area geometry ----- */
#define GAME_W      240
#define GAME_H      240
#define BIRD_X      60      /* fixed x; bird only moves vertically */
#define BIRD_R      8       /* circle radius */
#define PIPE_W      30
#define PIPE_GAP    72      /* vertical gap height — generous, round-screen visibility */
#define PIPE_MARGIN 40      /* keep gap_y inside [MARGIN, H - GAP - MARGIN] */
#define TICK_MS     33      /* ≈30 FPS */

/* ----- physics ----- */
#define GRAVITY     0.55f
#define JUMP_VY    -7.0f
#define PIPE_SPEED  2.4f

/* ----- LVGL widget handles (all parented to s_root) ----- */
static lv_obj_t  *s_root        = NULL;   /* hidden container, 240x240 */
static lv_obj_t  *s_bird        = NULL;
static lv_obj_t  *s_pipe_top    = NULL;
static lv_obj_t  *s_pipe_bot    = NULL;
static lv_obj_t  *s_score_label = NULL;
static lv_obj_t  *s_hint_label  = NULL;   /* "↑ to start" / "Game Over" */
static lv_timer_t *s_tick       = NULL;

/* ----- game state ----- */
static bool   s_active     = false;
static bool   s_gameover   = false;
static float  s_bird_y     = GAME_H / 2;
static float  s_bird_vy    = 0;
static float  s_pipe_x     = GAME_W;
static int    s_pipe_gap_y = 80;
static bool   s_pipe_scored = false;
static int    s_score      = 0;
static int    s_best       = 0;

/* ----- helpers ----- */

static int randint(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(esp_random() % (uint32_t)(hi - lo));
}

static void reset_game(void)
{
    s_bird_y = GAME_H / 2;
    s_bird_vy = 0;
    s_pipe_x = GAME_W;
    s_pipe_gap_y = randint(PIPE_MARGIN, GAME_H - PIPE_GAP - PIPE_MARGIN);
    s_pipe_scored = false;
    s_score = 0;
    s_gameover = false;
}

/* Caller holds the LVGL lock. */
static void apply_widget_positions(void)
{
    if (s_bird) {
        lv_obj_set_pos(s_bird, BIRD_X - BIRD_R, (int)s_bird_y - BIRD_R);
    }
    if (s_pipe_top) {
        lv_obj_set_pos(s_pipe_top, (int)s_pipe_x, 0);
        lv_obj_set_size(s_pipe_top, PIPE_W, s_pipe_gap_y);
    }
    if (s_pipe_bot) {
        lv_obj_set_pos(s_pipe_bot, (int)s_pipe_x, s_pipe_gap_y + PIPE_GAP);
        lv_obj_set_size(s_pipe_bot, PIPE_W, GAME_H - s_pipe_gap_y - PIPE_GAP);
    }
    if (s_score_label) {
        lv_label_set_text_fmt(s_score_label, "%d", s_score);
    }
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

static void trigger_gameover(void)
{
    s_gameover = true;
    if (s_score > s_best) s_best = s_score;
    char buf[48];
    snprintf(buf, sizeof(buf), "Game Over\n%d  best %d\ntap to retry", s_score, s_best);
    show_hint(buf, 0xef5350);
}

/* Game tick — runs from LVGL timer, so we already hold the lock. */
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active || s_gameover) return;

    /* physics */
    s_bird_vy += GRAVITY;
    s_bird_y  += s_bird_vy;
    s_pipe_x  -= PIPE_SPEED;

    /* pipe recycle + score */
    if (s_pipe_x + PIPE_W < 0) {
        s_pipe_x = GAME_W;
        s_pipe_gap_y = randint(PIPE_MARGIN, GAME_H - PIPE_GAP - PIPE_MARGIN);
        s_pipe_scored = false;
    }
    if (!s_pipe_scored && s_pipe_x + PIPE_W < BIRD_X - BIRD_R) {
        s_score++;
        s_pipe_scored = true;
    }

    /* collisions */
    if (s_bird_y < BIRD_R || s_bird_y > GAME_H - BIRD_R) {
        apply_widget_positions();
        trigger_gameover();
        return;
    }
    const float bx_left  = BIRD_X - BIRD_R;
    const float bx_right = BIRD_X + BIRD_R;
    const float by_top   = s_bird_y - BIRD_R;
    const float by_bot   = s_bird_y + BIRD_R;
    const float px_left  = s_pipe_x;
    const float px_right = s_pipe_x + PIPE_W;
    if (bx_right > px_left && bx_left < px_right) {
        if (by_top < s_pipe_gap_y || by_bot > s_pipe_gap_y + PIPE_GAP) {
            apply_widget_positions();
            trigger_gameover();
            return;
        }
    }

    apply_widget_positions();
}

/* ----- public API ----- */

esp_err_t doudou_flappy_init(lv_obj_t *pet_screen)
{
    if (!pet_screen) return ESP_ERR_INVALID_ARG;
    if (s_root) return ESP_OK;
    if (!doudou_lvgl_lock(500)) return ESP_ERR_TIMEOUT;

    /* Root container — full PET screen, dark-sky background. Sits on
     * top of all pet sprites because it's the last-created child. */
    s_root = lv_obj_create(pet_screen);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, GAME_W, GAME_H);
    /* The PET screen has SAFE_INSET padding (10 px); offset to fill
     * the underlying physical 240x240. */
    lv_obj_set_pos(s_root, -10, -10);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x1a2a44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    /* Bird — yellow filled circle */
    s_bird = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_bird);
    lv_obj_set_size(s_bird, BIRD_R * 2, BIRD_R * 2);
    lv_obj_set_style_radius(s_bird, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bird, lv_color_hex(0xffd83d), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bird, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bird, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bird, lv_color_hex(0x6b4400), LV_PART_MAIN);

    /* Pipes — green rectangles */
    const uint32_t PIPE_FG = 0x3ec27a;
    s_pipe_top = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pipe_top);
    lv_obj_set_style_bg_color(s_pipe_top, lv_color_hex(PIPE_FG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pipe_top, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pipe_top, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pipe_top, lv_color_hex(0x1f5d3a), LV_PART_MAIN);

    s_pipe_bot = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pipe_bot);
    lv_obj_set_style_bg_color(s_pipe_bot, lv_color_hex(PIPE_FG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pipe_bot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pipe_bot, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pipe_bot, lv_color_hex(0x1f5d3a), LV_PART_MAIN);

    /* Score — top-center, white */
    s_score_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_score_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_score_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_score_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_score_label, LV_ALIGN_TOP_MID, 0, 28);
    lv_label_set_text(s_score_label, "0");

    /* Hint — start / game-over banner, centered */
    s_hint_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_hint_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_hint_label, "tap to flap\nhold to exit");
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "flappy ready (overlay on PET screen)");
    return ESP_OK;
}

void doudou_flappy_start(void)
{
    if (s_active) return;
    if (!s_root) return;
    if (!doudou_lvgl_lock(200)) return;

    reset_game();
    apply_widget_positions();
    show_hint("tap to flap\nhold to exit", 0xe6e9ee);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);

    if (!s_tick) {
        s_tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    }
    s_active = true;

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "started");
}

void doudou_flappy_stop(void)
{
    if (!s_active) return;
    if (!doudou_lvgl_lock(200)) return;

    if (s_tick) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_active = false;
    s_gameover = false;

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "stopped");
}

void doudou_flappy_flap(void)
{
    if (!s_active) return;
    if (!doudou_lvgl_lock(50)) return;

    if (s_gameover) {
        reset_game();
        hide_hint();
        apply_widget_positions();
    } else {
        s_bird_vy = JUMP_VY;
        /* Hide the start-banner on first flap so the playfield is clean. */
        hide_hint();
    }
    doudou_lvgl_unlock();
}

bool doudou_flappy_active(void)
{
    return s_active;
}
