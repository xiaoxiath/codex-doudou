/**
 * protocol_parse.c — see header. IDF-free; host-testable.
 */
#include "protocol_parse.h"

#include <stdlib.h>
#include <string.h>

#define PROTO_VERSION 1

/* ---------- generic helpers ---------- */

const char *doudou_json_str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v)) return v->valuestring;
    return NULL;
}

int64_t doudou_json_i64(const cJSON *o, const char *k, int64_t def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return (int64_t)v->valuedouble;
    return def;
}

int doudou_json_bool_tri(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;
    return -1;
}

static bool type_is(const cJSON *root, const char *expected)
{
    const char *t = doudou_json_str(root, "type");
    return t && !strcmp(t, expected);
}

/* ---------- inbound parsers ---------- */

bool doudou_parse_welcome(const cJSON *root,
                          uint64_t *server_time_ms,
                          const char **session_id)
{
    if (!root || !type_is(root, "welcome")) return false;
    if (server_time_ms) *server_time_ms =
        (uint64_t)doudou_json_i64(root, "server_time_ms", 0);
    if (session_id) *session_id = doudou_json_str(root, "session_id");
    return true;
}

bool doudou_parse_status(const cJSON *root, doudou_status_t *out)
{
    if (!root || !out || !type_is(root, "status")) return false;
    out->state = doudou_json_str(root, "state");
    out->title = doudou_json_str(root, "title");
    out->body  = doudou_json_str(root, "body");
    return true;
}

bool doudou_parse_session_info(const cJSON *root, doudou_session_info_t *out)
{
    if (!root || !out || !type_is(root, "session_info")) return false;
    out->session_id         = doudou_json_str(root, "session_id");
    out->thread_title       = doudou_json_str(root, "thread_title");
    out->source             = doudou_json_str(root, "source");
    out->model              = doudou_json_str(root, "model");
    out->reasoning_effort   = doudou_json_str(root, "reasoning_effort");
    out->summary_mode       = doudou_json_str(root, "summary_mode");
    out->cwd                = doudou_json_str(root, "cwd");
    out->permissions        = doudou_json_str(root, "permissions");
    out->collaboration_mode = doudou_json_str(root, "collaboration_mode");
    out->account_email      = doudou_json_str(root, "account_email");
    out->plan_type          = doudou_json_str(root, "plan_type");
    out->agents_md          = doudou_json_bool_tri(root, "agents_md");
    return true;
}

bool doudou_parse_usage(const cJSON *root, doudou_usage_t *out)
{
    if (!root || !out || !type_is(root, "usage")) return false;
    memset(out, 0, sizeof(*out));

    const cJSON *sess = cJSON_GetObjectItemCaseSensitive(root, "session");
    if (cJSON_IsObject(sess)) {
        out->has_session             = true;
        out->input_tokens            = doudou_json_i64(sess, "input_tokens", 0);
        out->output_tokens           = doudou_json_i64(sess, "output_tokens", 0);
        out->cached_tokens           = doudou_json_i64(sess, "cached_tokens", 0);
        out->total_tokens            = doudou_json_i64(sess, "total_tokens", 0);
        out->current_context_tokens  = doudou_json_i64(sess, "current_context_tokens", 0);
        out->model_context_window    = doudou_json_i64(sess, "model_context_window", 0);
    }
    out->plan_type = doudou_json_str(root, "plan_type");

    const cJSON *limits = cJSON_GetObjectItemCaseSensitive(root, "limits");
    if (cJSON_IsArray(limits)) {
        int i = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, limits) {
            if (i >= (int)(sizeof(out->limits) / sizeof(out->limits[0]))) break;
            out->limits[i].id              = doudou_json_str(it, "id");
            out->limits[i].label           = doudou_json_str(it, "label");
            out->limits[i].group_label     = doudou_json_str(it, "group_label");
            out->limits[i].used_pct        = (int)doudou_json_i64(it, "used_pct", -1);
            out->limits[i].window_minutes  = (int)doudou_json_i64(it, "window_minutes", -1);
            out->limits[i].resets_at_ms    = doudou_json_i64(it, "resets_at", 0);
            i++;
        }
        out->n_limits = i;
    }
    return true;
}

