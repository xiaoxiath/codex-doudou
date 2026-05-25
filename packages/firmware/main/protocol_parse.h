/**
 * protocol_parse.h — Doudou wire-protocol v1 parser/builder.
 *
 * Pure C: only depends on cJSON. No ESP-IDF symbols. The firmware
 * (`bridge_client.c`) and the host test harness both link against this
 * module so the protocol has a single source of truth.
 *
 * String fields in parsed structs point INTO the supplied cJSON tree;
 * the caller must keep the tree alive while reading.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- shared types (also used by bridge_client.h) ---------- */

typedef struct {
    /* "idle" | "thinking" | "executing" | "waiting_input" |
     * "waiting_approval" | "done" | "error" */
    const char *state;
    const char *title;
    const char *body;   /* may be NULL */
} doudou_status_t;

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
    int agents_md;            /* -1 unknown, 0 false, 1 true */
} doudou_session_info_t;

typedef struct {
    const char *id;
    const char *label;
    const char *group_label;
    int used_pct;             /* -1 unknown */
    int window_minutes;       /* -1 unknown */
    int64_t resets_at_ms;     /* 0 unknown */
} doudou_limit_t;

typedef struct {
    bool has_session;
    int64_t input_tokens, output_tokens, cached_tokens, total_tokens;
    int64_t current_context_tokens, model_context_window;
    const char *plan_type;
    doudou_limit_t limits[8];
    int n_limits;
} doudou_usage_t;

typedef struct {
    const char *id;
    const char *title;
    const char *source;
    bool active;
    int64_t updated_at_ms;
} doudou_thread_t;

typedef struct {
    const char *id;
    const char *label;
} doudou_choice_t;

#define DOUDOU_MAX_CHOICES 4

/* "low" | "medium" | "high" */
typedef struct {
    const char *id;
    const char *risk;
    const char *action_type;          /* "run_command" | "modify_file" | ... */
    const char *title;
    const char *body;                 /* may be NULL */
    doudou_choice_t choices[DOUDOU_MAX_CHOICES];
    int n_choices;
    int64_t expires_at_ms;
    bool require_confirm;
    int queue_total;                  /* 0 = unspecified */
} doudou_question_t;

/* ---------- generic JSON helpers ---------- */

const char *doudou_json_str(const cJSON *o, const char *k);
int64_t     doudou_json_i64(const cJSON *o, const char *k, int64_t def);
int         doudou_json_bool_tri(const cJSON *o, const char *k);  /* -1/0/1 */

/* ---------- inbound parsers ---------- */

/** Parse welcome envelope. Returns true if root has type=="welcome". */
bool doudou_parse_welcome(const cJSON *root,
                          uint64_t *server_time_ms,
                          const char **session_id);

/** Parse status into *out. Returns true if root has type=="status". */
bool doudou_parse_status(const cJSON *root, doudou_status_t *out);

bool doudou_parse_session_info(const cJSON *root, doudou_session_info_t *out);
bool doudou_parse_usage(const cJSON *root, doudou_usage_t *out);

/** Parse thread_list into *out (up to max_n). Returns count parsed,
 *  or -1 if the message is not a thread_list. */
int  doudou_parse_thread_list(const cJSON *root,
                              doudou_thread_t *out, int max_n);

bool doudou_parse_error(const cJSON *root,
                        const char **code,
                        const char **title,
                        const char **body);

/** Parse question into *out (string fields point into root). Returns
 *  true on success. choices array is filled up to DOUDOU_MAX_CHOICES. */
bool doudou_parse_question(const cJSON *root, doudou_question_t *out);

/* ---------- outbound builders ----------
 * Caller owns the returned object and must cJSON_Delete() it. */

cJSON *doudou_build_envelope(const char *type, uint32_t seq, uint32_t ts_ms);
cJSON *doudou_build_hello(uint32_t seq, uint32_t ts_ms,
                          const char *device_id,
                          const char *fw_version,
                          const char *pairing_token);
cJSON *doudou_build_pong(uint32_t seq, uint32_t ts_ms, uint32_t pong_for_seq);
cJSON *doudou_build_follow_thread(uint32_t seq, uint32_t ts_ms,
                                  const char *thread_id);
cJSON *doudou_build_reply(uint32_t seq, uint32_t ts_ms,
                          const char *device_id,
                          const char *question_id,
                          const char *choice_id);

#ifdef __cplusplus
}
#endif
