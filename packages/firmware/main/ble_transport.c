/**
 * ble_transport.c — BLE GATT replacement for bridge_client.c (WS).
 *
 * Compiled in place of bridge_client.c when CONFIG_DOUDOU_TRANSPORT_BLE
 * is selected. Implements the exact same public API
 * (`doudou_bridge_connect` / `doudou_bridge_follow_thread` /
 *  `doudou_bridge_reply`) so main.c stays transport-agnostic.
 *
 * Wire model:
 *
 *   Bridge (BLE central, noble.js)
 *     │
 *     │  scan + filter by name "doudou-XXXXXX" (last 6 of MAC)
 *     │  connect, discover service 0xDD00DDDD-... , subscribe to TX
 *     │
 *     ▼
 *   ble_transport.c  (GATT peripheral on the ESP32-C3)
 *     ├─ TX char (notify):  outbound JSON, fragmented to MTU-3-2
 *     ├─ RX char (write):   inbound JSON, reassembled across fragments
 *     └─ CTL char (read+notify):  status string "ready"/"busy"/"err"
 *
 *   Fragment header (2 bytes, prepended to each notify/write payload):
 *     byte 0 = seq        — wraps 0..255, sender keeps independent
 *                           sequences for tx and rx
 *     byte 1 = flags      — bit 0: more_fragments
 *                           bit 1: reset_assembly (drop receiver buffer)
 *
 * Outbound messages (hello/pong/reply/follow_thread) build envelopes via
 * the shared `protocol_parse.h` builders (the same module the WS path
 * uses), so the wire JSON is byte-identical between transports.
 *
 * Reassembly is on a single 16KB buffer (same as WS) — pessimistic but
 * matches the protocol's 1KB-per-message budget × a queue of 16.
 *
 * Known gaps (Phase 3 / 4 in the BLE migration plan):
 *   * No bonding / encryption yet — `just_works` pairing would slot in
 *     here. For now the assumption is LAN-equivalent trust.
 *   * No backpressure on notify — if Bridge stops draining we'll start
 *     dropping notifies. Bridge needs to subscribe + ACK before we send.
 *   * No real-hardware validation in CI; build green ≠ link-layer OK.
 */
#include "bridge_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "cJSON.h"
#include "protocol_parse.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

/* ---------- UUIDs ----------
 * Service: 0xDD00DDDD-0000-1000-8000-00805F9B34FB
 * TX char: 0xDD01DDDD-...
 * RX char: 0xDD02DDDD-...
 * CTL char: 0xDD03DDDD-...
 * NimBLE BLE_UUID128_INIT takes bytes in *little-endian* order.
 * All data characteristics require an encrypted link (BLE_GATT_CHR_F_*_ENC). */
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xdd, 0xdd, 0x00, 0xdd);
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xdd, 0xdd, 0x01, 0xdd);
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xdd, 0xdd, 0x02, 0xdd);
static const ble_uuid128_t CTL_UUID = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xdd, 0xdd, 0x03, 0xdd);

#define FRAG_HDR_LEN 2
#define FRAG_FLAG_MORE  0x01
#define FRAG_FLAG_RESET 0x02
#define RX_BUF_MAX     (16 * 1024)
#define BLE_NAME_BUFLEN 24

/* forward decls */
static esp_err_t ble_transport_send(const char *body);
static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---------- module state ---------- */
static doudou_bridge_handlers_t s_h = {0};
static SemaphoreHandle_t s_send_mutex = NULL;
static uint32_t s_out_seq = 1;
static int64_t s_start_us = 0;
static bool s_connected = false;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_attr_handle = 0;
static uint16_t s_ctl_attr_handle = 0;
static uint16_t s_mtu = 23;          /* default ATT MTU; updated on exchange */
static uint8_t  s_tx_seq = 0;
static char     s_dev_name[BLE_NAME_BUFLEN] = "doudou-?";

/* Reassembly buffer for inbound fragments. */
static uint8_t *s_rx_buf = NULL;
static size_t   s_rx_buf_cap = 0;
static size_t   s_rx_buf_len = 0;

