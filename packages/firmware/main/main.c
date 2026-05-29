/**
 * main.c — Doudou firmware entry, MVP-1b: real Codex data over WebSocket.
 *
 * Boot order:
 *   display → touch → lvgl → pet_ui → net (wifi+mdns) → bridge_client (ws)
 *
 * Touch task handles taps + slide gestures (slide → switch screen,
 * tap pet → wiggle reaction). Thread-list rows on the HISTORY screen
 * fire `doudou_bridge_follow_thread` when tapped.
 *
 * If Wi-Fi creds aren't configured (CONFIG_DOUDOU_WIFI_SSID empty),
 * everything except `bridge_client` still runs — the pet sits idle
 * waiting for a connection.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "touch.h"
#include "lvgl_port.h"
#include "pet_ui.h"
#include "lvgl.h"
#include "lv_font_cjk_14.h"
#include "whack.h"
#include "squash.h"
#include "bubble.h"
#include "feed.h"
#include "flappy.h"
#include "simon.h"
#include "game_menu.h"
#include "bridge_client.h"
#include "pins.h"
#include "sdkconfig.h"
#if !defined(CONFIG_DOUDOU_TRANSPORT_BLE)
#include "net.h"
#endif

static const char *TAG = "doudou";

/* ---------- net-ready watchdog ----------
 *
 * If Wi-Fi is unconfigured / wrong password / no AP in range, the
 * device would otherwise sit forever waiting for IP. After NET_WAIT_US
 * we give up and let the user play with the pet anyway. Once Wi-Fi
 * eventually does come up, `on_net_ready` will fire, bridge_connect
 * will run, and a successful WS handshake auto-exits toy mode. */
#define NET_WAIT_US ((uint64_t)15 * 1000 * 1000)

static esp_timer_handle_t s_net_wait_timer = NULL;

static void net_wait_cb(void *arg)
{
    (void)arg;
    if (!doudou_pet_toy_mode_active()) {
        ESP_LOGI(TAG, "Wi-Fi not ready after 15s → toy mode");
        doudou_pet_toy_mode_set(true);
    }
}

static void net_wait_arm(void)
{
    if (s_net_wait_timer) return;
    const esp_timer_create_args_t args = {
        .callback = net_wait_cb,
        .name     = "net_wait",
    };
    if (esp_timer_create(&args, &s_net_wait_timer) != ESP_OK) {
        ESP_LOGW(TAG, "net_wait timer create failed");
        return;
    }
    esp_timer_start_once(s_net_wait_timer, NET_WAIT_US);
}

static void net_wait_cancel(void)
{
    if (s_net_wait_timer) esp_timer_stop(s_net_wait_timer);
}

/* ---------- bridge → UI handlers ---------- */

static void on_ws_connected(void)
{
    net_wait_cancel();
    if (doudou_pet_toy_mode_active()) doudou_pet_toy_mode_set(false);

    doudou_pet_set_state(DOUDOU_PET_IDLE);
    /* Clear any leftover title so the next session_info push from bridge
     * lands on a clean widget. */
    doudou_pet_set_title("");
    doudou_pet_set_status("connected");
}

static void on_ws_disconnected(void)
{
    /* No grace, no "Bridge disconnected" banner — the kid doesn't care
     * why the link dropped. Flip straight to toy mode on the first
     * disconnect, then ignore subsequent reconnect-attempt failures
     * (toy_mode_set is idempotent). */
    if (!doudou_pet_toy_mode_active()) {
        ESP_LOGI(TAG, "WS down → toy mode");
        doudou_pet_toy_mode_set(true);
    }
}

static bool s_clock_synced = false;

static void on_welcome(uint64_t srv_ms, const char *sid)
{
    /* The first welcome gives us a real Unix-epoch timestamp from
     * Bridge — we use it to set the device's wall clock so anything
     * downstream (USAGE reset-time formatter, audit timestamps, log
     * timestamps) sees the right "now". RTC is volatile on the C3
     * we use, so this fires after every cold boot. */
    if (srv_ms > 0 && !s_clock_synced) {
        struct timeval tv = {
            .tv_sec  = (time_t)(srv_ms / 1000),
            .tv_usec = (suseconds_t)((srv_ms % 1000) * 1000),
        };
        if (settimeofday(&tv, NULL) == 0) {
            s_clock_synced = true;
            time_t now = (time_t)(srv_ms / 1000);
            struct tm tm_local;
            localtime_r(&now, &tm_local);
            char buf[40];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm_local);
            ESP_LOGI(TAG, "clock synced from bridge → %s", buf);
        } else {
            ESP_LOGW(TAG, "settimeofday failed");
        }
    }
    ESP_LOGI(TAG, "welcome session=%s server_time=%" PRIu64, sid ? sid : "?", srv_ms);
}