int doudou_parse_thread_list(const cJSON *root,
                             doudou_thread_t *out, int max_n)
{
    if (!root || !out || max_n <= 0) return -1;
    if (!type_is(root, "thread_list")) return -1;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "threads");
    if (!cJSON_IsArray(arr)) return -1;
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= max_n) break;
        out[n].id            = doudou_json_str(it, "id");
        out[n].title         = doudou_json_str(it, "title");
        out[n].source        = doudou_json_str(it, "source");
        out[n].active        = doudou_json_bool_tri(it, "active") == 1;
        out[n].updated_at_ms = doudou_json_i64(it, "updated_at", 0);
        n++;
    }
    return n;
}

bool doudou_parse_error(const cJSON *root,
                        const char **code,
                        const char **title,
                        const char **body)
{
    if (!root || !type_is(root, "error")) return false;
    if (code)  *code  = doudou_json_str(root, "code");
    if (title) *title = doudou_json_str(root, "title");
    if (body)  *body  = doudou_json_str(root, "body");
    return true;
}

bool doudou_parse_question(const cJSON *root, doudou_question_t *out)
{
    if (!root || !out || !type_is(root, "question")) return false;
    memset(out, 0, sizeof(*out));
    out->id              = doudou_json_str(root, "id");
    out->risk            = doudou_json_str(root, "risk");
    out->action_type     = doudou_json_str(root, "action_type");
    out->title           = doudou_json_str(root, "title");
    out->body            = doudou_json_str(root, "body");
    out->expires_at_ms   = doudou_json_i64(root, "expires_at", 0);
    out->require_confirm = doudou_json_bool_tri(root, "require_confirm") == 1;
    out->queue_total     = (int)doudou_json_i64(root, "queue_total", 0);

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(arr)) return out->id != NULL;   /* title-only msg still parseable */
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= DOUDOU_MAX_CHOICES) break;
        out->choices[n].id    = doudou_json_str(it, "id");
        out->choices[n].label = doudou_json_str(it, "label");
        n++;
    }
    out->n_choices = n;
    return true;
}

/* ---------- outbound builders ---------- */

cJSON *doudou_build_envelope(const char *type, uint32_t seq, uint32_t ts_ms)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "v", PROTO_VERSION);
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddNumberToObject(o, "seq", (double)seq);
    cJSON_AddNumberToObject(o, "ts",  (double)ts_ms);
    return o;
}

cJSON *doudou_build_hello(uint32_t seq, uint32_t ts_ms,
                          const char *device_id,
                          const char *fw_version,
                          const char *pairing_token)
{
    cJSON *o = doudou_build_envelope("hello", seq, ts_ms);
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "device_id",     device_id ? device_id : "");
    cJSON_AddStringToObject(o, "fw_version",    fw_version ? fw_version : "");
    cJSON_AddStringToObject(o, "pairing_token", pairing_token ? pairing_token : "");
    return o;
}

cJSON *doudou_build_pong(uint32_t seq, uint32_t ts_ms, uint32_t pong_for_seq)
{
    cJSON *o = doudou_build_envelope("pong", seq, ts_ms);
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "pong_for_seq", (double)pong_for_seq);
    return o;
}

cJSON *doudou_build_follow_thread(uint32_t seq, uint32_t ts_ms,
                                  const char *thread_id)
{
    if (!thread_id || !*thread_id) return NULL;
    cJSON *o = doudou_build_envelope("follow_thread", seq, ts_ms);
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "thread_id", thread_id);
    return o;
}

cJSON *doudou_build_reply(uint32_t seq, uint32_t ts_ms,
                          const char *device_id,
                          const char *question_id,
                          const char *choice_id)
{
    if (!question_id || !choice_id) return NULL;
    cJSON *o = doudou_build_envelope("reply", seq, ts_ms);
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "id",        question_id);
    cJSON_AddStringToObject(o, "choice_id", choice_id);
    cJSON_AddStringToObject(o, "device_id", device_id ? device_id : "");
    return o;
}
