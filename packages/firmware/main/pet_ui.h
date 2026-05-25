/**
 * pet_ui.h — Doudou's 4-screen pet UI for the round 240×240 display.
 *
 * Screen layout matches the browser simulator:
 *   INFO ← USAGE ← PET → HISTORY
 *
 * MVP-1a step 3 populates only PET with a working pet + breath animation;
 * the other three screens render placeholder text until MVP-1b wires up
 * real data over WebSocket.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "protocol_parse.h"      /* doudou_question_t for the overlay API */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOUDOU_PET_IDLE = 0,
    DOUDOU_PET_THINKING,
    DOUDOU_PET_EXECUTING,
    DOUDOU_PET_WAITING,
    DOUDOU_PET_DONE,
    DOUDOU_PET_ERROR,
    /* Firmware-local. Not in the wire protocol — auto-enters when the
     * device has had no activity for a minute, exits as soon as a real
     * state arrives. Eye sprite: doudou_eye_sleep. */
    DOUDOU_PET_SLEEPING,
} doudou_pet_state_t;

typedef enum {
    DOUDOU_SCREEN_INFO = 0,
    DOUDOU_SCREEN_USAGE,
    DOUDOU_SCREEN_PET,
    DOUDOU_SCREEN_HISTORY,
    DOUDOU_SCREEN_COUNT
} doudou_screen_id_t;

esp_err_t doudou_pet_ui_init(void);

void doudou_pet_set_state(doudou_pet_state_t state);
/** Accepts the wire-format state string (e.g. "idle", "thinking"). */
void doudou_pet_set_state_str(const char *state_str);
void doudou_pet_set_title(const char *title);          /* shown above pet */
void doudou_pet_set_status(const char *status);        /* shown below pet */
void doudou_pet_wiggle(void);                          /* one-shot react */
void doudou_pet_show_bubble(const char *text);         /* small bubble above pet, auto-fades */
/** If currently SLEEPING, flash a startled face (WAITING) then settle
 *  into IDLE after ~900ms; returns true and consumes the gesture.
 *  If not sleeping, returns false so callers can fall back to wiggle. */
bool doudou_pet_wake_from_sleep(void);

/** Switch active screen with LVGL slide animation. */
void doudou_screen_show(doudou_screen_id_t which);
/** Advance current screen by ±1 (with wrap stop at edges). */
void doudou_screen_shift(int direction);
doudou_screen_id_t doudou_screen_current(void);

/* ---- Real data setters (called by bridge_client callbacks) ---- */

typedef struct {
    const char *session_id;
    const char *thread_title;
    const char *source;
    const char *model;
    const char *reasoning_effort;
    const char *summary_mode;
    const char *cwd;
    const char *permissions;
    const char *collaboration_mode;
    const char *account_email;
    const char *plan_type;
    int agents_md;       /* -1 unknown, 0 false, 1 true */
} doudou_ui_session_info_t;

typedef struct {
    const char *id;
    const char *label;
    const char *group_label;
    int used_pct;
    int window_minutes;
    int64_t resets_at_ms;
} doudou_ui_limit_t;

typedef struct {
    bool has_session;
    int64_t input_tokens, output_tokens, cached_tokens, total_tokens;
    int64_t current_context_tokens, model_context_window;
    const char *plan_type;
    const doudou_ui_limit_t *limits;
    int n_limits;
} doudou_ui_usage_t;

typedef struct {
    const char *id;
    const char *title;
    const char *source;
    bool active;
    int64_t updated_at_ms;
} doudou_ui_thread_t;

void doudou_pet_set_session_info(const doudou_ui_session_info_t *info);
void doudou_pet_set_usage(const doudou_ui_usage_t *usage);
void doudou_pet_set_threads(const doudou_ui_thread_t *threads, int n);

typedef void (*doudou_thread_click_cb_t)(const char *thread_id);
void doudou_pet_set_thread_click_cb(doudou_thread_click_cb_t cb);

/* ---- Question overlay ---- */

/** Called when the user taps a choice on the question overlay. */
typedef void (*doudou_question_reply_cb_t)(const char *question_id, const char *choice_id);
void doudou_pet_set_question_cb(doudou_question_reply_cb_t cb);

/** Show the question overlay on top of the PET screen. */
void doudou_pet_show_question(const doudou_question_t *q);
void doudou_pet_hide_question(void);

#ifdef __cplusplus
}
#endif
