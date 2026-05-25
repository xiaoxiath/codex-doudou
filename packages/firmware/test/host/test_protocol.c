/**
 * test_protocol.c — host-native unit tests for protocol_parse.{c,h}.
 *
 * Built and run by `make test` in this directory. Linked against
 * vendored cJSON (downloaded by the Makefile on first run) and the
 * IDF-free protocol_parse.c from the firmware sources.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cJSON.h"
#include "protocol_parse.h"

/* ---------- tiny assertion harness ---------- */

static int g_pass = 0, g_fail = 0;
static const char *g_curr = "(unknown)";

#define FAIL_HEADER() \
    fprintf(stderr, "  \xE2\x9C\x97 %s:%d  ", g_curr, __LINE__)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { FAIL_HEADER(); fprintf(stderr, "expected TRUE: %s\n", #cond); g_fail++; return; } \
} while(0)

#define ASSERT_FALSE(cond) do { \
    if (cond) { FAIL_HEADER(); fprintf(stderr, "expected FALSE: %s\n", #cond); g_fail++; return; } \
} while(0)

#define ASSERT_EQ_INT(actual, expected) do { \
    long long _a = (long long)(actual), _e = (long long)(expected); \
    if (_a != _e) { FAIL_HEADER(); fprintf(stderr, "expected %lld got %lld (%s)\n", _e, _a, #actual); g_fail++; return; } \
} while(0)

#define ASSERT_EQ_STR(actual, expected) do { \
    const char *_a = (actual), *_e = (expected); \
    if ((_a == NULL) != (_e == NULL) || (_a && _e && strcmp(_a, _e) != 0)) { \
        FAIL_HEADER(); fprintf(stderr, "expected \"%s\" got \"%s\" (%s)\n", _e ? _e : "(null)", _a ? _a : "(null)", #actual); g_fail++; return; \
    } \
} while(0)

#define ASSERT_NULL(p)     ASSERT_TRUE((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)

#define TEST(name) static void name(void)
#define RUN(name) do { \
    g_curr = #name; \
    int _before = g_fail; \
    name(); \
    if (g_fail == _before) { printf("  \xE2\x9C\x93 %s\n", #name); g_pass++; } \
} while(0)

/* ---------- json helpers ---------- */

TEST(json_str_present) {
    cJSON *o = cJSON_Parse("{\"k\":\"hello\"}");
    ASSERT_EQ_STR(doudou_json_str(o, "k"), "hello");
    cJSON_Delete(o);
}

TEST(json_str_missing_returns_null) {
    cJSON *o = cJSON_Parse("{\"k\":\"hello\"}");
    ASSERT_NULL(doudou_json_str(o, "z"));
    cJSON_Delete(o);
}

TEST(json_str_wrong_type_returns_null) {
    cJSON *o = cJSON_Parse("{\"k\":42}");
    ASSERT_NULL(doudou_json_str(o, "k"));
    cJSON_Delete(o);
}

TEST(json_i64_present) {
    cJSON *o = cJSON_Parse("{\"k\":42}");
    ASSERT_EQ_INT(doudou_json_i64(o, "k", -1), 42);
    cJSON_Delete(o);
}

TEST(json_i64_missing_returns_default) {
    cJSON *o = cJSON_Parse("{\"k\":42}");
    ASSERT_EQ_INT(doudou_json_i64(o, "z", 99), 99);
    cJSON_Delete(o);
}

TEST(json_i64_handles_large_number) {
    cJSON *o = cJSON_Parse("{\"k\":1700000000000}");
    ASSERT_EQ_INT(doudou_json_i64(o, "k", 0), 1700000000000LL);
    cJSON_Delete(o);
}

TEST(json_bool_tri_true)   { cJSON *o = cJSON_Parse("{\"k\":true}");  ASSERT_EQ_INT(doudou_json_bool_tri(o, "k"),  1); cJSON_Delete(o); }
TEST(json_bool_tri_false)  { cJSON *o = cJSON_Parse("{\"k\":false}"); ASSERT_EQ_INT(doudou_json_bool_tri(o, "k"),  0); cJSON_Delete(o); }
TEST(json_bool_tri_missing){ cJSON *o = cJSON_Parse("{}");            ASSERT_EQ_INT(doudou_json_bool_tri(o, "k"), -1); cJSON_Delete(o); }
TEST(json_bool_tri_null_value) { cJSON *o = cJSON_Parse("{\"k\":null}"); ASSERT_EQ_INT(doudou_json_bool_tri(o, "k"), -1); cJSON_Delete(o); }

/* ---------- welcome ---------- */

TEST(parse_welcome_full) {
    cJSON *o = cJSON_Parse(
        "{\"v\":1,\"type\":\"welcome\",\"seq\":1,\"ts\":1234,"
        "\"server_time_ms\":1700000000000,\"session_id\":\"abc-123\"}");
    uint64_t srv = 0; const char *sid = NULL;
    ASSERT_TRUE(doudou_parse_welcome(o, &srv, &sid));
    ASSERT_EQ_INT(srv, 1700000000000LL);
    ASSERT_EQ_STR(sid, "abc-123");
    cJSON_Delete(o);
}

TEST(parse_welcome_wrong_type_rejected) {
    cJSON *o = cJSON_Parse("{\"type\":\"status\",\"server_time_ms\":1}");
    ASSERT_FALSE(doudou_parse_welcome(o, NULL, NULL));
    cJSON_Delete(o);
}

TEST(parse_welcome_no_session_id_returns_null) {
    cJSON *o = cJSON_Parse("{\"type\":\"welcome\",\"server_time_ms\":42}");
    uint64_t srv = 0; const char *sid = (const char *)0xdeadbeef;
    ASSERT_TRUE(doudou_parse_welcome(o, &srv, &sid));
    ASSERT_EQ_INT(srv, 42);
    ASSERT_NULL(sid);
    cJSON_Delete(o);
}

TEST(parse_welcome_null_outparams_ok) {
    cJSON *o = cJSON_Parse("{\"type\":\"welcome\",\"server_time_ms\":7}");
    ASSERT_TRUE(doudou_parse_welcome(o, NULL, NULL));
    cJSON_Delete(o);
}

/* ---------- status ---------- */

TEST(parse_status_full) {
    cJSON *o = cJSON_Parse(
        "{\"type\":\"status\",\"state\":\"thinking\","
        "\"title\":\"分析代码\",\"body\":\"扫描 packages/bridge\"}");
    doudou_status_t s = {0};
    ASSERT_TRUE(doudou_parse_status(o, &s));
    ASSERT_EQ_STR(s.state, "thinking");
    ASSERT_EQ_STR(s.title, "分析代码");
    ASSERT_EQ_STR(s.body,  "扫描 packages/bridge");
    cJSON_Delete(o);
}

TEST(parse_status_missing_body_ok) {
    cJSON *o = cJSON_Parse("{\"type\":\"status\",\"state\":\"idle\",\"title\":\"等待\"}");
    doudou_status_t s = {0};
    ASSERT_TRUE(doudou_parse_status(o, &s));
    ASSERT_EQ_STR(s.state, "idle");
    ASSERT_EQ_STR(s.title, "等待");
    ASSERT_NULL(s.body);
    cJSON_Delete(o);
}

TEST(parse_status_wrong_type_rejected) {
    cJSON *o = cJSON_Parse("{\"type\":\"thread_list\",\"state\":\"idle\"}");
    doudou_status_t s = {0};
    ASSERT_FALSE(doudou_parse_status(o, &s));
    cJSON_Delete(o);
}

/* ---------- session_info ---------- */

TEST(parse_session_info_full) {
    cJSON *o = cJSON_Parse(
        "{\"type\":\"session_info\","
        "\"session_id\":\"sid-1\",\"thread_title\":\"开发豆豆\","
        "\"source\":\"follow\",\"model\":\"gpt-5\","
        "\"reasoning_effort\":\"high\",\"summary_mode\":\"detailed\","
        "\"cwd\":\"/u/proj\",\"permissions\":\"on-request\","
        "\"collaboration_mode\":\"agent\","
        "\"account_email\":\"u@x.com\",\"plan_type\":\"plus\","
        "\"agents_md\":true}");
    doudou_session_info_t si = {0};
    ASSERT_TRUE(doudou_parse_session_info(o, &si));
    ASSERT_EQ_STR(si.session_id, "sid-1");
    ASSERT_EQ_STR(si.thread_title, "开发豆豆");
    ASSERT_EQ_STR(si.source, "follow");
    ASSERT_EQ_STR(si.model, "gpt-5");
    ASSERT_EQ_STR(si.reasoning_effort, "high");
    ASSERT_EQ_STR(si.summary_mode, "detailed");
    ASSERT_EQ_STR(si.cwd, "/u/proj");
    ASSERT_EQ_STR(si.permissions, "on-request");
    ASSERT_EQ_STR(si.collaboration_mode, "agent");
    ASSERT_EQ_STR(si.account_email, "u@x.com");
    ASSERT_EQ_STR(si.plan_type, "plus");
    ASSERT_EQ_INT(si.agents_md, 1);
    cJSON_Delete(o);
}

TEST(parse_session_info_agents_md_tristate) {
    cJSON *a = cJSON_Parse("{\"type\":\"session_info\",\"agents_md\":false}");
    doudou_session_info_t si = {0};
    ASSERT_TRUE(doudou_parse_session_info(a, &si));
    ASSERT_EQ_INT(si.agents_md, 0);
    cJSON_Delete(a);

    cJSON *b = cJSON_Parse("{\"type\":\"session_info\"}");
    doudou_session_info_t si2 = {0};
    ASSERT_TRUE(doudou_parse_session_info(b, &si2));
    ASSERT_EQ_INT(si2.agents_md, -1);
    cJSON_Delete(b);
}

/* ---------- usage ---------- */

TEST(parse_usage_full) {
    cJSON *o = cJSON_Parse(
        "{\"type\":\"usage\",\"plan_type\":\"plus\","
        "\"session\":{\"input_tokens\":100,\"output_tokens\":200,"
                     "\"cached_tokens\":50,\"total_tokens\":300,"
                     "\"current_context_tokens\":500,"
                     "\"model_context_window\":200000},"
        "\"limits\":["
          "{\"id\":\"weekly_plus\",\"label\":\"周配额\","
           "\"group_label\":null,\"used_pct\":42,"
           "\"window_minutes\":10080,\"resets_at\":1700000000},"
          "{\"id\":\"hourly\",\"label\":\"小时\",\"used_pct\":8,"
           "\"window_minutes\":60,\"resets_at\":1700001000}"
        "]}");
    doudou_usage_t u = {0};
    ASSERT_TRUE(doudou_parse_usage(o, &u));
    ASSERT_TRUE(u.has_session);
    ASSERT_EQ_INT(u.input_tokens,          100);
    ASSERT_EQ_INT(u.output_tokens,         200);
    ASSERT_EQ_INT(u.cached_tokens,         50);
    ASSERT_EQ_INT(u.total_tokens,          300);
    ASSERT_EQ_INT(u.current_context_tokens,500);
    ASSERT_EQ_INT(u.model_context_window,  200000);
    ASSERT_EQ_STR(u.plan_type, "plus");
    ASSERT_EQ_INT(u.n_limits, 2);
    ASSERT_EQ_STR(u.limits[0].id, "weekly_plus");
    ASSERT_EQ_INT(u.limits[0].used_pct, 42);
    ASSERT_EQ_INT(u.limits[0].window_minutes, 10080);
    ASSERT_EQ_INT(u.limits[0].resets_at_ms, 1700000000);
    ASSERT_EQ_INT(u.limits[1].used_pct, 8);
    cJSON_Delete(o);
}

TEST(parse_usage_no_session_no_limits) {
    cJSON *o = cJSON_Parse("{\"type\":\"usage\"}");
    doudou_usage_t u = {0};
    ASSERT_TRUE(doudou_parse_usage(o, &u));
    ASSERT_FALSE(u.has_session);
    ASSERT_EQ_INT(u.n_limits, 0);
    cJSON_Delete(o);
}

TEST(parse_usage_caps_limits_at_8) {
    /* 10 limits in the JSON; struct holds max 8 — must not overflow. */
    char buf[2048];
    int p = 0;
    p += snprintf(buf+p, sizeof(buf)-p, "{\"type\":\"usage\",\"limits\":[");
    for (int i = 0; i < 10; i++) {
        p += snprintf(buf+p, sizeof(buf)-p,
                      "%s{\"id\":\"l%d\",\"used_pct\":%d}",
                      i ? "," : "", i, i*10);
    }
    snprintf(buf+p, sizeof(buf)-p, "]}");
    cJSON *o = cJSON_Parse(buf);
    doudou_usage_t u = {0};
    ASSERT_TRUE(doudou_parse_usage(o, &u));
    ASSERT_EQ_INT(u.n_limits, 8);
    ASSERT_EQ_STR(u.limits[0].id, "l0");
    ASSERT_EQ_STR(u.limits[7].id, "l7");
    cJSON_Delete(o);
}

TEST(parse_usage_wrong_type_rejected) {
    cJSON *o = cJSON_Parse("{\"type\":\"status\"}");
    doudou_usage_t u = {0};
    ASSERT_FALSE(doudou_parse_usage(o, &u));
    cJSON_Delete(o);
}

/* ---------- thread_list ---------- */

TEST(parse_thread_list_basic) {
    cJSON *o = cJSON_Parse(
        "{\"type\":\"thread_list\",\"threads\":["
          "{\"id\":\"t1\",\"title\":\"Codex 配置\",\"source\":\"follow\","
           "\"active\":true,\"updated_at\":111},"
          "{\"id\":\"t2\",\"title\":\"修 bug\",\"source\":\"own\","
           "\"active\":false,\"updated_at\":222}"
        "]}");
    doudou_thread_t out[4];
    int n = doudou_parse_thread_list(o, out, 4);
    ASSERT_EQ_INT(n, 2);
    ASSERT_EQ_STR(out[0].id, "t1");
    ASSERT_EQ_STR(out[0].title, "Codex 配置");
    ASSERT_EQ_STR(out[0].source, "follow");
    ASSERT_TRUE(out[0].active);
    ASSERT_EQ_INT(out[0].updated_at_ms, 111);
    ASSERT_EQ_STR(out[1].id, "t2");
    ASSERT_FALSE(out[1].active);
    cJSON_Delete(o);
}

TEST(parse_thread_list_respects_max) {
    cJSON *o = cJSON_Parse(
        "{\"type\":\"thread_list\",\"threads\":["
        "{\"id\":\"a\"},{\"id\":\"b\"},{\"id\":\"c\"},"
        "{\"id\":\"d\"},{\"id\":\"e\"}"
        "]}");
    doudou_thread_t out[3];
    int n = doudou_parse_thread_list(o, out, 3);
    ASSERT_EQ_INT(n, 3);
    ASSERT_EQ_STR(out[2].id, "c");
    cJSON_Delete(o);
}

TEST(parse_thread_list_empty_array) {
    cJSON *o = cJSON_Parse("{\"type\":\"thread_list\",\"threads\":[]}");
    doudou_thread_t out[4];
    int n = doudou_parse_thread_list(o, out, 4);
    ASSERT_EQ_INT(n, 0);
    cJSON_Delete(o);
}

TEST(parse_thread_list_wrong_type_returns_neg1) {
    cJSON *o = cJSON_Parse("{\"type\":\"status\"}");
    doudou_thread_t out[2];
    ASSERT_EQ_INT(doudou_parse_thread_list(o, out, 2), -1);
    cJSON_Delete(o);
}

TEST(parse_thread_list_missing_array_returns_neg1) {
    cJSON *o = cJSON_Parse("{\"type\":\"thread_list\"}");
    doudou_thread_t out[2];
    ASSERT_EQ_INT(doudou_parse_thread_list(o, out, 2), -1);
    cJSON_Delete(o);
}

/* ---------- question ---------- */

TEST(parse_question_full) {
    cJSON *o = cJSON_Parse(
        "{\"type\":\"question\",\"id\":\"q-1\","
        "\"risk\":\"high\",\"action_type\":\"run_command\","
        "\"title\":\"删除 /tmp\",\"body\":\"rm -rf /tmp 是否执行?\","
        "\"choices\":[{\"id\":\"accept\",\"label\":\"允许\"},"
                     "{\"id\":\"reject\",\"label\":\"拒绝\"}],"
        "\"expires_at\":1700000060000,\"require_confirm\":true,"
        "\"queue_total\":3}");
    doudou_question_t q = {0};
    ASSERT_TRUE(doudou_parse_question(o, &q));
    ASSERT_EQ_STR(q.id,          "q-1");
    ASSERT_EQ_STR(q.risk,        "high");
    ASSERT_EQ_STR(q.action_type, "run_command");
    ASSERT_EQ_STR(q.title,       "删除 /tmp");
    ASSERT_EQ_STR(q.body,        "rm -rf /tmp 是否执行?");
    ASSERT_EQ_INT(q.expires_at_ms, 1700000060000LL);
    ASSERT_TRUE(q.require_confirm);
    ASSERT_EQ_INT(q.queue_total, 3);
    ASSERT_EQ_INT(q.n_choices, 2);
    ASSERT_EQ_STR(q.choices[0].id,    "accept");
    ASSERT_EQ_STR(q.choices[0].label, "允许");
    ASSERT_EQ_STR(q.choices[1].id,    "reject");
    cJSON_Delete(o);
}

TEST(parse_question_caps_choices_at_4) {
    cJSON *o = cJSON_Parse(
        "{\"type\":\"question\",\"id\":\"q\",\"risk\":\"low\","
        "\"action_type\":\"user_input\",\"title\":\"x\","
        "\"expires_at\":1,\"choices\":["
        "{\"id\":\"a\"},{\"id\":\"b\"},{\"id\":\"c\"},"
        "{\"id\":\"d\"},{\"id\":\"e\"}]}");
    doudou_question_t q = {0};
    ASSERT_TRUE(doudou_parse_question(o, &q));
    ASSERT_EQ_INT(q.n_choices, 4);   /* DOUDOU_MAX_CHOICES */
    ASSERT_EQ_STR(q.choices[3].id, "d");
    cJSON_Delete(o);
}

TEST(parse_question_wrong_type_rejected) {
    cJSON *o = cJSON_Parse("{\"type\":\"status\",\"id\":\"x\"}");
    doudou_question_t q = {0};
    ASSERT_FALSE(doudou_parse_question(o, &q));
    cJSON_Delete(o);
}

TEST(parse_question_require_confirm_defaults_false) {
    cJSON *o = cJSON_Parse(
        "{\"type\":\"question\",\"id\":\"q\",\"risk\":\"low\","
        "\"action_type\":\"tool_call\",\"title\":\"x\","
        "\"choices\":[{\"id\":\"ok\",\"label\":\"OK\"}],"
        "\"expires_at\":1}");
    doudou_question_t q = {0};
    ASSERT_TRUE(doudou_parse_question(o, &q));
    ASSERT_FALSE(q.require_confirm);
    ASSERT_EQ_INT(q.queue_total, 0);
    cJSON_Delete(o);
}

/* ---------- error ---------- */

TEST(parse_error_full) {
    cJSON *o = cJSON_Parse(
        "{\"type\":\"error\",\"code\":\"bridge_unreachable\","
        "\"title\":\"网络异常\",\"body\":\"无法连接到 Bridge\"}");
    const char *code = NULL, *title = NULL, *body = NULL;
    ASSERT_TRUE(doudou_parse_error(o, &code, &title, &body));
    ASSERT_EQ_STR(code,  "bridge_unreachable");
    ASSERT_EQ_STR(title, "网络异常");
    ASSERT_EQ_STR(body,  "无法连接到 Bridge");
    cJSON_Delete(o);
}

TEST(parse_error_wrong_type_rejected) {
    cJSON *o = cJSON_Parse("{\"type\":\"status\"}");
    ASSERT_FALSE(doudou_parse_error(o, NULL, NULL, NULL));
    cJSON_Delete(o);
}

/* ---------- builders ---------- */

TEST(build_envelope_v1_and_seq_ts) {
    cJSON *o = doudou_build_envelope("ping", 42, 1234);
    ASSERT_NOT_NULL(o);
    char *s = cJSON_PrintUnformatted(o);
    cJSON *p = cJSON_Parse(s);
    ASSERT_EQ_INT(doudou_json_i64(p, "v", 0),   1);
    ASSERT_EQ_STR(doudou_json_str(p, "type"),   "ping");
    ASSERT_EQ_INT(doudou_json_i64(p, "seq", 0), 42);
    ASSERT_EQ_INT(doudou_json_i64(p, "ts", 0),  1234);
    cJSON_Delete(p); free(s); cJSON_Delete(o);
}

TEST(build_hello_fields) {
    cJSON *o = doudou_build_hello(1, 100, "doudou-1", "0.1.0", "tok-secret");
    char *s = cJSON_PrintUnformatted(o);
    cJSON *p = cJSON_Parse(s);
    ASSERT_EQ_STR(doudou_json_str(p, "type"),          "hello");
    ASSERT_EQ_STR(doudou_json_str(p, "device_id"),     "doudou-1");
    ASSERT_EQ_STR(doudou_json_str(p, "fw_version"),    "0.1.0");
    ASSERT_EQ_STR(doudou_json_str(p, "pairing_token"), "tok-secret");
    cJSON_Delete(p); free(s); cJSON_Delete(o);
}

TEST(build_hello_null_args_become_empty) {
    cJSON *o = doudou_build_hello(1, 100, NULL, NULL, NULL);
    char *s = cJSON_PrintUnformatted(o);
    cJSON *p = cJSON_Parse(s);
    ASSERT_EQ_STR(doudou_json_str(p, "device_id"),     "");
    ASSERT_EQ_STR(doudou_json_str(p, "fw_version"),    "");
    ASSERT_EQ_STR(doudou_json_str(p, "pairing_token"), "");
    cJSON_Delete(p); free(s); cJSON_Delete(o);
}

TEST(build_pong_fields) {
    cJSON *o = doudou_build_pong(2, 100, 7);
    char *s = cJSON_PrintUnformatted(o);
    cJSON *p = cJSON_Parse(s);
    ASSERT_EQ_STR(doudou_json_str(p, "type"), "pong");
    ASSERT_EQ_INT(doudou_json_i64(p, "pong_for_seq", -1), 7);
    cJSON_Delete(p); free(s); cJSON_Delete(o);
}

TEST(build_follow_thread_rejects_empty_or_null) {
    ASSERT_NULL(doudou_build_follow_thread(1, 100, NULL));
    ASSERT_NULL(doudou_build_follow_thread(1, 100, ""));
}

TEST(build_follow_thread_normal) {
    cJSON *o = doudou_build_follow_thread(3, 100, "t-abc");
    char *s = cJSON_PrintUnformatted(o);
    cJSON *p = cJSON_Parse(s);
    ASSERT_EQ_STR(doudou_json_str(p, "type"),      "follow_thread");
    ASSERT_EQ_STR(doudou_json_str(p, "thread_id"), "t-abc");
    cJSON_Delete(p); free(s); cJSON_Delete(o);
}

TEST(build_reply_rejects_partial) {
    ASSERT_NULL(doudou_build_reply(1, 100, "did", NULL, "c-1"));
    ASSERT_NULL(doudou_build_reply(1, 100, "did", "q-1", NULL));
    ASSERT_NULL(doudou_build_reply(1, 100, "did", NULL, NULL));
}

TEST(build_reply_fields) {
    cJSON *o = doudou_build_reply(4, 100, "doudou-1", "q-1", "c-yes");
    char *s = cJSON_PrintUnformatted(o);
    cJSON *p = cJSON_Parse(s);
    ASSERT_EQ_STR(doudou_json_str(p, "type"),      "reply");
    ASSERT_EQ_STR(doudou_json_str(p, "id"),        "q-1");
    ASSERT_EQ_STR(doudou_json_str(p, "choice_id"), "c-yes");
    ASSERT_EQ_STR(doudou_json_str(p, "device_id"), "doudou-1");
    cJSON_Delete(p); free(s); cJSON_Delete(o);
}

/* ---------- roundtrip ---------- */

TEST(roundtrip_status) {
    cJSON *built = doudou_build_envelope("status", 1, 100);
    cJSON_AddStringToObject(built, "state", "executing");
    cJSON_AddStringToObject(built, "title", "构建固件");

    char *s = cJSON_PrintUnformatted(built);
    cJSON *parsed = cJSON_Parse(s);
    doudou_status_t st = {0};
    ASSERT_TRUE(doudou_parse_status(parsed, &st));
    ASSERT_EQ_STR(st.state, "executing");
    ASSERT_EQ_STR(st.title, "构建固件");
    ASSERT_NULL(st.body);

    cJSON_Delete(parsed); free(s); cJSON_Delete(built);
}

int main(void)
{
    printf("Doudou protocol_parse host tests\n");

    /* json helpers */
    RUN(json_str_present);
    RUN(json_str_missing_returns_null);
    RUN(json_str_wrong_type_returns_null);
    RUN(json_i64_present);
    RUN(json_i64_missing_returns_default);
    RUN(json_i64_handles_large_number);
    RUN(json_bool_tri_true);
    RUN(json_bool_tri_false);
    RUN(json_bool_tri_missing);
    RUN(json_bool_tri_null_value);

    /* welcome */
    RUN(parse_welcome_full);
    RUN(parse_welcome_wrong_type_rejected);
    RUN(parse_welcome_no_session_id_returns_null);
    RUN(parse_welcome_null_outparams_ok);

    /* status */
    RUN(parse_status_full);
    RUN(parse_status_missing_body_ok);
    RUN(parse_status_wrong_type_rejected);

    /* session_info */
    RUN(parse_session_info_full);
    RUN(parse_session_info_agents_md_tristate);

    /* usage */
    RUN(parse_usage_full);
    RUN(parse_usage_no_session_no_limits);
    RUN(parse_usage_caps_limits_at_8);
    RUN(parse_usage_wrong_type_rejected);

    /* thread_list */
    RUN(parse_thread_list_basic);
    RUN(parse_thread_list_respects_max);
    RUN(parse_thread_list_empty_array);
    RUN(parse_thread_list_wrong_type_returns_neg1);
    RUN(parse_thread_list_missing_array_returns_neg1);

    /* question */
    RUN(parse_question_full);
    RUN(parse_question_caps_choices_at_4);
    RUN(parse_question_wrong_type_rejected);
    RUN(parse_question_require_confirm_defaults_false);

    /* error */
    RUN(parse_error_full);
    RUN(parse_error_wrong_type_rejected);

    /* builders */
    RUN(build_envelope_v1_and_seq_ts);
    RUN(build_hello_fields);
    RUN(build_hello_null_args_become_empty);
    RUN(build_pong_fields);
    RUN(build_follow_thread_rejects_empty_or_null);
    RUN(build_follow_thread_normal);
    RUN(build_reply_rejects_partial);
    RUN(build_reply_fields);

    /* roundtrip */
    RUN(roundtrip_status);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
