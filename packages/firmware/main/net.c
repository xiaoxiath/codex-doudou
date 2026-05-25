/**
 * net.c — Wi-Fi STA + mDNS for Doudou.
 *
 * The state machine here is purposely tiny: connect → got IP → resolve
 * Bridge → notify caller. Auto-reconnect on disconnect, re-resolve on
 * each fresh GOT_IP so a Bridge IP change (laptop moved networks) is
 * picked up without a manual restart.
 */
#include "net.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define BRIDGE_HOST_DEFAULT  CONFIG_DOUDOU_BRIDGE_HOST
#define BRIDGE_PORT          CONFIG_DOUDOU_BRIDGE_PORT
#define RECONNECT_DELAY_MS   3000

static doudou_net_ready_cb_t s_cb = NULL;
static bool s_inited = false;

static void resolve_and_announce(void);

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "wifi start → connecting");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "wifi disconnected, retrying in %dms", RECONNECT_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
                esp_wifi_connect();
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        resolve_and_announce();
    }
}

/* Try mDNS first (works when Bridge announces _doudou._tcp). Fall back
 * to using the configured hostname as-is — caller still gets a URL it
 * can try, the WS client will produce a clean error if it can't reach it. */
/* Crude IPv4-literal sniff. Avoids mdns_query_a hanging on numeric hosts
 * (ESP-IDF's mdns library doesn't respect its own timeout for IP-shaped
 * inputs — observed on v5.3). */
static bool host_is_ipv4_literal(const char *s)
{
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) return false;
    }
    return true;
}

static void resolve_and_announce(void)
{
    ESP_LOGI(TAG, "resolve_and_announce entered (cb=%p)", (void *)s_cb);
    if (!s_cb) return;

    char url[96];
    const char *host = BRIDGE_HOST_DEFAULT;

    if (host_is_ipv4_literal(host)) {
        /* Direct IP — skip mDNS entirely. */
        snprintf(url, sizeof(url), "ws://%s:%d/device", host, BRIDGE_PORT);
        ESP_LOGI(TAG, "bridge host is IPv4 literal → %s", url);
    } else {
        /* Strip a trailing ".local" so mDNS query gets the bare hostname. */
        char base[64];
        strncpy(base, host, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        char *dot = strstr(base, ".local");
        if (dot) *dot = '\0';

        esp_ip4_addr_t addr = {0};
        esp_err_t err = mdns_query_a(base, 2000, &addr);
        if (err == ESP_OK) {
            snprintf(url, sizeof(url), "ws://" IPSTR ":%d/device", IP2STR(&addr), BRIDGE_PORT);
            ESP_LOGI(TAG, "mDNS resolved %s → %s", base, url);
        } else {
            snprintf(url, sizeof(url), "ws://%s:%d/device", host, BRIDGE_PORT);
            ESP_LOGW(TAG, "mDNS lookup of %s failed (%s); trying raw host: %s",
                     base, esp_err_to_name(err), url);
        }
    }
    s_cb(url);
}

esp_err_t doudou_net_start(doudou_net_ready_cb_t cb)
{
    if (s_inited) return ESP_ERR_INVALID_STATE;
    s_cb = cb;

    /* NVS holds Wi-Fi calibration data — required even when we don't
     * read user creds from it. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    const char *ssid = CONFIG_DOUDOU_WIFI_SSID;
    const char *pass = CONFIG_DOUDOU_WIFI_PASSWORD;
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "no Wi-Fi SSID configured (set CONFIG_DOUDOU_WIFI_SSID), staying offline");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL));

    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* let the AP dictate */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));

    /* mDNS init before Wi-Fi start so we don't miss the GOT_IP. */
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(CONFIG_DOUDOU_DEVICE_ID);

    ESP_ERROR_CHECK(esp_wifi_start());

    s_inited = true;
    ESP_LOGI(TAG, "wifi starting; ssid=\"%s\" bridge=%s:%d", ssid, BRIDGE_HOST_DEFAULT, BRIDGE_PORT);
    return ESP_OK;
}