static void on_status(const doudou_status_t *s)
{
    if (s->state) doudou_pet_set_state_str(s->state);
    if (s->title) doudou_pet_set_status(s->title);
    if (s->body)  ESP_LOGD(TAG, "status body=%s", s->body);
}

static void on_session_info(const doudou_session_info_t *si)
{
    /* Copy bridge_client's struct into the UI's identical struct
     * (separate headers so they don't depend on each other). */
    doudou_ui_session_info_t ui = {
        .session_id         = si->session_id,
        .thread_title       = si->thread_title,
        .source             = si->source,
        .model              = si->model,
        .reasoning_effort   = si->reasoning_effort,
        .summary_mode       = si->summary_mode,
        .cwd                = si->cwd,
        .permissions        = si->permissions,
        .collaboration_mode = si->collaboration_mode,
        .account_email      = si->account_email,
        .plan_type          = si->plan_type,
        .agents_md          = si->agents_md,
    };
    doudou_pet_set_session_info(&ui);
    if (si->thread_title) doudou_pet_set_title(si->thread_title);
}

static void on_usage(const doudou_usage_t *u)
{
    doudou_ui_limit_t ui_limits[8];
    int n = u->n_limits < (int)(sizeof(ui_limits) / sizeof(ui_limits[0]))
            ? u->n_limits : (int)(sizeof(ui_limits) / sizeof(ui_limits[0]));
    for (int i = 0; i < n; i++) {
        ui_limits[i].id            = u->limits[i].id;
        ui_limits[i].label         = u->limits[i].label;
        ui_limits[i].group_label   = u->limits[i].group_label;
        ui_limits[i].used_pct      = u->limits[i].used_pct;
        ui_limits[i].window_minutes = u->limits[i].window_minutes;
        ui_limits[i].resets_at_ms  = u->limits[i].resets_at_ms;
    }
    doudou_ui_usage_t ui = {
        .has_session            = u->has_session,
        .input_tokens           = u->input_tokens,
        .output_tokens          = u->output_tokens,
        .cached_tokens          = u->cached_tokens,
        .total_tokens           = u->total_tokens,
        .current_context_tokens = u->current_context_tokens,
        .model_context_window   = u->model_context_window,
        .plan_type              = u->plan_type,
        .limits                 = ui_limits,
        .n_limits               = n,
    };
    doudou_pet_set_usage(&ui);
}

static void on_thread_list(const doudou_thread_t *threads, int n)
{
    doudou_ui_thread_t buf[12];
    int copy = n < (int)(sizeof(buf) / sizeof(buf[0])) ? n : (int)(sizeof(buf) / sizeof(buf[0]));
    for (int i = 0; i < copy; i++) {
        buf[i].id            = threads[i].id;
        buf[i].title         = threads[i].title;
        buf[i].source        = threads[i].source;
        buf[i].active        = threads[i].active;
        buf[i].updated_at_ms = threads[i].updated_at_ms;
    }
    doudou_pet_set_threads(buf, copy);
}

static void on_error(const char *code, const char *title, const char *body)
{
    ESP_LOGW(TAG, "bridge error code=%s title=%s body=%s",
             code ? code : "?", title ? title : "", body ? body : "");
    doudou_pet_set_state(DOUDOU_PET_ERROR);
    if (title) doudou_pet_set_title(title);
}

static void on_question(const doudou_question_t *q)
{
    if (!q) {
        ESP_LOGW(TAG, "on_question got NULL");
        return;
    }
    ESP_LOGI(TAG, "RECEIVED question id=%s risk=%s action=%s title=\"%s\" choices=%d",
             q->id ? q->id : "?",
             q->risk ? q->risk : "?",
             q->action_type ? q->action_type : "?",
             q->title ? q->title : "",
             q->n_choices);
    /* Surface as a waiting_input pet state so the screen and pose match,
     * then layer the question overlay on top of the PET screen. */
    doudou_pet_set_state(DOUDOU_PET_WAITING);
    if (q->title) doudou_pet_set_title(q->title);
    doudou_pet_show_question(q);
}

