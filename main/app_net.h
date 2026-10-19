#pragma once
#include "esp_err.h"
#include "app_store.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    APP_NET_IDLE = 0,
    APP_NET_AP_MODE,
    APP_NET_STA_CONNECTING,
    APP_NET_STA_CONNECTED,
    APP_NET_STA_FAILED,
} app_net_state_t;

typedef void (*app_net_cb_t)(app_net_state_t state, const char *ip_or_null);
/**
 * Called after settings are written and the save HTTP response has been
 * sent (may run on a short-lived task, not the httpd task). stay_connected
 * is true when STA was already up with the same SSID/password.
 */
typedef void (*app_net_settings_saved_cb_t)(bool stay_connected);

esp_err_t app_net_start(app_net_cb_t cb);
void app_net_set_settings_saved_cb(app_net_settings_saved_cb_t cb);
app_net_state_t app_net_get_state(void);
void app_net_get_ip(char *buf, size_t len);
/** SoftAP SSID shown on screen for provisioning. */
const char *app_net_ap_ssid(void);
bool app_net_http_running(void);
/** True when SoftAP is up for 192.168.4.1 provisioning. */
bool app_net_softap_portal_active(void);
/** Stop SoftAP/httpd before HTTPS to free heap; STA stays up. */
void app_net_prepare_for_tls(void);
/** Restart config httpd after TLS; SoftAP portal if STA already lost. */
void app_net_restore_httpd(void);
/**
 * Mark that a DeepSeek fetch is armed/in progress so GOT_IP must NOT start
 * httpd (httpd + SoftAP starve TLS on no-PSRAM C3). Safe on httpd task
 * (does not call httpd_stop). Cleared by app_net_restore_httpd().
 */
void app_net_begin_fetch_window(void);
/**
 * Wait until STA is connected with an IP (and SoftAP has been dropped after
 * GOT_IP). Returns ESP_OK when ready, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t app_net_wait_sta_ip(int timeout_ms);
/**
 * Stop httpd, captive DNS, and Wi-Fi radio before deep sleep.
 * SoftAP must not stay advertising while sleeping. On wake, boot runs
 * app_net_start() again and restores SoftAP if still unconfigured.
 */
void app_net_stop_for_sleep(void);
