/**
 * bridge_client.c — WS transport + dispatch.
 *
 * Reassembles fragmented WS frames into a single buffer. Pure
 * parse/build is delegated to `protocol_parse.{c,h}` so it can be
 * exercised by host unit tests. This file only handles the IDF-
 * specific concerns: WS client, mutex, seq/ts stamping, dispatch.
 */
#include "bridge_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "protocol_parse.h"

static const char *TAG = "bridge";

#define DEVICE_FW_VERSION "0.1.0-fw"
#define RX_BUF_MAX_BYTES  (16 * 1024)

static esp_websocket_client_handle_t s_ws = NULL;
static doudou_bridge_handlers_t s_h = {0};
static SemaphoreHandle_t s_send_mutex = NULL;
static uint32_t s_out_seq = 1;
static int64_t s_start_us = 0;
static bool s_connected = false;
static int64_t s_last_outbound_us = 0;          /* used by keepalive */
static int64_t s_last_disconnect_us = 0;        /* used by watchdog */
static char s_saved_url[160] = {0};             /* for watchdog re-init */

/* Reassembly buffer for fragmented frames. */
static char *s_rx_buf = NULL;
static size_t s_rx_buf_cap = 0;
static size_t s_rx_buf_len = 0;

/* Pending reply: if the user taps a choice while WS is down, we stash it
 * here and replay on the next CONNECTED. Bridge dedupes by question id
 * (repliedIds) so a double-replay is safe. */
static char s_pending_reply_qid[64];
static char s_pending_reply_cid[24];
static bool s_pending_reply_armed = false;

static uint32_t local_ts_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
}

/* ---------- send helpers ---------- */

static esp_err_t send_owned(cJSON *msg)
{
    if (!msg) return ESP_ERR_NO_MEM;
    if (!s_ws || !s_connected) {
        cJSON_Delete(msg);
        return ESP_ERR_INVALID_STATE;
    }
    char *body = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!body) return ESP_ERR_NO_MEM;

    if (s_send_mutex) xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    int sent = esp_websocket_client_send_text(s_ws, body, strlen(body), pdMS_TO_TICKS(2000));
    if (s_send_mutex) xSemaphoreGive(s_send_mutex);

    free(body);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_hello(void)
{
    return send_owned(doudou_build_hello(s_out_seq++, local_ts_ms(),
                                         CONFIG_DOUDOU_DEVICE_ID,
                                         DEVICE_FW_VERSION,
                                         CONFIG_DOUDOU_PAIRING_TOKEN));
}

static esp_err_t send_pong(uint32_t pong_for_seq)
{
    return send_owned(doudou_build_pong(s_out_seq++, local_ts_ms(), pong_for_seq));
}

esp_err_t doudou_bridge_follow_thread(const char *thread_id)
{
    if (!thread_id || !*thread_id) return ESP_ERR_INVALID_ARG;
    return send_owned(doudou_build_follow_thread(s_out_seq++, local_ts_ms(), thread_id));
}

esp_err_t doudou_bridge_reply(const char *question_id, const char *choice_id)
{
    if (!question_id || !choice_id) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "send reply: qid=%s choice=%s ws_connected=%d",
             question_id, choice_id, s_connected ? 1 : 0);
    esp_err_t err = send_owned(doudou_build_reply(s_out_seq++, local_ts_ms(),
                                                  CONFIG_DOUDOU_DEVICE_ID,
                                                  question_id, choice_id));
    if (err == ESP_OK) {
        /* Sent on the wire — bridge dedup will swallow any later replay,
         * so we can safely keep the pending slot armed in case the WS
         * dropped before bridge processed it. Refreshed on every tap. */
        strncpy(s_pending_reply_qid, question_id, sizeof(s_pending_reply_qid) - 1);
        s_pending_reply_qid[sizeof(s_pending_reply_qid) - 1] = '\0';
        strncpy(s_pending_reply_cid, choice_id, sizeof(s_pending_reply_cid) - 1);
        s_pending_reply_cid[sizeof(s_pending_reply_cid) - 1] = '\0';
        s_pending_reply_armed = true;
    } else {
        ESP_LOGW(TAG, "reply send failed: %s — buffered for retry on reconnect",
                 esp_err_to_name(err));
        /* Buffer for replay even if we never got it on the wire. */
        strncpy(s_pending_reply_qid, question_id, sizeof(s_pending_reply_qid) - 1);
        s_pending_reply_qid[sizeof(s_pending_reply_qid) - 1] = '\0';
        strncpy(s_pending_reply_cid, choice_id, sizeof(s_pending_reply_cid) - 1);
        s_pending_reply_cid[sizeof(s_pending_reply_cid) - 1] = '\0';
        s_pending_reply_armed = true;
    }
    return err;
}