static void on_thread_clicked(const char *thread_id)
{
    ESP_LOGI(TAG, "ui requested follow_thread → %s", thread_id);
    doudou_bridge_follow_thread(thread_id);
    /* Bridge attaches to the new rollout and silently replays its
     * history — that can take several seconds for big sessions, during
     * which no events flow to the device and the screen looks frozen.
     * Give immediate visual ack: jump to PET, swap to THINKING (sky-
     * blue glow), and overwrite the title with a loading hint. Bridge's
     * next session_info / status push will overwrite all of this. */
    doudou_screen_show(DOUDOU_SCREEN_PET);
    doudou_pet_set_state(DOUDOU_PET_THINKING);
    doudou_pet_set_title("切换中...");
}

static void on_question_reply(const char *question_id, const char *choice_id)
{
    ESP_LOGI(TAG, "ui reply question_id=%s choice_id=%s", question_id, choice_id);
    doudou_bridge_reply(question_id, choice_id);
    /* Drop back to idle after answering; bridge will send a fresh status soon. */
    doudou_pet_set_state(DOUDOU_PET_IDLE);
}

/* ---------- net → bridge wiring ---------- */

static const doudou_bridge_handlers_t s_bridge_handlers = {
    .on_connected    = on_ws_connected,
    .on_disconnected = on_ws_disconnected,
    .on_welcome      = on_welcome,
    .on_status       = on_status,
    .on_session_info = on_session_info,
    .on_usage        = on_usage,
    .on_thread_list  = on_thread_list,
    .on_question     = on_question,
    .on_error        = on_error,
};

#if !defined(CONFIG_DOUDOU_TRANSPORT_BLE)
static void on_net_ready(const char *url)
{
    ESP_LOGI(TAG, "net ready → %s", url);
    /* Wi-Fi is up — kill the no-network watchdog. If the bridge is
     * unreachable, on_ws_disconnected will flip into toy mode instead. */
    net_wait_cancel();
    doudou_bridge_connect(url, &s_bridge_handlers);
}
#endif

/* ---------- touch input task ---------- */

/* Whether ANY game overlay is currently rendered on the PET screen. */
static bool any_game_active(void)
{
    return doudou_whack_active()  || doudou_squash_active()
        || doudou_bubble_active()
        || doudou_feed_active()   || doudou_flappy_active()
        || doudou_simon_active();
}

/* Stop whichever game is currently up and jump back to the GAMES
 * screen so the kid sees the launcher list again. */
static bool exit_to_menu_if_game_active(void)
{
    bool was_active = any_game_active();
    if (doudou_whack_active())  doudou_whack_stop();
    if (doudou_squash_active()) doudou_squash_stop();
    if (doudou_bubble_active()) doudou_bubble_stop();
    if (doudou_feed_active())   doudou_feed_stop();
    if (doudou_flappy_active()) doudou_flappy_stop();
    if (doudou_simon_active())  doudou_simon_stop();
    if (was_active) doudou_screen_show(DOUDOU_SCREEN_GAMES);
    return was_active;
}

/* ---------- in-game exit button ----------
 *
 * A small "✕" floating on the top-right of the PET screen, only
 * visible while a game overlay is up. Triggers on LV_EVENT_LONG_
 * PRESSED (default ~400 ms hold) so an accidental finger graze
 * during gameplay drag won't exit — the kid has to *intentionally*
 * park their finger on the button for nearly half a second. */
static lv_obj_t *s_exit_btn = NULL;

/* Button hit-area in screen coords. Tap-task watches double-taps
 * inside this rect to fire the exit. Top-centre placement so it's
 * easy to find but small enough (30×30) to leave Whack's centre-
 * top hole untouched (its bbox starts at y=38). */
#define EXIT_BTN_L  105
#define EXIT_BTN_R  135
#define EXIT_BTN_T  4
#define EXIT_BTN_B  34

static bool point_in_exit_btn(int x, int y)
{
    return x >= EXIT_BTN_L && x <= EXIT_BTN_R
        && y >= EXIT_BTN_T && y <= EXIT_BTN_B;
}

/* lv_timer that syncs the button visibility with game state. Tick
 * every 100 ms — cheap and good enough for "show/hide on game
 * start/stop". Also re-foregrounds the button so freshly-started
 * games don't bury it under their root overlay. */
