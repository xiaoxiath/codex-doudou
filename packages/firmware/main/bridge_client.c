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

/* Reassembly buffer for fragmented frames. */
static char *s_rx_buf = NULL;
static size_t s_rx_buf_cap = 0;
static size_t s_rx_buf_len = 0;

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
    return send_owned(doudou_build_reply(s_out_seq++, local_ts_ms(),
                                         CONFIG_DOUDOU_DEVICE_ID,
                                         question_id, choice_id));
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
            send_hello(); /* re-handshake; harmless on dup */
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
    } else if (!strcmp(type, "pong") || !strcmp(type, "ack")) {
        /* no-op */
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

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_connected = true;
            s_out_seq = 1;
            s_start_us = esp_timer_get_time();
            ESP_LOGI(TAG, "ws connected");
            if (s_h.on_connected) s_h.on_connected();
            send_hello();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "ws disconnected");
            if (s_h.on_disconnected) s_h.on_disconnected();
            rx_reset();
            break;
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
                ESP_LOGD(TAG, "ws control ping");
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "ws error");
            break;
        default: break;
    }
}

esp_err_t doudou_bridge_connect(const char *url, const doudou_bridge_handlers_t *h)
{
    if (!url || !h) return ESP_ERR_INVALID_ARG;
    s_h = *h;

    if (!s_send_mutex) s_send_mutex = xSemaphoreCreateMutex();

    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 8000,
        .buffer_size = 4096,
        .task_stack = 6144,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                                  ws_event_handler, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
    ESP_LOGI(TAG, "ws client → %s", url);
    return ESP_OK;
}
