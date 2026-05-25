/**
 * bridge_client.h — WebSocket client + device-protocol v1 dispatcher.
 *
 * Owns the connection to Doudou Bridge. Parses every inbound JSON
 * message and fans out to typed handlers. Sends hello / pong / reply /
 * follow_thread on demand.
 *
 * Handler functions are called from the WebSocket client task. UI
 * handlers are expected to acquire the LVGL lock themselves.
 *
 * Parsing/serialization lives in `protocol_parse.h` (IDF-free + host-
 * testable). This header just declares the IDF-bound API.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "protocol_parse.h"   /* types: doudou_status_t / session_info / usage / thread */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_connected)(void);
    void (*on_disconnected)(void);
    void (*on_welcome)(uint64_t server_time_ms, const char *session_id);
    void (*on_status)(const doudou_status_t *);
    void (*on_session_info)(const doudou_session_info_t *);
    void (*on_usage)(const doudou_usage_t *);
    void (*on_thread_list)(const doudou_thread_t *threads, int n);
    void (*on_question)(const doudou_question_t *q);
    void (*on_error)(const char *code, const char *title, const char *body);
} doudou_bridge_handlers_t;

/** Connect (or reconnect) to the given ws:// URL. Safe to call again
 *  with a new URL to switch Bridge endpoints. */
esp_err_t doudou_bridge_connect(const char *url, const doudou_bridge_handlers_t *h);

/** Tell the Bridge we want to follow a specific thread (UI action). */
esp_err_t doudou_bridge_follow_thread(const char *thread_id);

/** Send a reply to an inflight question (MVP-1b unused, MVP-2 wires this). */
esp_err_t doudou_bridge_reply(const char *question_id, const char *choice_id);

#ifdef __cplusplus
}
#endif