/* ---------- helpers ---------- */
static uint32_t local_ts_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
}

static size_t chunk_payload_max(void)
{
    /* ATT MTU minus 3 bytes of ATT header minus our 2-byte fragment hdr.
     * NimBLE guards this for us in ble_gatts_notify_custom, but we still
     * compute it for the chunker. */
    if (s_mtu < 23) return 18;
    return (size_t)s_mtu - 3 - FRAG_HDR_LEN;
}

static void rx_reset(void)
{
    s_rx_buf_len = 0;
}

static int rx_append(const uint8_t *data, size_t n)
{
    if (s_rx_buf_len + n > RX_BUF_MAX) {
        ESP_LOGW(TAG, "rx buffer overflow, dropping");
        rx_reset();
        return -1;
    }
    if (s_rx_buf_cap < s_rx_buf_len + n + 1) {
        size_t new_cap = s_rx_buf_len + n + 1;
        uint8_t *nb = realloc(s_rx_buf, new_cap);
        if (!nb) return -1;
        s_rx_buf = nb;
        s_rx_buf_cap = new_cap;
    }
    memcpy(s_rx_buf + s_rx_buf_len, data, n);
    s_rx_buf_len += n;
    s_rx_buf[s_rx_buf_len] = '\0';
    return 0;
}

/* ---------- inbound JSON dispatch ----------
 * Identical control flow to bridge_client.c's parse_and_dispatch — when
 * a complete message arrives we delegate to the shared parsers in
 * protocol_parse.h. */
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
        /* Bridge ping → answer with pong, mirroring WS behaviour. */
        uint32_t seq = (uint32_t)doudou_json_i64(root, "seq", 0);
        cJSON *o = doudou_build_pong(s_out_seq++, local_ts_ms(), seq);
        if (o) {
            char *body = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            if (body) { ble_transport_send(body); free(body); }
        }
    } else if (!strcmp(type, "pong") || !strcmp(type, "ack")) {
        /* no-op */
    } else {
        ESP_LOGD(TAG, "ignored msg type=%s", type);
    }

    cJSON_Delete(root);
}

/* ---------- outbound: chunked notify ---------- */
static esp_err_t ble_transport_send(const char *body)
{
    if (!body) return ESP_ERR_INVALID_ARG;
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_send_mutex) xSemaphoreTake(s_send_mutex, portMAX_DELAY);

    size_t total = strlen(body);
    size_t off = 0;
    size_t chunk_max = chunk_payload_max();
    esp_err_t result = ESP_OK;

    while (off < total) {
        size_t this_chunk = total - off;
        if (this_chunk > chunk_max) this_chunk = chunk_max;
        bool more = (off + this_chunk) < total;

        /* Build [seq][flags][payload]. */
        uint8_t hdr[FRAG_HDR_LEN] = {
            s_tx_seq,
            (uint8_t)(more ? FRAG_FLAG_MORE : 0),
        };
        struct os_mbuf *om = ble_hs_mbuf_from_flat(hdr, FRAG_HDR_LEN);
        if (!om) { result = ESP_ERR_NO_MEM; break; }
        if (os_mbuf_append(om, body + off, this_chunk) != 0) {
            os_mbuf_free_chain(om);
            result = ESP_ERR_NO_MEM;
            break;
        }
        int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_attr_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify failed rc=%d", rc);
            result = ESP_FAIL;
            break;
        }
        off += this_chunk;
    }
    s_tx_seq++;

    if (s_send_mutex) xSemaphoreGive(s_send_mutex);
    return result;
}

