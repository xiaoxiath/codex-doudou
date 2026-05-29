/**
 * squash.c — single-paddle wall pong.
 *
 * Ball bounces off top + side walls; the player keeps it alive with a
 * paddle that tracks the finger along the bottom. Miss = game over.
 *
 * Round-screen layout: walls inset 16 px from the edge so the play
 * area sits inside the visible circle (the corners are masked anyway).
 * Paddle at y=200, height 10, width 60 — wide enough to be tappable
 * even when the round mask trims its endpoints visually.
 */
#include "squash.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "lv_font_cjk_14.h"
#include "lvgl_port.h"
#include "touch.h"

static const char *TAG = "squash";

#define GAME_W        240
#define GAME_H        240
/* Walls inset so the 160×160 play box fits inside the inscribed
 * circle (radius 120). Corner of the box at (40, 40) is √(80²+80²) ≈
 * 113 from screen centre — well inside the bezel. */
#define WALL_L        40
#define WALL_R        (GAME_W - 40)   /* 200 */
#define WALL_T        50              /* below the score banner */
#define FLOOR_Y       200             /* miss line — below paddle */

#define BALL_R        7
#define PADDLE_W      60
#define PADDLE_H      10
#define PADDLE_Y      180

#define TICK_MS       33
#define BALL_SPEED0   3.0f             /* px/tick at start */
#define SPEEDUP       0.10f            /* per successful hit */
#define MAX_BALL_VX   6.5f

static lv_obj_t   *s_root        = NULL;
static lv_obj_t   *s_ball        = NULL;
static lv_obj_t   *s_paddle      = NULL;
static lv_obj_t   *s_score_label = NULL;
static lv_obj_t   *s_hint_label  = NULL;
static lv_timer_t *s_tick        = NULL;

static bool   s_active   = false;
static bool   s_gameover = false;

/* game state */
static float  s_ball_x, s_ball_y;
static float  s_ball_vx, s_ball_vy;
static float  s_paddle_x;        /* left edge */
static int    s_score = 0;
static int    s_best  = 0;

static int randint(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(esp_random() % (uint32_t)(hi - lo));
}

static void reset_ball(void)
{
    s_ball_x = GAME_W / 2;
    s_ball_y = GAME_H / 2 - 20;
    /* Random horizontal direction; downward start so the ball heads
     * for the paddle. Vertical component fixed so difficulty is
     * predictable on round 1. */
    int sign = (esp_random() & 1) ? 1 : -1;
    s_ball_vx = sign * (BALL_SPEED0 * 0.65f);
    s_ball_vy = BALL_SPEED0;
}

static void reset_game(void)
{
    s_score = 0;
    s_gameover = false;
    s_paddle_x = (GAME_W - PADDLE_W) / 2;
    reset_ball();
}