/* Re-send a buffered reply after a fresh connect. Called from
 * WEBSOCKET_EVENT_CONNECTED after the hello handshake. Bridge will
 * dedupe by question id (repliedIds in deviceRegistry.ts) so this is
 * idempotent. */
static void flush_pending_reply(void)
{
    if (!s_pending_reply_armed) return;
    ESP_LOGI(TAG, "flush pending reply qid=%s choice=%s",
             s_pending_reply_qid, s_pending_reply_cid);
    esp_err_t err = send_owned(doudou_build_reply(s_out_seq++, local_ts_ms(),
                                                  CONFIG_DOUDOU_DEVICE_ID,
                                                  s_pending_reply_qid,
                                                  s_pending_reply_cid));
    if (err == ESP_OK) {
        /* Keep armed for one more flush in case THIS connection also
         * dies before bridge sees the reply. It'll be cleared by an
         * inbound ack message — but if no ack arrives we just keep
         * replaying, harmless thanks to bridge dedup. */
    } else {
        ESP_LOGW(TAG, "flush pending reply failed: %s", esp_err_to_name(err));
    }
}

/* ---------- inbound dispatch ---------- */

static void parse_and_dispatch(const char *data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "json parse failed (%d bytes)", (int)len);
        return;
    }
    const char *type = doudou_json_str(root, "type");
    if (!type) { cJSON_Delete(root); return; }

    if (!strcmp(type, "welcome")) {
        uint64_t srv = 0;
        const char *sid = NULL;
        if (doudou_parse_welcome(root, &srv, &sid)) {
            if (s_h.on_welcome) s_h.on_welcome(srv, sid);
            ESP_LOGI(TAG, "welcome session=%s server_time=%" PRIu64,
                     sid ? sid : "?", srv);
            /* No re-handshake — we already sent hello on WS connect
             * (see WEBSOCKET_EVENT_CONNECTED). Sending again here just
             * triggers a `duplicate hello` warn on the bridge. */
        }
    } else if (!strcmp(type, "status")) {
        doudou_status_t s;
        if (doudou_parse_status(root, &s) && s_h.on_status) s_h.on_status(&s);
    } else if (!strcmp(type, "session_info")) {
        doudou_session_info_t si;
        if (doudou_parse_session_info(root, &si) && s_h.on_session_info) s_h.on_session_info(&si);
    } else if (!strcmp(type, "usage")) {
        doudou_usage_t u;
        if (doudou_parse_usage(root, &u) && s_h.on_usage) s_h.on_usage(&u);
    } else if (!strcmp(type, "thread_list")) {
        doudou_thread_t threads[12];
        int n = doudou_parse_thread_list(root, threads,
                                         (int)(sizeof(threads)/sizeof(threads[0])));
        if (n >= 0 && s_h.on_thread_list) s_h.on_thread_list(threads, n);
    } else if (!strcmp(type, "question")) {
        doudou_question_t q;
        if (doudou_parse_question(root, &q) && s_h.on_question) s_h.on_question(&q);
    } else if (!strcmp(type, "error")) {
        const char *code = NULL, *title = NULL, *body = NULL;
        if (doudou_parse_error(root, &code, &title, &body) && s_h.on_error)
            s_h.on_error(code, title, body);
    } else if (!strcmp(type, "ping")) {
        uint32_t seq = (uint32_t)doudou_json_i64(root, "seq", 0);
        send_pong(seq);
    } else if (!strcmp(type, "pong")) {
        /* no-op — bridge confirmed our pong/keepalive */
    } else if (!strcmp(type, "ack")) {
        /* Bridge ack'd a frame. If we had a pending reply buffered for
         * retry, this is the signal that it reached bridge — clear it
         * so a future reconnect doesn't keep re-flushing. We don't
         * check the seq number because bridge dedupes by question id
         * (DeviceState.repliedIds) so being slightly loose is safe. */
        if (s_pending_reply_armed) {
            ESP_LOGI(TAG, "ack received, clearing pending reply qid=%s",
                     s_pending_reply_qid);
            s_pending_reply_armed = false;
        }
    } else {
        ESP_LOGD(TAG, "ignored msg type=%s", type);
    }

    cJSON_Delete(root);
}

/* ---------- WebSocket event handler ---------- */