/* ---------- inbound: characteristic write callback ---------- */
static int rx_char_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
    if (total < FRAG_HDR_LEN) return BLE_ATT_ERR_INVALID_PDU;

    /* Pull the 2-byte fragment header. */
    uint8_t hdr[FRAG_HDR_LEN];
    if (ble_hs_mbuf_to_flat(ctxt->om, hdr, FRAG_HDR_LEN, NULL) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;

    uint8_t flags = hdr[1];
    if (flags & FRAG_FLAG_RESET) rx_reset();

    /* Copy payload (everything past the 2-byte header). NimBLE doesn't
     * support reading at an offset, so flatten the whole frame then
     * slice. Single-fragment frames are bounded by MTU (≤ 247B). */
    uint16_t payload_len = total - FRAG_HDR_LEN;
    if (payload_len > 0) {
        uint8_t full[260];
        if (total > sizeof(full)) return BLE_ATT_ERR_INSUFFICIENT_RES;
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, full, total, &got) != 0)
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        if (got != total) return BLE_ATT_ERR_INVALID_PDU;
        if (rx_append(full + FRAG_HDR_LEN, payload_len) != 0)
            return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (!(flags & FRAG_FLAG_MORE)) {
        /* Last fragment — dispatch + reset. */
        parse_and_dispatch((const char *)s_rx_buf, s_rx_buf_len);
        rx_reset();
    }
    return 0;
}

/* ctl char accept reads/writes but they're informational only. */
static int ctl_char_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *st = s_connected ? "ready" : "idle";
        os_mbuf_append(ctxt->om, st, strlen(st));
        return 0;
    }
    return 0;
}

/* ---------- GATT service registration ----------
 *
 * All three data characteristics gate on an encrypted link — NimBLE will
 * automatically start LE Secure Connections pairing if a central tries
 * to read/write/subscribe without an established encrypted session. On
 * Just-Works the central + peripheral derive an LTK; we persist it in
 * NVS via CONFIG_BT_NIMBLE_NVS_PERSIST so subsequent connects only
 * encrypt (no re-pair). */
static const struct ble_gatt_chr_def s_chars[] = {
    {
        .uuid       = &TX_UUID.u,
        .access_cb  = NULL,
        .flags      = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
        .val_handle = &s_tx_attr_handle,
    },
    {
        .uuid       = &RX_UUID.u,
        .access_cb  = rx_char_access_cb,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
                    | BLE_GATT_CHR_F_WRITE_ENC,
    },
    {
        .uuid       = &CTL_UUID.u,
        .access_cb  = ctl_char_access_cb,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY
                    | BLE_GATT_CHR_F_READ_ENC,
        .val_handle = &s_ctl_attr_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &SVC_UUID.u,
        .characteristics = s_chars,
    },
    { 0 },
};

/* ---------- advertising ---------- */
static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags         = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name          = (const uint8_t *)s_dev_name;
    fields.name_len      = strlen(s_dev_name);
    fields.name_is_complete = 1;
    fields.uuids128      = (ble_uuid128_t *)&SVC_UUID;
    fields.num_uuids128  = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) { ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc); return; }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &params, gap_event_cb, NULL);
    if (rc) ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
    else    ESP_LOGI(TAG, "advertising as \"%s\"", s_dev_name);
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                s_connected   = true;
                s_start_us    = esp_timer_get_time();
                s_out_seq     = 1;
                s_tx_seq      = 0;
                rx_reset();
                ESP_LOGI(TAG, "central connected handle=%d", s_conn_handle);
                if (s_h.on_connected) s_h.on_connected();
                /* On connect, send hello via the same builder the WS path
                 * uses. */
                cJSON *o = doudou_build_hello(s_out_seq++, local_ts_ms(),
                                              CONFIG_DOUDOU_DEVICE_ID,
                                              "0.1.0-fw-ble",
                                              CONFIG_DOUDOU_PAIRING_TOKEN);
                if (o) {
                    char *body = cJSON_PrintUnformatted(o);
                    cJSON_Delete(o);
                    if (body) { ble_transport_send(body); free(body); }
                }
            } else {
                ESP_LOGW(TAG, "connect failed status=%d, re-adv", event->connect.status);
                start_advertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "central disconnected reason=%d",
                     event->disconnect.reason);
            s_connected   = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            rx_reset();
            if (s_h.on_disconnected) s_h.on_disconnected();
            start_advertising();
            break;
        case BLE_GAP_EVENT_MTU:
            s_mtu = event->mtu.value;
            ESP_LOGI(TAG, "mtu updated → %u (chunk payload max %u)",
                     s_mtu, (unsigned)chunk_payload_max());
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "central subscribed notify=%d",
                     event->subscribe.cur_notify);
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "encryption change status=%d",
                     event->enc_change.status);
            break;
        case BLE_GAP_EVENT_PASSKEY_ACTION:
            /* Just Works expects no user interaction; this event fires
             * only on Numeric Comparison / Passkey Entry / OOB flows
             * which we don't enable. Log defensively. */
            ESP_LOGW(TAG, "passkey action requested action=%d — ignored",
                     event->passkey.params.action);
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            /* Peer re-pairs (e.g. central forgot its bond). Delete our
             * side and accept the new pairing — mirrors the bleprph
             * example pattern. */
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        default:
            break;
    }
    return 0;
}

