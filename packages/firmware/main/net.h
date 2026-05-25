/**
 * net.h — Wi-Fi STA + mDNS resolution for Doudou.
 *
 *   doudou_net_start(cb)
 *     1. brings up esp_netif + Wi-Fi STA with creds from sdkconfig
 *     2. on GOT_IP, starts mDNS and resolves CONFIG_DOUDOU_BRIDGE_HOST
 *        (falls back to using the host string as-is if mDNS fails)
 *     3. invokes cb with the ws:// URL (or NULL on permanent failure)
 *
 * If CONFIG_DOUDOU_WIFI_SSID is empty, returns ESP_ERR_NOT_FOUND
 * without doing anything — caller decides whether that's fatal.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Callback signature: `url` is "ws://host_or_ip:port/device" (caller
 *  retains ownership; copy if you need to keep it). Called once on
 *  success, may be called again on Wi-Fi reconnect with a fresh URL. */
typedef void (*doudou_net_ready_cb_t)(const char *url);

esp_err_t doudou_net_start(doudou_net_ready_cb_t cb);

#ifdef __cplusplus
}
#endif