static void rx_reset(void)
{
    s_rx_buf_len = 0;
}

static esp_err_t rx_append(const char *data, size_t data_len, size_t total_len)
{
    if (total_len > RX_BUF_MAX_BYTES) {
        ESP_LOGW(TAG, "incoming frame too large (%d > %d), drop", (int)total_len, RX_BUF_MAX_BYTES);
        rx_reset();
        return ESP_ERR_NO_MEM;
    }
    if (s_rx_buf_cap < total_len + 1) {
        char *nb = realloc(s_rx_buf, total_len + 1);
        if (!nb) return ESP_ERR_NO_MEM;
        s_rx_buf = nb;
        s_rx_buf_cap = total_len + 1;
    }
    memcpy(s_rx_buf + s_rx_buf_len, data, data_len);
    s_rx_buf_len += data_len;
    s_rx_buf[s_rx_buf_len] = '\0';
    return ESP_OK;
}

static int64_t s_ws_connected_at_us = 0;
static uint32_t s_ws_disconnect_count = 0;

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_connected = true;
            s_out_seq = 1;
            s_start_us = esp_timer_get_time();
            s_ws_connected_at_us = s_start_us;
            s_last_outbound_us = s_start_us;
            ESP_LOGI(TAG, "WS_CONNECTED (cycle #%lu)", (unsigned long)(s_ws_disconnect_count + 1));
            if (s_h.on_connected) s_h.on_connected();
            send_hello();
            s_last_outbound_us = esp_timer_get_time();
            /* If user tapped during a previous WS-down window, replay
             * the reply now so the in-flight question on bridge resolves. */
            flush_pending_reply();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED: {
            s_connected = false;
            s_ws_disconnect_count++;
            s_last_disconnect_us = esp_timer_get_time();
            int64_t lived_ms = (s_last_disconnect_us - s_ws_connected_at_us) / 1000;
            /* IDF wraps a few extra fields in the payload: HTTP status code,
             * close status from server, and any saved ESP errno from the
             * underlying transport. Print all of them so we can correlate
             * disconnects with what the bridge or network actually said. */
            ESP_LOGW(TAG, "WS_DISCONNECTED #%lu after %lld ms  "
                          "handshake_status=%d  errno=%d  err_type=%d  payload_len=%d",
                     (unsigned long)s_ws_disconnect_count,
                     (long long)lived_ms,
                     e ? e->error_handle.esp_ws_handshake_status_code : -1,
                     e ? e->error_handle.esp_transport_sock_errno : -1,
                     e ? (int)e->error_handle.error_type : -1,
                     e ? e->data_len : -1);
            if (s_h.on_disconnected) s_h.on_disconnected();
            rx_reset();
            break;
        }
        case WEBSOCKET_EVENT_DATA:
            if (e->op_code == 0x01 || e->op_code == 0x00) {  /* text or continuation */
                size_t total = e->payload_len > 0 ? (size_t)e->payload_len : (size_t)e->data_len;
                if (rx_append(e->data_ptr, e->data_len, total) == ESP_OK) {
                    if (s_rx_buf_len >= total) {
                        parse_and_dispatch(s_rx_buf, s_rx_buf_len);
                        rx_reset();
                    }
                }
            } else if (e->op_code == 0x09) {
                ESP_LOGI(TAG, "WS_PING from server (auto-pong'd by ws lib)");
            } else if (e->op_code == 0x0A) {
                ESP_LOGI(TAG, "WS_PONG from server");
            } else if (e->op_code == 0x08) {
                ESP_LOGW(TAG, "WS_CLOSE frame from server, payload_len=%d", e->data_len);
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "WS_ERROR  errno=%d  err_type=%d  http_status=%d",
                     e ? e->error_handle.esp_transport_sock_errno : -1,
                     e ? (int)e->error_handle.error_type : -1,
                     e ? e->error_handle.esp_ws_handshake_status_code : -1);
            break;
        case WEBSOCKET_EVENT_BEFORE_CONNECT:
            ESP_LOGI(TAG, "WS_BEFORE_CONNECT (attempting handshake)");
            break;
        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(TAG, "WS_CLOSED (client stopped)");
            break;
        default:
            ESP_LOGI(TAG, "WS event id=%ld (unhandled)", (long)id);
            break;
    }
}

