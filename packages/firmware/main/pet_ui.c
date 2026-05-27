/**
 * pet_ui.c — LVGL widgets for the 4-screen Doudou UI.
 *
 * Pet rendering: a colored circular "body" with two eyes drawn as small
 * white circles. State changes flip the body color + brief animation;
 * the body breathes continuously via a transform-scale tween.
 *
 * All public functions are safe to call from non-LVGL tasks — they
 * acquire the LVGL lock from lvgl_port internally.
 */
#include "pet_ui.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "lv_font_cjk_14.h"

#include "lvgl_port.h"
#include "pet_art.h"
#include "pins.h"

static const char *TAG = "pet_ui";

/* ------- safe-area constants matching docs/technical-plan.md ------- */
#define SAFE_INSET    10   /* inscribed-square margin around 240×240 lens */
#define BODY_W        200  /* must match scripts/build_pet_art.py sizes  */
#define BODY_H        200

/* ------- handles ------- */
static lv_obj_t *s_screens[DOUDOU_SCREEN_COUNT] = {0};

/* Earlier diagnostic helpers (on_screen_gesture, on_screen_pressed,
 * pulse_scale, pulse_glow_opa) were retired once the touch task / glow
 * implementations stabilised. Removed to keep -Werror=unused-function
 * happy. */
static lv_obj_t *s_pet_body    = NULL;   /* lv_image_create — the body sprite */
static lv_obj_t *s_pet_eye_l   = NULL;
static lv_obj_t *s_pet_eye_r   = NULL;
static lv_obj_t *s_pet_mouth   = NULL;
static lv_obj_t *s_pet_cheek_l = NULL;
static lv_obj_t *s_pet_cheek_r = NULL;
static lv_obj_t *s_pet_acc     = NULL;   /* floating accessory, parented to PET screen so it can rotate freely */
static lv_timer_t *s_blink_timer = NULL;
static const lv_image_dsc_t *s_resting_eye = &doudou_eye_normal;

static lv_obj_t *s_label_title  = NULL;
static lv_obj_t *build_screen_title(lv_obj_t *scr, const char *initial_text);
static uint32_t  state_glow_color(doudou_pet_state_t s);

/* Auto-sleep machinery — see doudou_pet_set_state. */
static int64_t   s_last_activity_us = 0;
static lv_timer_t *s_sleep_timer    = NULL;
static void      sleep_check_cb(lv_timer_t *t);
static void      mark_activity(void);
static lv_obj_t *s_label_status = NULL;
static doudou_screen_id_t s_current = DOUDOU_SCREEN_PET;
static doudou_pet_state_t s_state = DOUDOU_PET_IDLE;

/* INFO screen widgets */
static lv_obj_t *s_info_title = NULL;
static lv_obj_t *s_info_account = NULL;
static lv_obj_t *s_info_model = NULL;
static lv_obj_t *s_info_perms = NULL;
static lv_obj_t *s_info_cwd = NULL;
static lv_obj_t *s_info_source = NULL;

/* USAGE screen widgets */
static lv_obj_t *s_usage_container = NULL;  /* parent — children rebuilt on each update */

/* HISTORY screen widgets */
static lv_obj_t *s_history_list = NULL;
static doudou_thread_click_cb_t s_thread_click_cb = NULL;
/* Strdup'd thread ids so the LVGL event has a stable string. */
#define MAX_THREAD_ROWS 8
static char s_thread_ids[MAX_THREAD_ROWS][48];

/* ------- per-state sprite composition (mirrors simulator pet.ts) ------ */
typedef enum { ACC_ANIM_NONE = 0, ACC_ANIM_SPIN, ACC_ANIM_FLOAT } acc_anim_t;

typedef struct {
    const lv_image_dsc_t *eye;     /* always set (never NULL) */
    const lv_image_dsc_t *mouth;   /* NULL = hidden */
    const lv_image_dsc_t *acc;     /* NULL = hidden */
    acc_anim_t acc_anim;
    bool cheek;
    bool shake_on_enter;
} composition_t;

static composition_t compose_for_state(doudou_pet_state_t s)
{
    switch (s) {
        case DOUDOU_PET_IDLE:
            return (composition_t){ &doudou_eye_normal,   &doudou_mouth_smile, NULL,                 ACC_ANIM_NONE,  true,  false };
        case DOUDOU_PET_THINKING:
            return (composition_t){ &doudou_eye_normal,   NULL,                &doudou_acc_question, ACC_ANIM_FLOAT, false, false };
        case DOUDOU_PET_EXECUTING:
            return (composition_t){ &doudou_eye_normal,   &doudou_mouth_open,  &doudou_acc_gear,     ACC_ANIM_SPIN,  false, false };
        case DOUDOU_PET_WAITING:
            return (composition_t){ &doudou_eye_surprise, &doudou_mouth_open,  NULL,                 ACC_ANIM_NONE,  false, false };
        case DOUDOU_PET_DONE:
            return (composition_t){ &doudou_eye_happy,    &doudou_mouth_grin,  &doudou_acc_sparkle,  ACC_ANIM_FLOAT, true,  false };
        case DOUDOU_PET_ERROR:
            return (composition_t){ &doudou_eye_x,        &doudou_mouth_sad,   &doudou_acc_alert,    ACC_ANIM_FLOAT, false, true  };
        case DOUDOU_PET_SLEEPING:
            return (composition_t){ &doudou_eye_sleep,    NULL,                &doudou_acc_zzz,      ACC_ANIM_FLOAT, true,  false };
    }
    return (composition_t){ &doudou_eye_normal, &doudou_mouth_smile, NULL, ACC_ANIM_NONE, true, false };
}

static const char *label_for_state(doudou_pet_state_t s)
{
    switch (s) {
        case DOUDOU_PET_IDLE:      return "idle";
        case DOUDOU_PET_THINKING:  return "thinking";
        case DOUDOU_PET_EXECUTING: return "running";
        case DOUDOU_PET_WAITING:   return "waiting";
        case DOUDOU_PET_DONE:      return "done";
        case DOUDOU_PET_ERROR:     return "error";
        case DOUDOU_PET_SLEEPING:  return "睡眠中";
    }
    return "?";
}

/* ------- animations ------- */
static void breath_anim_cb(void *var, int32_t v)
{
    /* v is 256..272 → mild scale tween for "inhale" (1.0×→1.06×). */
    lv_obj_set_style_transform_scale((lv_obj_t *)var, v, LV_PART_MAIN);
}

/* lv_timer-based breath + blink + edge-glow driver.
 *
 * Why a single periodic timer instead of three lv_anim infinite-repeat
 * tweens? On this firmware build, multiple infinite tweens combined
 * with sprite-alpha redraws keep lv_timer_handler from ever returning
 * (LVGL accumulates invalidations faster than they drain at 160 MHz +
 * software RGB565+A8 blending). One timer that updates everything at
 * 80 ms keeps the redraw rate comfortably low (~12.5 Hz). */
static lv_obj_t *s_edge_glow = NULL;            /* arc-style ring on layer_top */
static lv_timer_t *s_pulse_timer = NULL;

/* Ease-in-out on a 0..255 phase → 60..220 opacity range. Replicates the
 * curve of lv_anim_path_ease_in_out but cheaper because we drive it
 * from one slow lv_timer instead of LVGL's 50 Hz anim cycle. */
static uint8_t glow_opa_for_phase(uint8_t phase)
{
    /* Triangle wave folded into 0..128 then eased. */
    int p = phase;
    if (p > 128) p = 256 - p;                 /* fold 128..255 down to 128..0 */
    /* Smoothstep on 0..128 → 0..1; multiply by range 160. */
    int s = (p * 1000) / 128;                  /* 0..1000 */
    int eased = (s * s * (3000 - 2 * s)) / 1000000;  /* smoothstep 0..1000 */
    return (uint8_t)(60 + (eased * 160) / 1000);
}

static void pulse_timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint8_t phase = 0;
    phase = (uint8_t)(phase + 12);             /* 12 → ~256/12 = 22 steps per cycle */
    if (s_edge_glow) {
        lv_obj_set_style_border_opa(s_edge_glow, glow_opa_for_phase(phase), LV_PART_MAIN);
    }
}

static void start_pet_pulse(void)
{
    if (s_pulse_timer) return;
    /* 120 ms × 22 steps ≈ 2.6 s round-trip — matches the simulator's
     * `glow-breath` 2600 ms ease-in-out keyframe. Cheaper than lv_anim
     * (which fires at 50 Hz). The edge-glow widget now lives on each
     * screen separately so the LVGL redraw only touches the visible
     * one, not every off-screen sibling. */
    s_pulse_timer = lv_timer_create(pulse_timer_cb, 250, NULL);
}

static void start_breath_anim(lv_obj_t *target)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_values(&a, 256, 272);
    lv_anim_set_time(&a, 1800);
    lv_anim_set_playback_time(&a, 1800);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, breath_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void wiggle_anim_cb(void *var, int32_t v)
{
    /* v is -120..120 → translate_x in pixels. We used transform_rotation
     * here originally, which for a sprite triggers bilinear-interpolated
     * full re-rendering each frame; combined with the layered face
     * sprites it queued enough invalidations to lock up
     * lv_timer_handler on the C3. Plain X translate is essentially free
     * (LVGL just shifts where it composites without resampling). */
    int8_t px = (int8_t)(v / 20);   // -120..120 → -6..+6 px
    lv_obj_set_style_translate_x((lv_obj_t *)var, px, LV_PART_MAIN);
}