static void exit_btn_sync_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_exit_btn) return;
    bool want = any_game_active();
    bool now = !lv_obj_has_flag(s_exit_btn, LV_OBJ_FLAG_HIDDEN);
    if (want && !now) {
        lv_obj_remove_flag(s_exit_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (!want && now) {
        lv_obj_add_flag(s_exit_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (want) lv_obj_move_foreground(s_exit_btn);
}

static void exit_btn_create(struct _lv_obj_t *pet_screen)
{
    if (!pet_screen) return;
    if (!doudou_lvgl_lock(500)) return;
    /* Use lv_obj instead of lv_button — and explicitly drop CLICKABLE.
     * The button is a visual hint only; LVGL skips it during hit
     * testing so short taps fall through to the game widget beneath.
     * Long-press detection happens manually in the touch task by
     * watching e.x/e.y against the button rect. */
    s_exit_btn = lv_obj_create(pet_screen);
    lv_obj_set_size(s_exit_btn, EXIT_BTN_R - EXIT_BTN_L, EXIT_BTN_B - EXIT_BTN_T);
    lv_obj_set_pos(s_exit_btn, EXIT_BTN_L, EXIT_BTN_T);
    lv_obj_set_style_radius(s_exit_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_exit_btn, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_exit_btn, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_exit_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_exit_btn, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_exit_btn, LV_OPA_60, LV_PART_MAIN);
    lv_obj_clear_flag(s_exit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_exit_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(s_exit_btn);
    lv_obj_set_style_text_font(l, &lv_font_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0xe6e9ee), LV_PART_MAIN);
    lv_label_set_text(l, "X");
    lv_obj_center(l);
    lv_obj_add_flag(s_exit_btn, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(exit_btn_sync_cb, 100, NULL);
    doudou_lvgl_unlock();
}

static void touch_input_task(void *arg)
{
    (void)arg;
    doudou_touch_event_t last = {0};
    int64_t last_armed_us  = 0;
    int     press_streak   = 0;     /* consecutive 'finger>0' reads */
    bool    was_armed      = false;
    /* Strict per-press dedup: exactly one gesture fires per physical
     * touch. Start "fired" so any boot-time phantom gesture is ignored
     * until we've actually seen a finger touch the panel. */
    bool fired_this_press  = true;
    while (1) {
        doudou_touch_event_t e = {0};
        doudou_touch_read(&e);
        int64_t now_us = esp_timer_get_time();

        /* Debounce: the CST816D periodically self-fires its gesture
         * register WITHOUT any real touch (observed: ~0.4 Hz of stray
         * gesture=0x03 on idle hardware). Single-cycle 'finger>0'
         * glitches do happen too — and a stray finger+stray gesture
         * landing in the same 10 ms poll used to slip through.
         *
         * Fix: a press must be held for ≥2 consecutive reads (~20 ms)
         * before we'll arm a gesture window. Real touches always last
         * 50+ ms, so we never lose a legit press. */
        if (e.pressed) {
            if (press_streak < 8) press_streak++;
        } else {
            press_streak = 0;
        }
        /* Require ≥5 consecutive 'pressed' reads (~50 ms) before arming.
         * Real touches always last 50+ ms; transient I²C / EMC glitches
         * rarely sustain that long. Bumped from 2 because we observed
         * a 150-ms phantom-press burst on idle hw that passed the lower
         * threshold and fired a phantom screen_shift. */
        bool armed = press_streak >= 5;

        if (armed) {
            last_armed_us = now_us;
            if (!was_armed) {
                /* Rising edge — arm one gesture for this touch. */
                fired_this_press = false;
            }
        }
        was_armed = armed;


        /* Phantom-filter: a confirmed press must have happened in the
         * last 800 ms. Without this, a cold-start gesture latch would
         * slip through before any press has been observed. */
        bool recent_press = (now_us - last_armed_us) < 800 * 1000;

        if (recent_press
            && !fired_this_press
            && e.gesture != last.gesture
            && e.gesture != DOUDOU_GESTURE_NONE) {
            fired_this_press = true;
            ESP_LOGI(TAG, "hw gesture transition %02x → %02x", last.gesture, e.gesture);
            switch (e.gesture) {
                case DOUDOU_GESTURE_SLIDE_UP:
                case DOUDOU_GESTURE_SLIDE_DOWN:
                case DOUDOU_GESTURE_SLIDE_LEFT:
                case DOUDOU_GESTURE_SLIDE_RIGHT:
                    /* Game active: all swipes belong to gameplay
                     * (paddle drag, brush stroke, food drag, jittery
                     * taps on Whack-a-Mole). Swallow them — the only
                     * exit path during play is the floating ✕ button
                     * long-press. */
                    if (any_game_active()) break;

                    if (e.gesture == DOUDOU_GESTURE_SLIDE_DOWN) {
                        /* Swipe-down on PET → switch to the GAMES
                         * launcher screen (in both online + toy mode). */
                        if (doudou_screen_current() == DOUDOU_SCREEN_PET) {
                            doudou_screen_show(DOUDOU_SCREEN_GAMES);
                        }
                    } else if (e.gesture == DOUDOU_GESTURE_SLIDE_UP) {
                        /* Swipe-up on the GAMES launcher → back to PET. */
                        if (doudou_screen_current() == DOUDOU_SCREEN_GAMES) {
                            doudou_screen_show(DOUDOU_SCREEN_PET);
                        }
                    } else {
                        /* Horizontal swipe = INFO/USAGE/PET/HISTORY
                         * carousel; blocked in toy mode (those screens
                         * need Bridge data that's gone offline). */
                        if (doudou_screen_current() == DOUDOU_SCREEN_GAMES) {
                            /* Same as swipe-up on GAMES — back to PET. */
                            doudou_screen_show(DOUDOU_SCREEN_PET);
                        } else if (doudou_pet_toy_mode_active()) {
                            ESP_LOGI(TAG, "toy: slide ignored");
                        } else {
                            doudou_screen_shift(
                                e.gesture == DOUDOU_GESTURE_SLIDE_LEFT ? +1 : -1);
                        }
                    }
                    break;
                case DOUDOU_GESTURE_SINGLE_TAP:
                case DOUDOU_GESTURE_DOUBLE_TAP: {
                    /* CST816D's hardware DOUBLE_TAP register (0x0B) is
                     * unreliable — it often reports two SINGLE_TAPs in
                     * a row instead. Synthesize double-tap in software
                     * by timing two consecutive SINGLE_TAP gestures.
                     * Hardware-reported DOUBLE_TAP still works (folded
                     * into the same branch via `is_double = true`). */
                    static int64_t s_last_tap_us = 0;
                    int64_t tap_now = esp_timer_get_time();
                    bool is_double = (e.gesture == DOUDOU_GESTURE_DOUBLE_TAP)
                                  || (s_last_tap_us != 0
                                      && (tap_now - s_last_tap_us) < 450 * 1000);
                    /* Reset so a 3rd tap doesn't re-fire double-tap. */
                    s_last_tap_us = is_double ? 0 : tap_now;

                    /* Double-tap inside the floating X button =
                     * back to the GAMES screen. Works in any game
                     * (highest-priority check so doodle's "double-
                     * tap clears" doesn't fire on exit). */
                    if (is_double && any_game_active()
                        && point_in_exit_btn(e.x, e.y)) {
                        ESP_LOGI(TAG, "exit-button double-tap → back to GAMES");
                        exit_to_menu_if_game_active();
                        break;
                    }
                    /* On the GAMES screen, LVGL routes taps to the
                     * row buttons. Just swallow at the touch-task
                     * level so we don't also wiggle the pet. */
                    if (doudou_screen_current() == DOUDOU_SCREEN_GAMES) break;
                    if (doudou_screen_current() != DOUDOU_SCREEN_PET) break;

                    /* Game wins: tap routes to whichever game is up.
                     * Whack + Bubble + Simon use LVGL CLICKED on
                     * their widgets, Doodle + Feed poll touch from
                     * their tick timer — most cases just need to
                     * swallow the event so it doesn't also wiggle. */
                    if (doudou_whack_active())  break;
                    if (doudou_bubble_active()) break;
                    if (doudou_simon_active())  break;
                    if (doudou_feed_active())   break;
                    if (doudou_squash_active()) {
                        doudou_squash_tap();
                        break;
                    }
                    if (doudou_flappy_active()) {
                        doudou_flappy_flap();
                        break;
                    }

                    /* Sleeping → wake takes priority regardless of single/double. */
                    if (doudou_pet_wake_from_sleep()) {
                        ESP_LOGI(TAG, "tap → wake from sleep");
                        break;
                    }

                    if (is_double) {
                        if (doudou_pet_toy_mode_active()) {
                            doudou_pet_toy_tap();           /* cycle expression */
                        } else {
                            ESP_LOGI(TAG, "double-tap (online) → wiggle");
                            doudou_pet_wiggle();
                        }
                    } else {
                        ESP_LOGI(TAG, "single-tap → wiggle");
                        doudou_pet_wiggle();
                    }
                    break;
                }
                case DOUDOU_GESTURE_LONG_PRESS:
                    /* CST816D's ~500-ms LONG_PRESS is intentionally
                     * ignored: in-game gameplay involves users pausing
                     * mid-drag for far longer than 500 ms (think of
                     * picking up a food token in Feed), so we'd false-
                     * trigger constantly. The real exit gesture is the
                     * 5-second static-press detector at the top of
                     * this loop. Out-of-game the gesture is just noise. */
                    break;
                default: break;
            }
        }
        last = e;
        vTaskDelay(pdMS_TO_TICKS(10));   /* 100Hz polling — catch fast flicks */
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "doudou firmware boot, target=esp32c3, MVP-1b");

    /* Lock the device timezone before Wi-Fi/bridge come up so that as
     * soon as `on_welcome` sets the wall clock, `localtime_r` already
     * yields the user-facing wall time. CST-8 = Asia/Shanghai (UTC+8).
     * If the target market changes, override at compile time. */
    setenv("TZ", "CST-8", 1);
    tzset();

    if (doudou_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "display init failed; halting");
        return;
    }
    if (doudou_touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed — UI works without swipe");
    }
    if (doudou_lvgl_init() != ESP_OK) {
        ESP_LOGE(TAG, "lvgl init failed; halting");
        return;
    }
    if (doudou_pet_ui_init() != ESP_OK) {
        ESP_LOGE(TAG, "pet UI init failed; halting");
        return;
    }
    doudou_pet_set_thread_click_cb(on_thread_clicked);
    doudou_pet_set_question_cb(on_question_reply);

    /* Mini-game lives as a hidden overlay on the PET screen. Started
     * by a swipe-up gesture while in toy mode (see touch_input_task). */
    {
        struct _lv_obj_t *pet = doudou_pet_screen_root();
        if (doudou_whack_init (pet) != ESP_OK) ESP_LOGW(TAG, "whack init failed");
        if (doudou_squash_init(pet) != ESP_OK) ESP_LOGW(TAG, "squash init failed");
        if (doudou_bubble_init(pet) != ESP_OK) ESP_LOGW(TAG, "bubble init failed");
        if (doudou_feed_init  (pet) != ESP_OK) ESP_LOGW(TAG, "feed init failed");
        if (doudou_flappy_init(pet) != ESP_OK) ESP_LOGW(TAG, "flappy init failed");
        if (doudou_simon_init (pet) != ESP_OK) ESP_LOGW(TAG, "simon init failed");
        /* Floating exit-button — owned by main.c so it can reach
         * across to any game's stop() + the screen switcher. */
        exit_btn_create(pet);
    }

    xTaskCreate(touch_input_task, "touch", 4096, NULL, 5, NULL);

#if defined(CONFIG_DOUDOU_TRANSPORT_BLE)
    /* BLE transport — no Wi-Fi handshake, no mDNS. The peripheral starts
     * advertising immediately; Bridge's BLE central will scan + connect
     * when the user runs it with DOUDOU_BLE=1. */
    doudou_pet_set_title("等待 Bridge 连接");
    doudou_pet_set_status("BLE 广播中...");
    esp_err_t be = doudou_bridge_connect(NULL, &s_bridge_handlers);
    if (be != ESP_OK) {
        ESP_LOGE(TAG, "ble bring-up failed: %s", esp_err_to_name(be));
    }
#else
    /* Network is optional — if Wi-Fi isn't configured / can't associate,
     * we fall back to the standalone offline pet so the device is
     * never just a dead screen. */
    esp_err_t e = doudou_net_start(on_net_ready);
    if (e == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "Wi-Fi not configured → toy mode immediately");
        doudou_pet_toy_mode_set(true);
    } else if (e != ESP_OK) {
        ESP_LOGE(TAG, "net start failed: %s", esp_err_to_name(e));
        doudou_pet_toy_mode_set(true);
    } else {
        /* Wi-Fi is starting; arm a 15-s watchdog so a stuck association
         * doesn't leave the device idle forever. */
        net_wait_arm();
    }
#endif

    ESP_LOGI(TAG, "boot complete");
}