/* WS keepalive + watchdog task.
 *
 * Two responsibilities:
 *  1. Active keepalive — every 20 s, if connected, send a tiny `ping`
 *     frame (application-layer, our own protocol). This proves the
 *     socket is bidirectionally alive even when bridge has nothing to
 *     say. Without it, NAT routers + corporate Wi-Fi APs sometimes
 *     reap idle connections; we observed disconnects ~6-7 s into
 *     otherwise-quiet sessions that this fixes.
 *  2. Stuck-state recovery — if we've been disconnected for >30 s
 *     with no successful reconnect, force-recreate the WS client. The
 *     IDF reconnect machinery can wedge in rare cases (we saw it
 *     today: WS task alive, Wi-Fi up, but client just never tried
 *     to reconnect). A hard restart kicks it free.
 *
 * Period 5 s — coarse enough to be cheap, fine enough to feel snappy. */
/* 10 s keepalive — short enough that even with one packet lost (or a
 * brief Wi-Fi blip), the next ping arrives well within bridge's
 * heartbeat window (HEARTBEAT_MS=15 s × GRACE=2 = 30 s on bridge). */
#define KEEPALIVE_INTERVAL_US  ((int64_t)10 * 1000 * 1000)
#define STUCK_THRESHOLD_US     ((int64_t)30 * 1000 * 1000)

static void send_keepalive_ping(void)
{
    if (!s_ws || !s_connected) return;
    cJSON *m = cJSON_CreateObject();
    if (!m) return;
    cJSON_AddStringToObject(m, "type",        "ping");
    cJSON_AddNumberToObject(m, "v",            1);
    cJSON_AddNumberToObject(m, "seq",          s_out_seq++);
    cJSON_AddNumberToObject(m, "ts",           local_ts_ms());
    send_owned(m);
    s_last_outbound_us = esp_timer_get_time();
}

static void recreate_ws_client(void)
{
    ESP_LOGW(TAG, "watchdog: WS stuck disconnected > 30s, recreating client");
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    esp_websocket_client_config_t cfg = {
        .uri = s_saved_url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 90000,
        .buffer_size = 4096,
        .task_stack = 6144,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "watchdog: ws client re-init failed");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);
    esp_websocket_client_start(s_ws);
    s_last_disconnect_us = esp_timer_get_time();
}

static void keepalive_task(void *arg)
{
    (void)arg;
    int64_t s_last_status_log_us = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));   /* poll cadence */
        int64_t now_us = esp_timer_get_time();
        /* Periodic state heartbeat to UART (every 60 s) so we can
         * spot split-brain: device thinks connected, bridge thinks
         * not. We'll observe disconnect_count + s_connected mismatch
         * over time. */
        if (now_us - s_last_status_log_us > 60 * 1000 * 1000) {
            s_last_status_log_us = now_us;
            ESP_LOGI(TAG, "ws status: connected=%d disconnects=%lu",
                     s_connected ? 1 : 0,
                     (unsigned long)s_ws_disconnect_count);
        }
        if (s_connected) {
            /* Quiet window? Send a tiny ping to refresh the link. */
            if (now_us - s_last_outbound_us > KEEPALIVE_INTERVAL_US) {
                send_keepalive_ping();
            }
        } else {
            /* Disconnected too long with no reconnect — kick the client. */
            if (s_last_disconnect_us > 0
                && now_us - s_last_disconnect_us > STUCK_THRESHOLD_US) {
                recreate_ws_client();
            }
        }
    }
}

esp_err_t doudou_bridge_connect(const char *url, const doudou_bridge_handlers_t *h)
{
    if (!url || !h) return ESP_ERR_INVALID_ARG;
    s_h = *h;
    strncpy(s_saved_url, url, sizeof(s_saved_url) - 1);
    s_saved_url[sizeof(s_saved_url) - 1] = '\0';

    if (!s_send_mutex) s_send_mutex = xSemaphoreCreateMutex();

    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .reconnect_timeout_ms = 5000,
        /* Bridge's application-layer ping every 15s is what really
         * proves the link is alive. Network timeout just needs to be
         * larger than that interval so the WS lib doesn't kill the
         * socket during normal quiet windows. Was 8000 originally,
         * which caused device-side self-disconnects on every gap. */
        .network_timeout_ms = 90000,
        .buffer_size = 4096,
        .task_stack = 6144,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                                  ws_event_handler, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
    ESP_LOGI(TAG, "ws client → %s", url);

    /* One-shot keepalive task creation (idempotent if called twice). */
    static bool s_keepalive_started = false;
    if (!s_keepalive_started) {
        xTaskCreate(keepalive_task, "ws_keepalive", 3072, NULL, 5, NULL);
        s_keepalive_started = true;
    }
    return ESP_OK;
}