/* Accessory spin: 0..3600 over 3s, infinite. */
static void acc_spin_cb(void *var, int32_t v)
{
    lv_image_set_rotation((lv_obj_t *)var, v);
}

/* Accessory float: 0..-8 over 1.7s, ping-pong. */
static void acc_float_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, LV_PART_MAIN);
}

static void stop_acc_anim(lv_obj_t *target)
{
    lv_anim_delete(target, acc_spin_cb);
    lv_anim_delete(target, acc_float_cb);
    lv_image_set_rotation(target, 0);
    lv_obj_set_style_translate_y(target, 0, LV_PART_MAIN);
}

static void start_acc_spin(lv_obj_t *target)
{
    stop_acc_anim(target);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_time(&a, 3000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, acc_spin_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

static void start_acc_float(lv_obj_t *target)
{
    stop_acc_anim(target);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_values(&a, 0, -8);
    lv_anim_set_time(&a, 850);
    lv_anim_set_playback_time(&a, 850);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, acc_float_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* Side-shake on error entry: ±10px over 600ms, then back to 0. */
static void shake_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, LV_PART_MAIN);
}

static void start_shake(lv_obj_t *target)
{
    lv_anim_delete(target, shake_cb);
    lv_obj_set_style_translate_x(target, 0, LV_PART_MAIN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_values(&a, -8, 8);
    lv_anim_set_time(&a, 100);
    lv_anim_set_playback_time(&a, 100);
    lv_anim_set_repeat_count(&a, 4);
    lv_anim_set_exec_cb(&a, shake_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* Blink: replace eye sprite with eye_blink for 130ms, then back. */
static void blink_restore_cb(lv_timer_t *t)
{
    if (s_pet_eye_l) lv_image_set_src(s_pet_eye_l, s_resting_eye);
    if (s_pet_eye_r) lv_image_set_src(s_pet_eye_r, s_resting_eye);
    lv_timer_delete(t);
}

static void blink_now(void)
{
    if (s_resting_eye != &doudou_eye_normal && s_resting_eye != &doudou_eye_surprise) return;
    if (!s_pet_eye_l || !s_pet_eye_r) return;
    lv_image_set_src(s_pet_eye_l, &doudou_eye_blink);
    lv_image_set_src(s_pet_eye_r, &doudou_eye_blink);
    lv_timer_t *t = lv_timer_create(blink_restore_cb, 130, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void blink_scheduler_cb(lv_timer_t *t)
{
    blink_now();
    /* random next-blink in 3..7s */
    uint32_t next = 3000 + (esp_random() % 4000);
    lv_timer_set_period(t, next);
}

static void start_blink_scheduler(void)
{
    if (s_blink_timer) return;
    s_blink_timer = lv_timer_create(blink_scheduler_cb, 4000, NULL);
}

static void wiggle_completed_cb(lv_anim_t *a)
{
    /* Belt-and-suspenders: make absolutely sure translate_x is back at
     * 0 when the animation ends, so the body doesn't end up parked off-
     * centre. (The play-back loop should return to 0 on its own, but
     * if the user rapidly taps and the second start preempts the first,
     * the final state can land somewhere mid-swing.) */
    lv_obj_t *obj = (lv_obj_t *)lv_anim_get_user_data(a);
    if (obj) lv_obj_set_style_translate_x(obj, 0, LV_PART_MAIN);
}

/* ---- wake-from-sleep transition ---- */

static lv_timer_t *s_wake_to_idle_timer = NULL;

static void wake_to_idle_cb(lv_timer_t *t)
{
    /* Only settle to IDLE if no real Codex state arrived in the meantime
     * — a thinking/executing push during the 900-ms window should win. */
    if (s_state == DOUDOU_PET_WAITING) {
        doudou_pet_set_state(DOUDOU_PET_IDLE);
    }
    lv_timer_delete(t);
    s_wake_to_idle_timer = NULL;
}

/* Re-arm the "WAITING → IDLE after 900 ms" one-shot. Used by both the
 * sleep-wake path and toy-mode triple-tap. Caller holds the LVGL lock. */
static void arm_surprise_settle(void)
{
    if (s_wake_to_idle_timer) {
        lv_timer_delete(s_wake_to_idle_timer);
        s_wake_to_idle_timer = NULL;
    }
    s_wake_to_idle_timer = lv_timer_create(wake_to_idle_cb, 900, NULL);
    if (s_wake_to_idle_timer) {
        lv_timer_set_repeat_count(s_wake_to_idle_timer, 1);
    }
}

bool doudou_pet_wake_from_sleep(void)
{
    /* Snapshot state under lock so we don't race the sleep timer. */
    if (!doudou_lvgl_lock(50)) return false;
    bool was_sleeping = (s_state == DOUDOU_PET_SLEEPING);
    doudou_lvgl_unlock();
    if (!was_sleeping) return false;

    /* Surprise face first — eye_surprise + mouth_open are the WAITING
     * composition, which doubles as a "huh?" wake reaction. */
    doudou_pet_set_state(DOUDOU_PET_WAITING);

    /* Schedule the settle-to-IDLE. lv_timer_create needs the LVGL lock. */
    if (doudou_lvgl_lock(50)) {
        arm_surprise_settle();
        doudou_lvgl_unlock();
    }
    return true;
}

/* ---- toy mode (offline pet) ---- */

static bool s_toy_mode = false;

void doudou_pet_toy_mode_set(bool enable)
{
    if (enable == s_toy_mode) return;
    s_toy_mode = enable;
    if (enable) {
        ESP_LOGI(TAG, "toy mode ON");
        /* set_state recomputes label visibility from (sleeping || toy)
         * — flip to IDLE so the kid sees a friendly face. We don't push
         * a title/status because toy mode hides both labels anyway. */
        doudou_pet_set_state(DOUDOU_PET_IDLE);
    } else {
        ESP_LOGI(TAG, "toy mode OFF");
        /* No state push here — caller (on_ws_connected) already sets
         * IDLE + clears title + sets status="connected", which will
         * unhide the labels via the same set_state path. */
    }
}

bool doudou_pet_toy_mode_active(void)
{
    return s_toy_mode;
}

void doudou_pet_toy_tap(void)
{
    if (!s_toy_mode) return;

    /* Cycle to a different random state. Excludes SLEEPING (that's the
     * inactivity-timeout screensaver) and ERROR (reserved for the
     * long-press "tantrum" — it'd feel weird if a kid double-tapped
     * once and randomly hit the angry face). */
    static const doudou_pet_state_t pool[] = {
        DOUDOU_PET_IDLE,
        DOUDOU_PET_THINKING,
        DOUDOU_PET_EXECUTING,
        DOUDOU_PET_WAITING,
        DOUDOU_PET_DONE,
    };
    const int N = (int)(sizeof(pool) / sizeof(pool[0]));
    doudou_pet_state_t next = pool[0];
    for (int tries = 0; tries < 6; tries++) {
        next = pool[esp_random() % N];
        if (next != s_state) break;
    }
    ESP_LOGI(TAG, "toy: double-tap → state %d", (int)next);
    doudou_pet_set_state(next);
}

void doudou_pet_toy_long_press(void)
{
    if (!s_toy_mode) return;
    ESP_LOGI(TAG, "toy: long-press → ERROR");
    /* ERROR's apply_composition starts a side-shake on entry, so the
     * kid sees the pet visibly throw a fit. Sits in ERROR until the
     * next tap. */
    doudou_pet_set_state(DOUDOU_PET_ERROR);
}

void doudou_pet_wiggle(void)
{
    if (!doudou_lvgl_lock(50)) return;
    /* Start + end at 0 (offset right to +120 then back). Two repeats
     * give a brief left-right shimmy before parking at centre again. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pet_body);
    lv_anim_set_user_data(&a, s_pet_body);
    lv_anim_set_values(&a, 0, 120);
    lv_anim_set_time(&a, 150);
    lv_anim_set_playback_time(&a, 150);
    lv_anim_set_repeat_count(&a, 2);
    lv_anim_set_exec_cb(&a, wiggle_anim_cb);
    lv_anim_set_completed_cb(&a, wiggle_completed_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
    doudou_lvgl_unlock();
}

/* Bubble — small label that pops above the pet for ~2 s. Mirrors the
 * simulator's `showBubble` on long-press. */
static lv_obj_t *s_pet_bubble = NULL;

static void bubble_remove_cb(lv_timer_t *t)
{
    if (s_pet_bubble) { lv_obj_delete(s_pet_bubble); s_pet_bubble = NULL; }
    lv_timer_delete(t);
}

void doudou_pet_show_bubble(const char *text)
{
    if (!text || !*text) return;
    if (!doudou_lvgl_lock(50)) return;

    if (s_pet_bubble) { lv_obj_delete(s_pet_bubble); s_pet_bubble = NULL; }

    s_pet_bubble = lv_obj_create(s_screens[DOUDOU_SCREEN_PET]);
    lv_obj_set_size(s_pet_bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_pet_bubble, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(s_pet_bubble, lv_color_hex(0x1d2128), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pet_bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pet_bubble, lv_color_hex(0x2f343d), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pet_bubble, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_pet_bubble, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_pet_bubble, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_pet_bubble, 4, LV_PART_MAIN);
    lv_obj_clear_flag(s_pet_bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(s_pet_bubble);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0xe6e9ee), LV_PART_MAIN);

    lv_timer_t *t = lv_timer_create(bubble_remove_cb, 2100, NULL);
    lv_timer_set_repeat_count(t, 1);

    doudou_lvgl_unlock();
}

/* ------- screen construction ------- */
static lv_obj_t *new_screen(uint32_t bg)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, SAFE_INSET, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static lv_obj_t *make_centered_label(lv_obj_t *parent, const char *text,
                                     const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(l, lv_pct(85));
    return l;
}

/* Apply a composition to the live sprites — swap eye/mouth/cheek/acc and
 * (re)start the accessory animation. Caller holds the LVGL lock. */
static void apply_composition(doudou_pet_state_t state)
{
    composition_t c = compose_for_state(state);

    s_resting_eye = c.eye;
    if (s_pet_eye_l) lv_image_set_src(s_pet_eye_l, c.eye);
    if (s_pet_eye_r) lv_image_set_src(s_pet_eye_r, c.eye);

    if (s_pet_mouth) {
        if (c.mouth) {
            lv_image_set_src(s_pet_mouth, c.mouth);
            lv_obj_remove_flag(s_pet_mouth, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pet_mouth, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_pet_cheek_l && s_pet_cheek_r) {
        if (c.cheek) {
            lv_obj_remove_flag(s_pet_cheek_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_pet_cheek_r, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pet_cheek_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_pet_cheek_r, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_pet_acc) {
        if (c.acc) {
            lv_image_set_src(s_pet_acc, c.acc);
            lv_obj_remove_flag(s_pet_acc, LV_OBJ_FLAG_HIDDEN);
            switch (c.acc_anim) {
                case ACC_ANIM_SPIN:  start_acc_spin (s_pet_acc); break;
                case ACC_ANIM_FLOAT: start_acc_float(s_pet_acc); break;
                default:             stop_acc_anim  (s_pet_acc); break;
            }
        } else {
            stop_acc_anim(s_pet_acc);
            lv_obj_add_flag(s_pet_acc, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (c.shake_on_enter && s_pet_body) start_shake(s_pet_body);
}

static void build_pet_screen(lv_obj_t *scr)
{
    /* Title — single-line, marquee-on-overflow, shared with the other
     * three screens via build_screen_title. */
    s_label_title = build_screen_title(scr, "-");

    /* Body — large sprite, breathes via transform_scale pivoted at bottom. */
    s_pet_body = lv_image_create(scr);
    lv_image_set_src(s_pet_body, &doudou_body_glossy);
    lv_obj_center(s_pet_body);
    /* Hard-reset any leftover transform that might survive across
     * re-init (translate / rotate stay applied to the obj otherwise). */
    lv_obj_set_style_translate_x(s_pet_body, 0, LV_PART_MAIN);
    lv_obj_set_style_translate_y(s_pet_body, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(s_pet_body, 0, LV_PART_MAIN);
    /* Pivot at horizontal center, bottom — so breath/squash anchors the
     * body's "feet" and the dome stretches up. */
    lv_obj_set_style_transform_pivot_x(s_pet_body, BODY_W / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(s_pet_body, BODY_H, LV_PART_MAIN);
    lv_obj_clear_flag(s_pet_body, LV_OBJ_FLAG_SCROLLABLE);

    /* Cheeks — children of body, so they follow breath. */
    /* Cheeks and eyes used to be horizontally mirrored via
     * transform_scale_x = -256, but LVGL v9 treats negative scale as
     * "invisible" — the right-side sprites simply disappeared on real
     * hardware. The cheek + eye PNGs are symmetric, so we just draw
     * them upright at both positions and skip the mirror. */
    s_pet_cheek_l = lv_image_create(s_pet_body);
    lv_image_set_src(s_pet_cheek_l, &doudou_cheek);
    /* Face positions on the 200×200 body. The simulator's CSS uses
     * percentages tuned for a 48-px-wide eye (22% × 220). Our eye
     * sprite is only 32 px, so we recompute from the desired centre
     * positions (sim's 37%/63% → 74/126 on a 200-wide body) instead
     * of copying the CSS `left` values verbatim — otherwise the right
     * eye ends up biased toward the centre.
     *   eye-l   centre 74  → x = 74 - 16 = 58
     *   eye-r   centre 126 → x = 126 - 16 = 110
     *   cheek-l centre 44  → x = 32
     *   cheek-r centre 156 → x = 144
     *   mouth   centred horizontally, top 120 */
    lv_obj_set_pos(s_pet_cheek_l, 32, 112);

    s_pet_cheek_r = lv_image_create(s_pet_body);
    lv_image_set_src(s_pet_cheek_r, &doudou_cheek);
    lv_obj_set_pos(s_pet_cheek_r, 144, 112);

    /* Eyes — children of body. eye-r is X-flipped for symmetry. */
    s_pet_eye_l = lv_image_create(s_pet_body);
    lv_image_set_src(s_pet_eye_l, &doudou_eye_normal);
    /* Eye sprite is now 48×48 to match the simulator's 22% × 220 = 48
     * effective size. Centres come from sim CSS percentages
     * (26%/52% × 200): eye-l at x=52, eye-r at x=104. */
    /* Body is 200×200, centered. Eye is 48px. With gap=8 px between
     * the two eyes, each eye left-edge is offset 4 px from body center:
     *   eye_l.x = 100 - 4 - 48 = 48
     *   eye_r.x = 100 + 4      = 104
     * Earlier value 52 was asymmetric vs the right eye's 104. */
    lv_obj_set_pos(s_pet_eye_l, 48, 84);

    s_pet_eye_r = lv_image_create(s_pet_body);
    lv_image_set_src(s_pet_eye_r, &doudou_eye_normal);
    lv_obj_set_pos(s_pet_eye_r, 104, 84);

    /* Mouth — child of body, horizontally centered. */
    s_pet_mouth = lv_image_create(s_pet_body);
    lv_image_set_src(s_pet_mouth, &doudou_mouth_smile);
    lv_obj_set_pos(s_pet_mouth, (BODY_W - 48) / 2, 120);

    /* Floating accessory — child of the SCREEN, not the body. It needs
     * to spin/float independently of breath. Sits near body's top-right. */
    s_pet_acc = lv_image_create(scr);
    lv_image_set_src(s_pet_acc, &doudou_acc_question);
    /* Position above the body's right shoulder — screen coords. */
    lv_obj_align(s_pet_acc, LV_ALIGN_CENTER, 70, -60);
    lv_image_set_pivot(s_pet_acc, 24, 24);    /* center pivot for spin */
    lv_obj_add_flag(s_pet_acc, LV_OBJ_FLAG_HIDDEN);

    /* Status text below the pet */
    s_label_status = make_centered_label(scr, label_for_state(DOUDOU_PET_IDLE),
                                         &lv_font_cjk_14, 0x8a909c);
    lv_obj_align(s_label_status, LV_ALIGN_BOTTOM_MID, 0, -22);

    apply_composition(DOUDOU_PET_IDLE);
    /* Body stays at 1.0× — even a *static* transform_scale > 256
     * triggers bilinear resampling on every redraw, and when the
     * edge-glow ring pulses opacity each frame the whole pet area
     * needs recompositing, which queued enough work to lock up the
     * lv_timer_handler. The cheaper fix is to pre-render the body at
     * its target size in build_pet_art.py; until then we live with
     * the native 180×132 proportions. */
    lv_obj_set_style_transform_scale(s_pet_body, 256, LV_PART_MAIN);
    start_blink_scheduler();
    start_pet_pulse();
}

/* Configure an existing label so it marquees when text doesn't fit. */
static void enable_marquee(lv_obj_t *label, int width)
{
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(label, 8000, LV_PART_MAIN);
}

static lv_obj_t *make_compact_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(l, lv_pct(95));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

static void build_info_screen(lv_obj_t *scr)
{
    /* No title on the INFO screen — the body labels are already
     * self-explanatory (email/plan, model, permissions, cwd, source/sid),
     * a top title would just steal vertical space without adding info. */
    s_info_title = NULL;

    /* Content block centered on the full screen (no title above it). */
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(92), 170);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_layout(body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 4, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    s_info_account = make_compact_label(body, &lv_font_cjk_14, 0xe6e9ee);
    s_info_model   = make_compact_label(body, &lv_font_cjk_14, 0x8a909c);
    s_info_perms   = make_compact_label(body, &lv_font_cjk_14, 0x8a909c);
    s_info_cwd     = make_compact_label(body, &lv_font_cjk_14, 0x8a909c);
    enable_marquee(s_info_cwd, 200);
    s_info_source  = make_compact_label(body, &lv_font_cjk_14, 0x444a55);

    lv_label_set_text(s_info_account, "未连接 Bridge");
    lv_label_set_text(s_info_model, "-");
}

static void build_usage_screen(lv_obj_t *scr)
{
    /* Standard top title shared across all screens. */
    build_screen_title(scr, "用量");

    s_usage_container = lv_obj_create(scr);
    lv_obj_remove_style_all(s_usage_container);
    lv_obj_set_size(s_usage_container, lv_pct(92), 150);
    lv_obj_align(s_usage_container, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_layout(s_usage_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_usage_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_usage_container, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_usage_container, 2, LV_PART_MAIN);
    lv_obj_clear_flag(s_usage_container, LV_OBJ_FLAG_SCROLLABLE);

    /* placeholder until first usage event */
    lv_obj_t *l = make_compact_label(s_usage_container, &lv_font_cjk_14, 0x8a909c);
    lv_label_set_text(l, "暂无用量数据");
}

static void build_history_screen(lv_obj_t *scr)
{
    /* Standard top title shared across all screens. */
    build_screen_title(scr, "会话列表");

    s_history_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_history_list);
    lv_obj_set_size(s_history_list, lv_pct(92), 150);
    lv_obj_align(s_history_list, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_layout(s_history_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_history_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_history_list, 3, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_history_list, LV_DIR_VER);

    lv_obj_t *empty = make_compact_label(s_history_list, &lv_font_cjk_14, 0x8a909c);
    lv_label_set_text(empty, "暂无会话");
}

/* Shared top-of-screen title bar:
 *  - single line, marquee-scrolls when text exceeds the safe width
 *  - centered horizontally, sits 14 px below the screen's top edge
 *  Returns the label so callers can stash it for later set_text. */
static lv_obj_t *build_screen_title(lv_obj_t *scr, const char *initial_text)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    /* Bounded width keeps the label single-line; LONG_SCROLL_CIRCULAR
     * makes it auto-marquee when content overflows. */
    lv_obj_set_width(l, 200);
    lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(l, 8000, LV_PART_MAIN);
    lv_label_set_text(l, initial_text ? initial_text : "-");
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 14);
    return l;
}

esp_err_t doudou_pet_ui_init(void)
{
    if (!doudou_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "lvgl lock failed");
        return ESP_ERR_TIMEOUT;
    }

    s_screens[DOUDOU_SCREEN_INFO]    = new_screen(0x14171c);
    s_screens[DOUDOU_SCREEN_USAGE]   = new_screen(0x14171c);
    s_screens[DOUDOU_SCREEN_PET]     = new_screen(0x0a0c10);
    s_screens[DOUDOU_SCREEN_HISTORY] = new_screen(0x14171c);

    build_info_screen(s_screens[DOUDOU_SCREEN_INFO]);
    build_usage_screen(s_screens[DOUDOU_SCREEN_USAGE]);
    build_pet_screen(s_screens[DOUDOU_SCREEN_PET]);
    build_history_screen(s_screens[DOUDOU_SCREEN_HISTORY]);

    for (int i = 0; i < DOUDOU_SCREEN_COUNT; i++) {
        lv_obj_clear_flag(s_screens[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Edge breathing-glow ring — on the PET screen only (where the
     * user looks most of the time). Putting it on lv_layer_top
     * triggered invalidation of every screen on each opa step and
     * hung lv_timer_handler. Confining it to the PET screen means
     * only the active screen gets redrawn when the opacity changes;
     * the other three sit idle. */
    s_edge_glow = lv_obj_create(s_screens[DOUDOU_SCREEN_PET]);
    lv_obj_set_size(s_edge_glow, 240, 240);
    lv_obj_set_pos(s_edge_glow, -SAFE_INSET, -SAFE_INSET);
    lv_obj_set_style_radius(s_edge_glow, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_edge_glow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_edge_glow, 8, LV_PART_MAIN);
    /* Initial colour matches the boot state (idle = warm sand). It then
     * follows doudou_pet_set_state. */
    lv_obj_set_style_border_color(s_edge_glow,
                                  lv_color_hex(state_glow_color(s_state)),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_edge_glow, 80, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_edge_glow, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_edge_glow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_edge_glow, LV_OBJ_FLAG_SCROLLABLE);
    /* Ride on top of the pet so the glow ring doesn't get hidden by
     * later-created widgets (the question overlay still wins because
     * the overlay covers only the bottom 55%). */
    lv_obj_move_foreground(s_edge_glow);

    s_current = DOUDOU_SCREEN_PET;
    lv_screen_load(s_screens[s_current]);

    /* Auto-sleep watchdog. Checks every 5 s whether AUTO_SLEEP_AFTER_US
     * has passed without any state/title/status/question activity, and
     * if so flips the pet into the SLEEPING composition. */
    mark_activity();
    s_sleep_timer = lv_timer_create(sleep_check_cb, 5000, NULL);

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "UI ready, 4 screens, default=PET");
    return ESP_OK;
}

/* ------- public state mutators (lock internally) ------- */
/* Per-state edge-glow colour. Mirrors STATE_GLOW in simulator/src/ui/pet.ts
 * so device and browser show identical halo tints. RGB565-safe pastels. */
static uint32_t state_glow_color(doudou_pet_state_t s)
{
    switch (s) {
        case DOUDOU_PET_IDLE:      return 0xfce8ad;   /* warm sand */
        case DOUDOU_PET_THINKING:  return 0xa8c8ff;   /* sky blue */
        case DOUDOU_PET_EXECUTING: return 0xffd47a;   /* amber */
        case DOUDOU_PET_WAITING:   return 0xd8b7ff;   /* lilac */
        case DOUDOU_PET_DONE:      return 0x94e9a3;   /* mint green */
        case DOUDOU_PET_ERROR:     return 0xf47878;   /* coral red */
        case DOUDOU_PET_SLEEPING:  return 0x6b7390;   /* muted indigo */
    }
    return 0xfce8ad;
}

/* Auto-sleep: any real activity (set_state, set_title, set_status, an
 * incoming question, …) bumps `s_last_activity_us`. If 60s pass without
 * a bump, the lv_timer below drops the pet into SLEEPING. The next
 * activity restores whatever state was just set. */
#define AUTO_SLEEP_AFTER_US  ((int64_t)60 * 1000 * 1000)

static void sleep_check_cb(lv_timer_t *t)
{
    (void)t;
    if (s_state == DOUDOU_PET_SLEEPING) return;
    int64_t now = esp_timer_get_time();
    int64_t idle_us = now - s_last_activity_us;
    /* Periodic visibility at DEBUG so it's silent in normal builds but
     * easy to flip on via `idf.py menuconfig → Component config → Log →
     * Default log verbosity = Debug` when triaging "why no sleep?". */
    static int s_tick = 0;
    if ((++s_tick % 6) == 0) {
        ESP_LOGD(TAG, "sleep_check idle=%lld s state=%d",
                 (long long)(idle_us / 1000000), (int)s_state);
    }
    if (idle_us >= AUTO_SLEEP_AFTER_US) {
        ESP_LOGI(TAG, "no activity for 60s → SLEEPING");
        doudou_pet_set_state(DOUDOU_PET_SLEEPING);
    }
}

/* External tag passed by each caller so we can see WHO bumped the
 * activity clock. Kept tiny to keep UART quiet but informative. */
#define MARK_ACTIVITY_FROM(reason)  mark_activity_impl(reason)

static void mark_activity_impl(const char *reason)
{
    s_last_activity_us = esp_timer_get_time();
    ESP_LOGI(TAG, "mark_activity(%s) state=%d", reason ? reason : "?", (int)s_state);
}

static void mark_activity(void)
{
    mark_activity_impl("?");
}

/* DONE → IDLE auto-settle. DONE is a "task finished" celebratory pose
 * (green glow, happy eyes); it should celebrate briefly and then drop
 * back to IDLE so the auto-sleep clock can run. Without this, doudou
 * sits forever in DONE until the next Codex event pushes a new state. */
#define DONE_AUTO_IDLE_MS 5000
static lv_timer_t *s_done_to_idle_timer = NULL;

static void done_to_idle_cb(lv_timer_t *t)
{
    /* Mark consumed FIRST so the recursive set_state below doesn't see
     * a live pointer and try to lv_timer_delete it — that would free
     * `t` from under us and trash LVGL's timer list when we delete
     * again at the bottom. Order matters; do not reorder. */
    s_done_to_idle_timer = NULL;
    if (s_state == DOUDOU_PET_DONE) {
        /* Only settle if we're still in DONE — a thinking/executing/
         * error push during the 5-s window should win. */
        doudou_pet_set_state(DOUDOU_PET_IDLE);
    }
    lv_timer_delete(t);
}

/* Latch the "pending state under sleep" so we know what to wake to
 * later when the user actually touches the device. */
static doudou_pet_state_t s_pending_under_sleep = DOUDOU_PET_IDLE;

/* Cache the last-set text on both labels. Two purposes:
 *  1. Suppress no-op mark_activity bumps when bridge replays the same
 *     title/status after a rollout switch.
 *  2. Carry the freshest title/status through a sleep cycle — while
 *     sleeping, set_title/set_status update the cache but not the
 *     hidden widget; on wake set_state re-applies the cache so the
 *     bridge's most-recent push is what the user sees. Empty = unset. */
static char s_cached_title[80]  = {0};
static char s_cached_status[40] = {0};

/* States that wake doudou from sleep. The signal we're trying to
 * filter is bridge "bookkeeping" pushes — every rollout switch /
 * follow_thread / replay-on-reconnect re-emits `state: idle` even
 * though nothing real changed. Real codex activity (thinking /
 * executing / done / waiting / error) IS user-attention-worthy and
 * SHOULD wake.
 *
 * The asymmetry that matters:
 *  - IDLE → SLEEPING → IDLE-from-bridge:  ignore  (bookkeeping)
 *  - IDLE → SLEEPING → THINKING:          wake    (user just asked)
 *  - IDLE → SLEEPING → DONE:              wake    (briefly celebrate)
 *  - IDLE → SLEEPING → WAITING:           wake    (question)
 *  - IDLE → SLEEPING → ERROR:             wake    (something's wrong)
 *
 * Without this, the user starts a new conversation in Codex Desktop,
 * the device stays asleep, and the whole "show me what Codex is
 * doing" promise breaks. */
static bool state_warrants_wake(doudou_pet_state_t s)
{
    /* Anything except IDLE counts as real activity worth waking for. */
    return s != DOUDOU_PET_IDLE
        && s != DOUDOU_PET_SLEEPING;
}

void doudou_pet_set_state(doudou_pet_state_t state)
{
    if (!doudou_lvgl_lock(50)) return;

    /* Sleep is sticky: if we're sleeping and this push isn't a
     * wake-worthy state, just remember it internally and return.
     * Bridge can rapid-fire status=idle / done / thinking all it
     * wants and the screensaver stays put until the user taps OR a
     * question / error arrives. */
    if (s_state == DOUDOU_PET_SLEEPING
        && state != DOUDOU_PET_SLEEPING
        && !state_warrants_wake(state)) {
        s_pending_under_sleep = state;
        ESP_LOGI(TAG, "ignoring set_state(%d) while sleeping (latched as pending)",
                 (int)state);
        doudou_lvgl_unlock();
        return;
    }

    const bool changed = (state != s_state);
    const bool was_sleeping = (s_state == DOUDOU_PET_SLEEPING);
    s_state = state;
    if (changed) apply_composition(state);
    if (s_label_status) {
        lv_label_set_text(s_label_status, label_for_state(state));
    }
    if (s_edge_glow) {
        lv_obj_set_style_border_color(s_edge_glow,
                                      lv_color_hex(state_glow_color(state)),
                                      LV_PART_MAIN);
    }
    /* SLEEPING is a screensaver: hide top thread-title and bottom
     * status text so only the dozing pet + zzz accessory show.
     *
     * We only mess with the LABEL TEXT on the wake-from-sleep edge,
     * not on every state transition. Reason: set_session_info writes
     * the thread title directly to the widget WITHOUT going through
     * doudou_pet_set_title(), so the cache `s_cached_title` is stale
     * (often stuck at "-" from an earlier WS reconnect). Re-applying
     * the cache on every set_state would clobber a perfectly good
     * thread title with "-" when transitioning between
     * THINKING/EXECUTING/DONE/IDLE. */
    const bool sleeping = (state == DOUDOU_PET_SLEEPING);
    const bool waking = was_sleeping && !sleeping;
    /* Hide labels when the screen should look "label-less":
     *  - SLEEPING: screensaver mode.
     *  - toy mode: offline pet; the kid plays with the face, not a
     *    Codex thread title that doesn't exist right now. */
    const bool hide_labels = sleeping || s_toy_mode;
    if (s_label_title) {
        if (hide_labels) {
            lv_obj_add_flag(s_label_title, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_label_title, LV_OBJ_FLAG_HIDDEN);
            /* Only on the wake-from-sleep edge: while sleeping,
             * set_title silently updated the cache but not the
             * (hidden) widget. Sync it now so the user sees the
             * freshest title. Cache and widget agree afterwards
             * because set_session_info now also routes through
             * set_title. */
            if (waking && s_cached_title[0]) {
                lv_label_set_text(s_label_title, s_cached_title);
            }
        }
    }
    if (s_label_status) {
        if (hide_labels) {
            lv_obj_add_flag(s_label_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_label_status, LV_OBJ_FLAG_HIDDEN);
            if (waking && s_cached_status[0]) {
                lv_label_set_text(s_label_status, s_cached_status);
            }
        }
    }
    if (changed && state != DOUDOU_PET_SLEEPING) MARK_ACTIVITY_FROM("set_state");
    /* Reset the pending-under-sleep latch — we're no longer sleeping. */
    if (state != DOUDOU_PET_SLEEPING) s_pending_under_sleep = DOUDOU_PET_IDLE;

    /* Manage the DONE auto-settle timer:
     *  - leaving DONE for anything else → cancel pending timer
     *  - entering DONE → (re)arm a fresh 5-s countdown */
    if (s_done_to_idle_timer && state != DOUDOU_PET_DONE) {
        lv_timer_delete(s_done_to_idle_timer);
        s_done_to_idle_timer = NULL;
    }
    if (state == DOUDOU_PET_DONE) {
        if (s_done_to_idle_timer) {
            lv_timer_delete(s_done_to_idle_timer);
            s_done_to_idle_timer = NULL;
        }
        s_done_to_idle_timer = lv_timer_create(done_to_idle_cb,
                                               DONE_AUTO_IDLE_MS, NULL);
        if (s_done_to_idle_timer) {
            lv_timer_set_repeat_count(s_done_to_idle_timer, 1);
        }
    }
    doudou_lvgl_unlock();
}

void doudou_pet_set_title(const char *title)
{
    if (!doudou_lvgl_lock(50)) return;
    const char *eff = title && *title ? title : "-";
    bool changed = strncmp(eff, s_cached_title, sizeof(s_cached_title)) != 0;
    /* While sleeping, latch the cached text but don't update LVGL
     * widget (hidden anyway) and DON'T mark activity. Sleep stays
     * sticky until user touches OR a real wake event arrives. */
    if (s_state == DOUDOU_PET_SLEEPING) {
        if (changed) {
            strncpy(s_cached_title, eff, sizeof(s_cached_title) - 1);
            s_cached_title[sizeof(s_cached_title) - 1] = '\0';
        }
        doudou_lvgl_unlock();
        return;
    }
    if (s_label_title) lv_label_set_text(s_label_title, eff);
    if (changed) {
        strncpy(s_cached_title, eff, sizeof(s_cached_title) - 1);
        s_cached_title[sizeof(s_cached_title) - 1] = '\0';
        MARK_ACTIVITY_FROM("set_title");
    }
    doudou_lvgl_unlock();
}

void doudou_pet_set_status(const char *status)
{
    if (!doudou_lvgl_lock(50)) return;
    const char *eff = status && *status ? status : label_for_state(s_state);
    bool changed = strncmp(eff, s_cached_status, sizeof(s_cached_status)) != 0;
    if (s_state == DOUDOU_PET_SLEEPING) {
        if (changed) {
            strncpy(s_cached_status, eff, sizeof(s_cached_status) - 1);
            s_cached_status[sizeof(s_cached_status) - 1] = '\0';
        }
        doudou_lvgl_unlock();
        return;
    }
    if (s_label_status) lv_label_set_text(s_label_status, eff);
    if (changed) {
        strncpy(s_cached_status, eff, sizeof(s_cached_status) - 1);
        s_cached_status[sizeof(s_cached_status) - 1] = '\0';
        MARK_ACTIVITY_FROM("set_status");
    }
    doudou_lvgl_unlock();
}

/* Deferred screen switch: external callers (touch task, button handlers,
 * BLE/WS commands) write `s_pending_screen` and the LVGL task picks it
 * up at the top of its next iteration. Avoids cross-task lock starvation
 * when LVGL is rendering heavy sprite animations and a touch event
 * comes in at exactly the wrong moment. */
volatile int s_pending_screen = -1;

void doudou_pet_apply_pending_screen(void)
{
    int target = s_pending_screen;
    if (target < 0) return;
    s_pending_screen = -1;
    if (target == s_current || target >= DOUDOU_SCREEN_COUNT) return;
    ESP_LOGI(TAG, "screen_apply %d → %d (no-anim swap)", (int)s_current, target);
    s_current = (doudou_screen_id_t)target;
    /* lv_screen_load_anim caused the LVGL task to hang on subsequent
     * iterations — animations on the off-screen screens probably never
     * finished. Use the plain non-animated load for now; we can
     * reintroduce the slide animation once the lockup is understood. */
    lv_screen_load(s_screens[target]);
}

void doudou_screen_show(doudou_screen_id_t which)
{
    if ((int)which < 0 || which >= DOUDOU_SCREEN_COUNT) return;
    if (which == s_current) return;
    s_pending_screen = (int)which;
    ESP_LOGI(TAG, "screen_show queued → %d", (int)which);
}

void doudou_screen_shift(int direction)
{
    int next = (int)s_current + (direction < 0 ? -1 : 1);
    if (next < 0) next = 0;
    if (next >= DOUDOU_SCREEN_COUNT) next = DOUDOU_SCREEN_COUNT - 1;
    ESP_LOGI(TAG, "screen_shift dir=%d, %d → %d",
             direction, (int)s_current, next);
    doudou_screen_show((doudou_screen_id_t)next);
}

doudou_screen_id_t doudou_screen_current(void)
{
    return s_current;
}

void doudou_pet_set_state_str(const char *s)
{
    if (!s) return;
    doudou_pet_state_t st = s_state;
    if      (!strcmp(s, "idle"))             st = DOUDOU_PET_IDLE;
    else if (!strcmp(s, "thinking"))         st = DOUDOU_PET_THINKING;
    else if (!strcmp(s, "executing"))        st = DOUDOU_PET_EXECUTING;
    else if (!strcmp(s, "waiting_input") ||
             !strcmp(s, "waiting_approval")) st = DOUDOU_PET_WAITING;
    else if (!strcmp(s, "done"))             st = DOUDOU_PET_DONE;
    else if (!strcmp(s, "error"))            st = DOUDOU_PET_ERROR;
    doudou_pet_set_state(st);
}

/* ---------- INFO screen renderer ---------- */

static void set_label_text_safe(lv_obj_t *l, const char *txt)
{
    if (l) lv_label_set_text(l, (txt && *txt) ? txt : "-");
}

void doudou_pet_set_session_info(const doudou_ui_session_info_t *info)
{
    if (!info) return;
    if (!doudou_lvgl_lock(100)) return;

    /* Route thread_title through the cached set_title path so the
     * cache stays in sync with the widget. Earlier, this wrote
     * s_label_title directly, leaving s_cached_title stuck at
     * whatever set_title last received (often "-" from a WS
     * reconnect) — and the cache-restore-on-wake clobbered the real
     * title. Going through set_title is the single source of truth. */
    if (info->thread_title) {
        doudou_lvgl_unlock();
        doudou_pet_set_title(info->thread_title);
        if (!doudou_lvgl_lock(100)) return;
    }

    if (s_info_title) {
        set_label_text_safe(s_info_title, info->thread_title ? info->thread_title : "Codex");
    }
    if (s_info_account) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s%s%s",
                 info->account_email ? info->account_email : "-",
                 info->plan_type ? " / " : "",
                 info->plan_type ? info->plan_type : "");
        lv_label_set_text(s_info_account, buf);
    }
    if (s_info_model) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s%s",
                 info->model ? info->model : "-",
                 info->reasoning_effort ? " / " : "",
                 info->reasoning_effort ? info->reasoning_effort : "");
        lv_label_set_text(s_info_model, buf);
    }
    set_label_text_safe(s_info_perms, info->permissions);
    if (s_info_cwd && info->cwd) {
        /* tail-shorten ~/dir/... when long */
        const char *p = info->cwd;
        const char *tail = strrchr(p, '/');
        lv_label_set_text(s_info_cwd, tail ? tail + 1 : p);
    }
    if (s_info_source) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s%s",
                 info->source ? info->source : "-",
                 info->session_id ? " / " : "",
                 info->session_id ? info->session_id : "");
        /* short session id */
        if (info->session_id && strlen(info->session_id) > 8) buf[strlen(info->source ? info->source : "-") + 3 + 8] = '\0';
        lv_label_set_text(s_info_source, buf);
    }
    doudou_lvgl_unlock();
}

/* ---------- USAGE screen renderer ---------- */

static lv_obj_t *make_usage_row(lv_obj_t *parent, const char *left, const char *right, uint32_t color)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8a909c), LV_PART_MAIN);
    lv_label_set_text(l, left);
    lv_obj_t *r = lv_label_create(row);
    lv_obj_set_style_text_font(r, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(r, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(r, right);
    return row;
}

static void make_remaining_bar(lv_obj_t *parent, int remain_pct)
{
    if (remain_pct < 0) remain_pct = 0;
    if (remain_pct > 100) remain_pct = 100;
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, lv_pct(100), 5);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, remain_pct, LV_ANIM_OFF);
    uint32_t fg = (remain_pct < 10) ? 0xef5350 : (remain_pct < 30) ? 0xe2b53a : 0x3ec27a;
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1f242d), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(fg), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
}

/**
 * Format an epoch-ms reset moment in the user's *local* time zone.
 * Mirrors simulator usage.ts `untilHM()`:
 *   today    → "今天 HH:MM"
 *   tomorrow → "明天 HH:MM"
 *   in 2-6d  → "周X HH:MM"  (X = mon..sun in Chinese)
 *   further  → "M月D日 HH:MM"
 * Returns empty string when target is in the past or 0/unset.
 *
 * Requires the device clock to be sane — bridge stamps `server_time_ms`
 * on welcome which lvgl_port currently doesn't use to sync time. That
 * means on a fresh boot the clock may be UNIX epoch and this function
 * returns an obviously-wrong date. TODO: sync from welcome.
 */
static void format_local_reset(int64_t reset_ms, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (reset_ms <= 0) return;

    time_t now_s = time(NULL);
    time_t tgt_s = (time_t)(reset_ms / 1000);
    if (tgt_s <= now_s) {
        snprintf(out, cap, "即将重置");
        return;
    }

    struct tm now_tm, tgt_tm;
    localtime_r(&now_s, &now_tm);
    localtime_r(&tgt_s, &tgt_tm);

    /* Calendar-day delta (not 24-hour chunks) — mirrors sim. */
    struct tm n_mid = now_tm; n_mid.tm_hour = 0; n_mid.tm_min = 0; n_mid.tm_sec = 0;
    struct tm t_mid = tgt_tm; t_mid.tm_hour = 0; t_mid.tm_min = 0; t_mid.tm_sec = 0;
    int days_ahead = (int)((mktime(&t_mid) - mktime(&n_mid)) / 86400);

    if (days_ahead == 0)
        snprintf(out, cap, "今天 %02d:%02d", tgt_tm.tm_hour, tgt_tm.tm_min);
    else if (days_ahead == 1)
        snprintf(out, cap, "明天 %02d:%02d", tgt_tm.tm_hour, tgt_tm.tm_min);
    else if (days_ahead >= 2 && days_ahead <= 6) {
        static const char *zh_wd[] = {"日","一","二","三","四","五","六"};
        snprintf(out, cap, "周%s %02d:%02d",
                 zh_wd[tgt_tm.tm_wday % 7], tgt_tm.tm_hour, tgt_tm.tm_min);
    } else {
        snprintf(out, cap, "%d月%d日 %02d:%02d",
                 tgt_tm.tm_mon + 1, tgt_tm.tm_mday,
                 tgt_tm.tm_hour, tgt_tm.tm_min);
    }
}

static void format_n(int64_t n, char *out, size_t cap)
{
    if (n < 1000) snprintf(out, cap, "%lld", (long long)n);
    else if (n < 1000000) snprintf(out, cap, "%.1fk", n / 1000.0);
    else snprintf(out, cap, "%.2fM", n / 1000000.0);
}

void doudou_pet_set_usage(const doudou_ui_usage_t *u)
{
    if (!u || !s_usage_container) return;
    if (!doudou_lvgl_lock(100)) return;

    lv_obj_clean(s_usage_container);

    /* Context remaining */
    if (u->has_session && u->model_context_window > 0) {
        int64_t cur = u->current_context_tokens;
        int64_t total = u->model_context_window;
        int64_t remain = total - cur; if (remain < 0) remain = 0;
        int pct = (int)((remain * 100) / total);
        char buf[48], rs[16], ts[16];
        format_n(remain, rs, sizeof(rs));
        format_n(total, ts, sizeof(ts));
        snprintf(buf, sizeof(buf), "%s / %s  %d%%", rs, ts, pct);
        lv_obj_t *h = lv_label_create(s_usage_container);
        lv_obj_set_style_text_font(h, &lv_font_cjk_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
        lv_label_set_text(h, "上下文剩余");
        make_usage_row(s_usage_container, "", buf, 0xe6e9ee);
        make_remaining_bar(s_usage_container, pct);
    }

    /* Cumulative tokens */
    if (u->has_session && u->total_tokens > 0) {
        char buf[24];
        format_n(u->total_tokens, buf, sizeof(buf));
        make_usage_row(s_usage_container, "本会话 tokens", buf, 0xe6e9ee);
    }

    /* Limit groups, in order; group by group_label.
     * Skip the "Spark" variant — matches simulator usage.ts filter. */
    const char *last_group = "__none__";
    for (int i = 0; i < u->n_limits; i++) {
        const doudou_ui_limit_t *l = &u->limits[i];
        if (l->group_label && strcasestr(l->group_label, "spark")) continue;
        const char *grp = l->group_label ? l->group_label : "main";
        if (strcmp(grp, last_group) != 0) {
            char hdr[48];
            snprintf(hdr, sizeof(hdr), "%s / 剩余",
                     l->group_label ? l->group_label : (u->plan_type ? u->plan_type : "main"));
            lv_obj_t *h = lv_label_create(s_usage_container);
            lv_obj_set_style_text_font(h, &lv_font_cjk_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(h, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
            lv_label_set_text(h, hdr);
            last_group = grp;
        }
        int remain = (l->used_pct >= 0) ? (100 - l->used_pct) : 0;
        char when[32] = {0};
        format_local_reset(l->resets_at_ms, when, sizeof(when));
        char right[48];
        if (when[0]) snprintf(right, sizeof(right), "%d%%  %s", remain, when);
        else         snprintf(right, sizeof(right), "%d%%",     remain);
        make_usage_row(s_usage_container, l->label ? l->label : l->id, right, 0xe6e9ee);
        make_remaining_bar(s_usage_container, remain);
    }

    if (lv_obj_get_child_count(s_usage_container) == 0) {
        lv_obj_t *l = make_compact_label(s_usage_container, &lv_font_cjk_14, 0x8a909c);
        lv_label_set_text(l, "暂无用量数据");
    }
    doudou_lvgl_unlock();
}

/* ---------- HISTORY screen renderer ---------- */

void doudou_pet_set_thread_click_cb(doudou_thread_click_cb_t cb)
{
    s_thread_click_cb = cb;
}

static void thread_row_click_cb(lv_event_t *e)
{
    if (!s_thread_click_cb) return;
    const char *tid = (const char *)lv_event_get_user_data(e);
    if (tid && *tid) s_thread_click_cb(tid);
}

void doudou_pet_set_threads(const doudou_ui_thread_t *threads, int n)
{
    if (!s_history_list) return;
    if (!doudou_lvgl_lock(100)) return;

    lv_obj_clean(s_history_list);
    if (n <= 0) {
        lv_obj_t *l = make_compact_label(s_history_list, &lv_font_cjk_14, 0x8a909c);
        lv_label_set_text(l, "暂无会话");
        doudou_lvgl_unlock();
        return;
    }
    int rows = n < MAX_THREAD_ROWS ? n : MAX_THREAD_ROWS;
    for (int i = 0; i < rows; i++) {
        const doudou_ui_thread_t *t = &threads[i];

        lv_obj_t *row = lv_obj_create(s_history_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 2, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(t->active ? 0xef5350 : 0x3ec27a), LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);

        lv_obj_t *title = lv_label_create(row);
        lv_obj_set_style_text_font(title, &lv_font_cjk_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(title,
            lv_color_hex(t->active ? 0xe6e9ee : 0x8a909c), LV_PART_MAIN);
        lv_obj_set_flex_grow(title, 1);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_label_set_text(title, t->title ? t->title : "-");

        /* cache id for click handler */
        if (t->id) {
            strncpy(s_thread_ids[i], t->id, sizeof(s_thread_ids[i]) - 1);
            s_thread_ids[i][sizeof(s_thread_ids[i]) - 1] = '\0';
            lv_obj_add_event_cb(row, thread_row_click_cb, LV_EVENT_CLICKED, s_thread_ids[i]);
        }
    }
    doudou_lvgl_unlock();
}

/* ---------- QUESTION overlay (touchable choices, risk-tinted) ----------
 *
 * Mirrors `packages/simulator/src/ui/question.ts`. Supports:
 *   - 1-4 choice buttons with risk-tinted accept (matches simulator)
 *   - `require_confirm: true` → first tap opens a 2nd-stage confirm page
 *     instead of replying immediately (high-risk safety)
 *   - `queue_total > 1` → a "+N" badge in the upper-right showing how
 *     many other questions are waiting
 *   - Title marquee for long content
 */

static lv_obj_t *s_question_overlay = NULL;
static doudou_question_reply_cb_t s_question_reply_cb = NULL;

/* Snapshot of the active question kept in static memory so the confirm
 * stage can re-render without holding a cJSON tree. */
typedef struct {
    char     id[64];
    char     risk[8];                                 /* "low" / "medium" / "high" */
    char     action_type[24];
    char     title[160];
    char     body[160];
    bool     has_body;
    bool     require_confirm;
    int      queue_total;                             /* 0 = unspecified */
    int      n_choices;
    char     choice_ids[DOUDOU_MAX_CHOICES][32];
    char     choice_labels[DOUDOU_MAX_CHOICES][48];
} q_snapshot_t;

static q_snapshot_t s_q;                              /* current question */
static char         s_q_pending_choice[32];           /* armed in confirm stage */

static const char *action_label_zh(const char *a)
{
    if (!a)                             return "请确认";
    if (!strcmp(a, "run_command"))      return "运行命令";
    if (!strcmp(a, "modify_file"))      return "修改文件";
    if (!strcmp(a, "network_access"))   return "访问网络";
    if (!strcmp(a, "user_input"))       return "需要输入";
    if (!strcmp(a, "tool_call"))        return "工具调用";
    return "其他";
}

static uint32_t risk_color_hex(const char *risk)
{
    if (!risk)                       return 0x8a909c;
    if (!strcmp(risk, "low"))        return 0x3ec27a;
    if (!strcmp(risk, "medium"))     return 0xe2b53a;
    if (!strcmp(risk, "high"))       return 0xef5350;
    return 0x8a909c;
}

void doudou_pet_set_question_cb(doudou_question_reply_cb_t cb)
{
    s_question_reply_cb = cb;
}

static void copy_clip(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

/* Forward decls */
static void render_choose_stage(void);
static void render_confirm_stage(void);

static void clear_overlay_children(void)
{
    if (!s_question_overlay) return;
    lv_obj_clean(s_question_overlay);
}

static void build_overlay_frame(void)
{
    /* Bottom 55% of PET screen so the pet keeps its top half visible. */
    lv_obj_t *parent = s_screens[DOUDOU_SCREEN_PET];
    s_question_overlay = lv_obj_create(parent);
    /* Width is the full physical panel (240), not lv_pct(100) which would
     * resolve to 220 because the parent screen has SAFE_INSET padding.
     * Buttons inside set their width relative to this, so 100% of overlay
     * = full 240 — visible portion goes right to the round mask edge. */
    lv_obj_set_size(s_question_overlay, 240, lv_pct(55));
    lv_obj_align(s_question_overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_question_overlay, lv_color_hex(0x0a0c10), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_question_overlay, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_question_overlay, 0, LV_PART_MAIN);
    /* Top pad = 0 — meta/title/body need every available vertical pixel
     * so they don't get LVGL-clipped (line-height of montserrat_14 is
     * ~18px, three stacked = 54px ≈ overlay top half). */
    lv_obj_set_style_pad_top(s_question_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_question_overlay, SAFE_INSET, LV_PART_MAIN);
    /* Horizontal pad zero so choice buttons (width 100%) reach the
     * screen's left/right edges — bigger touch zone for thumbs.
     * Title/body widgets in this overlay are already explicitly width-
     * capped (lv_pct(90)), so they don't get visually slammed against
     * the round mask. */
    lv_obj_set_style_pad_left(s_question_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_question_overlay, 0, LV_PART_MAIN);
    lv_obj_set_layout(s_question_overlay, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_question_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_question_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_question_overlay, 6, LV_PART_MAIN);
    lv_obj_clear_flag(s_question_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Queue-total badge — absolute child of the overlay, upper-right. */
    if (s_q.queue_total > 1) {
        lv_obj_t *badge = lv_label_create(s_question_overlay);
        char buf[24];
        snprintf(buf, sizeof(buf), "+%d", s_q.queue_total - 1);
        lv_label_set_text(badge, buf);
        lv_obj_set_style_text_font(badge, &lv_font_cjk_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(badge, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x2a2e36), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(badge, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(badge, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(badge, 8, LV_PART_MAIN);
        /* Pull out of the flex flow and pin to upper-right. */
        lv_obj_add_flag(badge, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -6, 4);
    }
}

static void choice_click_cb(lv_event_t *e)
{
    const char *cid = (const char *)lv_event_get_user_data(e);
    if (!cid || !*cid) return;

    /* If this question requires explicit confirmation, arm the pending
     * choice and switch to the confirm stage instead of replying. */
    if (s_q.require_confirm) {
        copy_clip(s_q_pending_choice, sizeof(s_q_pending_choice), cid);
        render_confirm_stage();
        return;
    }

    if (s_question_reply_cb) s_question_reply_cb(s_q.id, cid);
    doudou_pet_hide_question();
}

static void confirm_commit_cb(lv_event_t *e)
{
    (void)e;
    if (s_q_pending_choice[0] == '\0') return;
    if (s_question_reply_cb) s_question_reply_cb(s_q.id, s_q_pending_choice);
    doudou_pet_hide_question();
}

static void confirm_back_cb(lv_event_t *e)
{
    (void)e;
    s_q_pending_choice[0] = '\0';
    render_choose_stage();
}

/* Vertical-stack button height when 3+ choices force a column layout. */
static int stack_btn_height(int n_buttons)
{
    if (n_buttons == 3) return 40;
    return 32;                                /* 4 choices, rare + cramped */
}

/* Where to put the label within the button. For 2-button horizontal
 * rows, both buttons bleed into the invisible round corners. Centred
 * text would visually land near (or over) the cropped edge, so we push
 * left-button text right and right-button text left, both inset 18px
 * from the inner gap. Vertical-stack buttons use CENTER. */
typedef enum {
    Q_TEXT_CENTER,
    Q_TEXT_LEFT,         /* label hugs the left edge with inset — for the right-side btn */
    Q_TEXT_RIGHT,        /* label hugs the right edge with inset — for the left-side btn */
} q_text_align_t;

#define Q_TEXT_INSET 18

/* Build a single choice button. Width comes from the parent layout
 * (flex-grow for the horizontal row, lv_pct(100) for the column). */
static lv_obj_t *make_q_button(lv_obj_t *parent, const char *label,
                               uint32_t bg, uint32_t fg, int radius,
                               int height, q_text_align_t align)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, height);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), LV_PART_MAIN);
    lv_label_set_text(l, label);
    switch (align) {
        case Q_TEXT_LEFT:
            lv_obj_align(l, LV_ALIGN_LEFT_MID,  Q_TEXT_INSET, 0);
            break;
        case Q_TEXT_RIGHT:
            lv_obj_align(l, LV_ALIGN_RIGHT_MID, -Q_TEXT_INSET, 0);
            break;
        case Q_TEXT_CENTER:
        default:
            lv_obj_center(l);
            break;
    }
    return btn;
}

/* Make a flex-row container that stretches the full 240px width of the
 * overlay. Use for the 2-choice case + the confirm stage. Buttons added
 * as children with flex_grow=1 share the row equally with the given gap.
 * The 16px gap leaves enough thumb-clear space between the two targets. */
static lv_obj_t *make_q_button_row(lv_obj_t *parent, int height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), height);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 16, LV_PART_MAIN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* Add a button to a flex-row, equal-share via flex_grow.
 * `is_left` decides text alignment so labels lean inward toward the gap. */
static lv_obj_t *make_q_row_button(lv_obj_t *row, const char *label,
                                   uint32_t bg, uint32_t fg, int height,
                                   bool is_left)
{
    lv_obj_t *btn = make_q_button(row, label, bg, fg, 8, height,
                                  is_left ? Q_TEXT_RIGHT : Q_TEXT_LEFT);
    lv_obj_set_flex_grow(btn, 1);
    return btn;
}

/* Add a button to a vertical column (3+ choice case), full overlay width. */
static lv_obj_t *make_q_col_button(lv_obj_t *parent, const char *label,
                                   uint32_t bg, uint32_t fg, int height)
{
    lv_obj_t *btn = make_q_button(parent, label, bg, fg, 8, height,
                                  Q_TEXT_CENTER);
    lv_obj_set_width(btn, lv_pct(100));
    return btn;
}

static void render_choose_stage(void)
{
    clear_overlay_children();

    /* Risk + action header */
    lv_obj_t *meta = lv_label_create(s_question_overlay);
    char meta_buf[48];
    snprintf(meta_buf, sizeof(meta_buf), "%s / %s",
             s_q.risk[0] ? s_q.risk : "?", action_label_zh(s_q.action_type));
    lv_label_set_text(meta, meta_buf);
    lv_obj_set_style_text_font(meta, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(meta, lv_color_hex(risk_color_hex(s_q.risk)), LV_PART_MAIN);

    /* Title — marquee if it doesn't fit */
    if (s_q.title[0]) {
        lv_obj_t *title = lv_label_create(s_question_overlay);
        lv_obj_set_style_text_font(title, &lv_font_cjk_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_width(title, lv_pct(90));
        lv_label_set_text(title, s_q.title);
        lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(title, 8000, LV_PART_MAIN);
    }

    /* Body */
    if (s_q.has_body) {
        lv_obj_t *body = lv_label_create(s_question_overlay);
        lv_obj_set_style_text_font(body, &lv_font_cjk_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(body, lv_color_hex(0x8a909c), LV_PART_MAIN);
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_width(body, lv_pct(90));
        lv_label_set_text(body, s_q.body);
        lv_label_set_long_mode(body, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(body, 8000, LV_PART_MAIN);
    }

    /* Choice buttons.
     *   n=2 → horizontal row, both buttons share full 240px width.
     *   n=3,4 → vertical column, full width each, smaller height. */
    uint32_t accent = risk_color_hex(s_q.risk);
    bool risk_is_high = (strcmp(s_q.risk, "high") == 0);

    if (s_q.n_choices == 2) {
        lv_obj_t *row = make_q_button_row(s_question_overlay, 48);
        for (int i = 0; i < 2; i++) {
            const char *cid    = s_q.choice_ids[i];
            const char *label  = s_q.choice_labels[i][0] ? s_q.choice_labels[i] : cid;
            bool is_accept = cid[0] && !strcmp(cid, "accept");
            uint32_t bg = is_accept ? accent : 0x2a2e36;
            uint32_t fg = (is_accept && risk_is_high) ? 0xffffff : 0xe6e9ee;
            lv_obj_t *btn = make_q_row_button(row, label, bg, fg, 48,
                                              /*is_left=*/ i == 0);
            lv_obj_add_event_cb(btn, choice_click_cb, LV_EVENT_CLICKED,
                                (void *)s_q.choice_ids[i]);
        }
    } else {
        for (int i = 0; i < s_q.n_choices; i++) {
            const char *cid    = s_q.choice_ids[i];
            const char *label  = s_q.choice_labels[i][0] ? s_q.choice_labels[i] : cid;
            bool is_accept = cid[0] && !strcmp(cid, "accept");
            uint32_t bg = is_accept ? accent : 0x2a2e36;
            uint32_t fg = (is_accept && risk_is_high) ? 0xffffff : 0xe6e9ee;
            lv_obj_t *btn = make_q_col_button(s_question_overlay, label, bg, fg,
                                              stack_btn_height(s_q.n_choices));
            lv_obj_add_event_cb(btn, choice_click_cb, LV_EVENT_CLICKED,
                                (void *)s_q.choice_ids[i]);
        }
    }
}

static void render_confirm_stage(void)
{
    clear_overlay_children();

    /* Find the pending choice's human label for the banner. */
    const char *pending_label = s_q_pending_choice;
    for (int i = 0; i < s_q.n_choices; i++) {
        if (!strcmp(s_q.choice_ids[i], s_q_pending_choice)) {
            pending_label = s_q.choice_labels[i][0]
                              ? s_q.choice_labels[i]
                              : s_q.choice_ids[i];
            break;
        }
    }

    /* Warning banner. */
    lv_obj_t *warn = lv_label_create(s_question_overlay);
    lv_obj_set_style_text_font(warn, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(warn, lv_color_hex(0xef5350), LV_PART_MAIN);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(warn, lv_pct(90));
    char buf[192];
    snprintf(buf, sizeof(buf), "[!] 确认要 \"%s\" 吗?", pending_label);
    lv_label_set_text(warn, buf);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_SCROLL_CIRCULAR);

    /* Confirm stage = 2 buttons,horizontal row layout to match the
     * choose stage's 2-choice look. 返回 left, 确认 right. */
    lv_obj_t *row = make_q_button_row(s_question_overlay, 48);
    lv_obj_t *back   = make_q_row_button(row, "返回", 0x2a2e36, 0xe6e9ee, 48,
                                         /*is_left=*/ true);
    lv_obj_add_event_cb(back, confirm_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *commit = make_q_row_button(row, "确认", 0xef5350, 0xffffff, 48,
                                         /*is_left=*/ false);
    lv_obj_add_event_cb(commit, confirm_commit_cb, LV_EVENT_CLICKED, NULL);
}

void doudou_pet_show_question(const doudou_question_t *q)
{
    if (!q || !q->id) return;
    if (!doudou_lvgl_lock(100)) return;

    /* Replace any existing overlay. */
    if (s_question_overlay) {
        lv_obj_delete(s_question_overlay);
        s_question_overlay = NULL;
    }

    /* Snapshot the question into our static struct. */
    memset(&s_q, 0, sizeof(s_q));
    copy_clip(s_q.id,           sizeof(s_q.id),           q->id);
    copy_clip(s_q.risk,         sizeof(s_q.risk),         q->risk);
    copy_clip(s_q.action_type,  sizeof(s_q.action_type),  q->action_type);
    copy_clip(s_q.title,        sizeof(s_q.title),        q->title);
    if (q->body) {
        s_q.has_body = true;
        copy_clip(s_q.body, sizeof(s_q.body), q->body);
    }
    s_q.require_confirm = q->require_confirm;
    s_q.queue_total     = q->queue_total;
    int n = q->n_choices < DOUDOU_MAX_CHOICES ? q->n_choices : DOUDOU_MAX_CHOICES;
    s_q.n_choices = n;
    for (int i = 0; i < n; i++) {
        copy_clip(s_q.choice_ids[i],    sizeof(s_q.choice_ids[i]),    q->choices[i].id);
        copy_clip(s_q.choice_labels[i], sizeof(s_q.choice_labels[i]), q->choices[i].label);
    }
    s_q_pending_choice[0] = '\0';

    build_overlay_frame();
    render_choose_stage();

    /* Make sure PET screen is the active one. */
    if (s_current != DOUDOU_SCREEN_PET) {
        s_current = DOUDOU_SCREEN_PET;
        lv_screen_load(s_screens[DOUDOU_SCREEN_PET]);
    }

    doudou_lvgl_unlock();
    ESP_LOGI(TAG, "question shown id=%s risk=%s n_choices=%d queue=%d confirm=%d",
             s_q.id, s_q.risk[0] ? s_q.risk : "?", n, s_q.queue_total, s_q.require_confirm);
}

void doudou_pet_hide_question(void)
{
    if (!doudou_lvgl_lock(100)) return;
    if (s_question_overlay) {
        lv_obj_delete(s_question_overlay);
        s_question_overlay = NULL;
    }
    s_q_pending_choice[0] = '\0';
    doudou_lvgl_unlock();
}