static void apply_positions(void)
{
    if (s_ball) {
        lv_obj_set_pos(s_ball, (int)s_ball_x - BALL_R, (int)s_ball_y - BALL_R);
    }
    if (s_paddle) {
        lv_obj_set_pos(s_paddle, (int)s_paddle_x, PADDLE_Y);
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

/* Read the finger horizontally; if pressed, snap paddle centre to it. */
static void poll_paddle_input(void)
{
    doudou_touch_event_t e = {0};
    if (doudou_touch_read(&e) != ESP_OK) return;
    if (!e.pressed) return;
    int target = (int)e.x - PADDLE_W / 2;
    if (target < WALL_L) target = WALL_L;
    if (target > WALL_R - PADDLE_W) target = WALL_R - PADDLE_W;
    s_paddle_x = (float)target;
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    if (!s_gameover) poll_paddle_input();
    if (s_gameover) { apply_positions(); return; }

    /* physics */
    s_ball_x += s_ball_vx;
    s_ball_y += s_ball_vy;

    /* side walls */
    if (s_ball_x - BALL_R < WALL_L)  { s_ball_x = WALL_L + BALL_R;  s_ball_vx = -s_ball_vx; }
    if (s_ball_x + BALL_R > WALL_R)  { s_ball_x = WALL_R - BALL_R;  s_ball_vx = -s_ball_vx; }
    /* ceiling */
    if (s_ball_y - BALL_R < WALL_T)  { s_ball_y = WALL_T + BALL_R;  s_ball_vy = -s_ball_vy; }

    /* paddle collision: ball is descending into paddle's row + x range */
    if (s_ball_vy > 0
        && s_ball_y + BALL_R >= PADDLE_Y
        && s_ball_y - BALL_R <= PADDLE_Y + PADDLE_H
        && s_ball_x >= s_paddle_x
        && s_ball_x <= s_paddle_x + PADDLE_W) {
        s_score++;
        s_ball_y = PADDLE_Y - BALL_R;
        s_ball_vy = -s_ball_vy;
        /* English: where you hit on the paddle nudges horizontal angle */
        float rel = (s_ball_x - (s_paddle_x + PADDLE_W / 2)) / (PADDLE_W / 2.0f);
        s_ball_vx += rel * 1.2f;
        if (s_ball_vx >  MAX_BALL_VX) s_ball_vx =  MAX_BALL_VX;
        if (s_ball_vx < -MAX_BALL_VX) s_ball_vx = -MAX_BALL_VX;
        /* gentle speed-up */
        float gain = 1.0f + SPEEDUP;
        s_ball_vy *= gain;
        if (s_ball_vy >  MAX_BALL_VX) s_ball_vy =  MAX_BALL_VX;
        if (s_ball_vy < -MAX_BALL_VX) s_ball_vy = -MAX_BALL_VX;
    }

    /* miss line */
    if (s_ball_y - BALL_R > FLOOR_Y) {
        apply_positions();
        trigger_gameover();
        return;
    }

    apply_positions();
}

/* ---- restart on tap after game over: called from touch task ----
 * Touch task already routes single-tap → squash via the wrapper
 * doudou_squash_tap() (see below) when squash is active. */

esp_err_t doudou_squash_init(struct _lv_obj_t *pet_screen)
{
    if (!pet_screen) return ESP_ERR_INVALID_ARG;
    if (s_root) return ESP_OK;
    if (!doudou_lvgl_lock(500)) return ESP_ERR_TIMEOUT;

    s_root = lv_obj_create(pet_screen);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, GAME_W, GAME_H);
    lv_obj_set_pos(s_root, -10, -10);                            /* cancel SAFE_INSET */
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x101216), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    /* Top wall — thin white line just below the score, visually
     * frames the playfield so it's obvious where the ball will bounce. */
    lv_obj_t *top = lv_obj_create(s_root);
    lv_obj_remove_style_all(top);
    lv_obj_set_pos(top, WALL_L, WALL_T - 2);
    lv_obj_set_size(top, WALL_R - WALL_L, 2);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x3a4154), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);

    /* Ball */
    s_ball = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_ball);
    lv_obj_set_size(s_ball, BALL_R * 2, BALL_R * 2);
    lv_obj_set_style_radius(s_ball, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ball, lv_color_hex(0xffe066), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ball, LV_OPA_COVER, LV_PART_MAIN);

    /* Paddle */
    s_paddle = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_paddle);
    lv_obj_set_size(s_paddle, PADDLE_W, PADDLE_H);
    lv_obj_set_style_radius(s_paddle, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_paddle, lv_color_hex(0x3ec27a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_paddle, LV_OPA_COVER, LV_PART_MAIN);

    /* Score — top center */
    s_score_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_score_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_score_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_score_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_score_label, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(s_score_label, "0");

    /* Hint banner */
    s_hint_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_hint_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_hint_label, "drag to play\nhold to exit");
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "squash ready (overlay on PET screen)");
    return ESP_OK;
}

void doudou_squash_start(void)
{
    if (s_active) return;
    if (!s_root) return;
    if (!doudou_lvgl_lock(200)) return;

    reset_game();
    apply_positions();
    show_hint("drag to play\nhold to exit", 0xe6e9ee);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);

    if (!s_tick) s_tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    s_active = true;

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "started");
}

void doudou_squash_stop(void)
{
    if (!s_active) return;
    if (!doudou_lvgl_lock(200)) return;

    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_active = false;
    s_gameover = false;

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "stopped");
}

bool doudou_squash_active(void)
{
    return s_active;
}

/* Tap handler — restart on game-over. The first finger-down during
 * play is already consumed by poll_paddle_input(), so a "tap" while
 * playing is effectively a no-op (paddle just jumps to that x and
 * stays). Hide the start-banner on the first tap so the playfield
 * is clean. */
void doudou_squash_tap(void);   /* fwd-decl for main.c */
void doudou_squash_tap(void)
{
    if (!s_active) return;
    if (!doudou_lvgl_lock(50)) return;
    if (s_gameover) {
        reset_game();
        hide_hint();
        apply_positions();
    } else {
        hide_hint();
    }
    doudou_lvgl_unlock();
}