/* ---------- host sync / boot ---------- */
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) { ESP_LOGE(TAG, "ensure_addr rc=%d", rc); return; }
    /* Generate unique name from the MAC. */
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_dev_name, sizeof(s_dev_name), "%s%02X%02X%02X",
             CONFIG_DOUDOU_BLE_DEVICE_NAME_PREFIX, mac[3], mac[4], mac[5]);
    ble_svc_gap_device_name_set(s_dev_name);
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "ble host reset reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();   /* returns when nimble_port_stop is called */
    nimble_port_freertos_deinit();
}

/* ---------- public API parallel to bridge_client.c ----------
 * url is ignored (BLE has no addressable endpoint to dial; Bridge scans).
 * handlers are saved + nimble stack is brought up + advertising starts. */
esp_err_t doudou_bridge_connect(const char *url, const doudou_bridge_handlers_t *h)
{
    (void)url;
    if (!h) return ESP_ERR_INVALID_ARG;
    s_h = *h;

    if (!s_send_mutex) s_send_mutex = xSemaphoreCreateMutex();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed %d", err);
        return err;
    }
    ble_hs_cfg.sync_cb           = on_sync;
    ble_hs_cfg.reset_cb          = on_reset;
    ble_hs_cfg.gatts_register_cb = NULL;
    /* Security manager:
     *   Just Works (no IO so no PIN), bonding ON, LE Secure Connections.
     *   MITM stays 0 because Just Works pairing is unauthenticated by
     *   construction — fine for a personal companion device on the
     *   user's own desk. To upgrade, add a passkey/OOB flow + set
     *   sm_mitm = 1. NVS persists the resulting LTK across reboots. */
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 0;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC
                                 | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC
                                 | BLE_SM_PAIR_KEY_DIST_ID;

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc) { ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc) { ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc); return ESP_FAIL; }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "ble peripheral up — waiting for Bridge to scan + connect");
    return ESP_OK;
}

esp_err_t doudou_bridge_follow_thread(const char *thread_id)
{
    if (!thread_id || !*thread_id) return ESP_ERR_INVALID_ARG;
    cJSON *o = doudou_build_follow_thread(s_out_seq++, local_ts_ms(), thread_id);
    if (!o) return ESP_ERR_NO_MEM;
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return ESP_ERR_NO_MEM;
    esp_err_t r = ble_transport_send(body);
    free(body);
    return r;
}

esp_err_t doudou_bridge_reply(const char *question_id, const char *choice_id)
{
    if (!question_id || !choice_id) return ESP_ERR_INVALID_ARG;
    cJSON *o = doudou_build_reply(s_out_seq++, local_ts_ms(),
                                  CONFIG_DOUDOU_DEVICE_ID,
                                  question_id, choice_id);
    if (!o) return ESP_ERR_NO_MEM;
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return ESP_ERR_NO_MEM;
    esp_err_t r = ble_transport_send(body);
    free(body);
    return r;
}
