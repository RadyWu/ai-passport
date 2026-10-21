#include "app_net.h"
#include "app_save_policy.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "dns_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "app_net";
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
/** STA retries after disconnect before declaring APP_NET_STA_FAILED. */
#define STA_RETRY_MAX 8

static app_net_cb_t s_cb;
static app_net_settings_saved_cb_t s_saved_cb;
static app_net_state_t s_state = APP_NET_IDLE;
static char s_ip[16];
static char s_ap_ssid[32];
static httpd_handle_t s_httpd;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static EventGroupHandle_t s_wifi_eg;
static int s_retry;
/** SoftAP is up for phone provisioning (192.168.4.1). */
static bool s_softap_portal;
/** True after we switched to STA-only (GOT_IP or prepare_for_tls). */
static bool s_sta_only_radio;
/** True while HTTPS is in flight — do not restore SoftAP/httpd mid-TLS. */
static bool s_tls_busy;
/** Fetch armed or running — GOT_IP must not start httpd (heap for TLS). */
static bool s_fetch_pending;
static wifi_config_t s_ap_cfg;
static dns_server_handle_t s_dns;
/** True while tearing down radio for deep sleep — ignore STA_DISCONNECTED. */
static volatile bool s_stopping_for_sleep;
/** SoftAP already scheduled/up for this outage (don't flap ensure_softap). */
static bool s_softap_for_outage;
/** LAN httpd must be rebound after STA-only → SoftAP before phones can open :80. */
static bool s_httpd_needs_softap_rebind;
/** Deferred SoftAP restore task (never block wifi event task with httpd). */
static TaskHandle_t s_portal_task;
/** First disconnect of this streak (for debounce / logging). */
static int s_disc_streak;
/**
 * SoftAP-up STA scan pacing. Must NOT "throttle" by skipping esp_wifi_connect
 * without scheduling a later attempt — on ESP-IDF, no connect() after
 * STA_DISCONNECTED means scanning stops forever (v0.17 hang until wake).
 * Keepalive task calls connect every STA_SOFTAP_RECONNECT_MS while SoftAP is
 * up and STA is down (gentle enough for beacons, continuous for recovery).
 */
#define STA_SOFTAP_RECONNECT_MS 8000
static TaskHandle_t s_sta_ka_task;
static volatile bool s_sta_ka_wanted;
/**
 * Save is installing a new STA profile. Keepalive and the disconnect handler
 * must not call esp_wifi_connect() until that profile is in the driver.
 */
static volatile bool s_sta_apply_busy;
/** Count real join failures after a credential apply; then show Wi-Fi error. */
static volatile bool s_cred_watch;
static volatile bool s_cred_gave_up;
/**
 * Which saved slot the driver should be using (0..2), or -1 before the first
 * install. Candidates are the scan intersection, not every filled slot.
 */
static int s_active_slot = -1;
/** Join failures already counted on s_active_slot (not including the next one). */
static int s_slot_fails;
/** Driver STA profile is not s_active_slot yet; next connect must set_config. */
static volatile bool s_slot_reload;
/**
 * Join failures on the current visible slot before the next visible one.
 * One visible slot still uses this budget (same as the single-SSID firmware).
 * An absent-AP reason skips the rest of the budget.
 */
#define CRED_JOIN_FAIL_MAX 3
/**
 * Empty scans before the all-failed / SoftAP path when no saved name is in
 * range. Two scans absorb one missed sweep during a short flap. Neither
 * calls connect(), so an invisible network does not add a join timeout.
 */
#define STA_EMPTY_SCAN_MAX 2
#define STA_EMPTY_RESCAN_MS 1500
/** Scan records kept for the SSID intersection (capped). */
#define STA_SCAN_MAX 32
/** Last slot that got an IP (0..2), or -1 when NVS has no preference yet. */
static int s_pref_slot = -1;
/** Slot index waiting for an NVS write, or -1. */
static volatile int s_pref_dirty = -1;
static char s_visible[STA_SCAN_MAX][APP_WIFI_SSID_MAX];
static int s_visible_n;
static wifi_ap_record_t s_scan_recs[STA_SCAN_MAX];
static int s_try_order[APP_WIFI_SLOTS];
static int s_try_n;
static bool s_try_valid;
/** Next choice must scan. Invisible saved SSIDs are not joined. */
static bool s_need_scan = true;
static int s_empty_scans;
static volatile bool s_pick_busy;
static volatile bool s_scan_task_busy;
static int s_scan_delay_ms;
static int s_pending_rescan_ms = -1;
static TickType_t s_last_scan_tick;
/** Serialize stop/start httpd (boot, outage, GOT_IP, phone-join watch). */
static SemaphoreHandle_t s_httpd_mu;

static esp_err_t root_get(httpd_req_t *req);
static esp_err_t root_post(httpd_req_t *req);
static esp_err_t save_post(httpd_req_t *req);
static void stop_httpd(void);
static esp_err_t restart_httpd(void);
static esp_err_t start_httpd(void);
static void ensure_softap_portal(void);
static void start_captive_dns(void);
static void stop_captive_dns(void);
static esp_err_t captive_404(httpd_req_t *req, httpd_err_code_t err);
static void schedule_softap_for_outage(void);
static void ensure_apsta_keepalive(void);
static void ensure_softap_ip_dhcp(void);
static void schedule_httpd_if_needed(const char *why);
static void ensure_sta_softap_keepalive(void);
static void stop_sta_softap_keepalive(void);
static void apply_saved_sta(bool rebind_httpd);
static bool sta_cred_is(const char *ssid, const char *pass);
static bool join_fail_reason(uint8_t reason);
static void install_sta_slot(const app_settings_t *s, int idx);
static int active_slot_index(const app_wifi_cred_t slots[APP_WIFI_SLOTS]);
/** Load Wi-Fi slots without putting app_settings_t on a small event stack. */
static bool load_wifi_slots(app_wifi_cred_t slots[APP_WIFI_SLOTS]);
static bool settings_have_wifi(const app_settings_t *s);
static void schedule_softap_for_outage(void);
static void schedule_scan_pick(int delay_ms);
static void persist_wifi_pref_if_dirty(void);
static void sta_pick_and_connect(const app_wifi_cred_t slots[APP_WIFI_SLOTS],
                                 const char *why, bool from_apply);

static void set_state(app_net_state_t st)
{
    s_state = st;
    if (s_cb) s_cb(st, s_ip[0] ? s_ip : NULL);
}

app_net_state_t app_net_get_state(void) { return s_state; }
void app_net_set_settings_saved_cb(app_net_settings_saved_cb_t cb) { s_saved_cb = cb; }
void app_net_get_ip(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strncpy(buf, s_ip, len - 1);
    buf[len - 1] = '\0';
}
const char *app_net_ap_ssid(void) { return s_ap_ssid; }
bool app_net_http_running(void) { return s_httpd != NULL; }

/** True when Wi-Fi mode actually includes SoftAP (not just an optimistic flag). */
static bool wifi_mode_has_ap(void)
{
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) != ESP_OK) return false;
    return m == WIFI_MODE_AP || m == WIFI_MODE_APSTA;
}

bool app_net_softap_portal_active(void)
{
    /* UI must not claim "可连" unless the radio is really in AP/APSTA. */
    return s_softap_portal && wifi_mode_has_ap();
}

bool app_net_cred_join_gave_up(void)
{
    return s_cred_gave_up;
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

/** Strip leading/trailing ASCII space/CR/LF/tab; drop optional "Bearer " prefix. */
static void trim_field(char *s)
{
    if (!s) return;
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    /* Case-insensitive Bearer prefix from copy-paste. */
    if ((n >= 7) && (strncasecmp(s, "Bearer ", 7) == 0 || strncasecmp(s, "bearer ", 7) == 0)) {
        memmove(s, s + 7, n - 7 + 1);
    }
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !key || !out || out_len == 0) {
        if (out && out_len) out[0] = '\0';
        return false;
    }
    char pat[64];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = NULL;
    size_t klen = strlen(pat);
    /* Match start-of-body or "&key=" so "pass=" does not hit inside values. */
    if (strncmp(body, pat, klen) == 0) {
        p = body;
    } else {
        char amp[66];
        snprintf(amp, sizeof(amp), "&%s=", key);
        const char *q = strstr(body, amp);
        if (q) p = q + 1; /* point at key= */
    }
    if (!p) { out[0] = '\0'; return false; }
    p += klen;
    size_t i = 0;
    while (*p && *p != '&' && i + 1 < out_len) out[i++] = *p++;
    out[i] = '\0';
    url_decode(out);
    return true;
}

/** Escape for HTML attribute / textarea body: &, ", <, >. */
static void html_escape(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!in) return;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        const char *rep = NULL;
        size_t rlen = 0;
        switch (*p) {
        case '&': rep = "&amp;"; rlen = 5; break;
        case '"': rep = "&quot;"; rlen = 6; break;
        case '<': rep = "&lt;"; rlen = 4; break;
        case '>': rep = "&gt;"; rlen = 4; break;
        default: break;
        }
        if (rep) {
            if (j + rlen >= out_len) break;
            memcpy(out + j, rep, rlen);
            j += rlen;
        } else {
            if (j + 1 >= out_len) break;
            out[j++] = (char)*p;
        }
    }
    out[j] = '\0';
}

static const char *PAGE_HTML_HEAD =
    "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>AI护照配置</title><style>"
    "body{font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 16px;background:#1a2332;color:#e8eef7}"
    "h1{font-size:1.3rem}label{display:block;margin-top:12px;font-size:.9rem;opacity:.85}"
    "input,textarea{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #445;background:#0f1622;color:#e8eef7}"
    "button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:8px;background:#3d9cf0;color:#fff;font-size:1rem}"
    "p.hint{opacity:.7;font-size:.85rem}</style></head><body>"
    "<h1>AI护照 · 解闷阅读</h1>"
    "<p class=hint>最多填写 3 个 2.4GHz Wi-Fi，按 Wi-Fi 1、2、3 的顺序尝试，连上一个就停止；空着的名称会跳过。"
    "首次请填该 Wi-Fi 密码；已保存过可留空保留原密码。无密码网络请在密码框填一个空格。"
    "首次保存并联网成功后会自动拉取一篇短文。之后再保存只连接 Wi-Fi，不换正在读的短文；换一篇请在设备上长按 OK。"
    "若都连不上，热点仍开着，请连 AIPassport-xxxx 打开 192.168.4.1 重配。</p>"
    "<form method=POST action=/save enctype=\"application/x-www-form-urlencoded\">"
    "<input type=hidden name=cfg id=cfg value=\"\">";

static const char *PAGE_HTML_TAIL =
    "<button type=submit id=btn>保存并联网</button></form>"
    "<p class=hint id=st></p>"
    "<p class=hint>联网成功后同一局域网可用屏幕上的 IP 打开本页；丢网后请改用设备热点 192.168.4.1。"
    "若热点登录页点保存没反应，请用系统浏览器打开 http://192.168.4.1/</p>"
    "<script>"
    "document.querySelector('form').addEventListener('submit',function(e){"
    "var b=document.getElementById('btn'),s=document.getElementById('st'),c=document.getElementById('cfg');"
    "if(b.disabled){e.preventDefault();return;}"
    "if(c)c.value='1';"
    "b.disabled=true;b.textContent='正在保存…';"
    "if(s)s.textContent='已提交，请稍候；成功后本页会显示「已保存」。';"
    "});</script>"
    "</body></html>";

/** Worst-case escaped length for preference (largest field). */
#define HTML_ESC_BUF (APP_STORE_PROMPT_MAX * 6 + 1)

/**
 * Chunked HTTP ends the body on a zero-length chunk.
 * httpd_resp_sendstr_chunk("") therefore closes the response early —
 * never send empty prefill values through it.
 */
static esp_err_t chunk_str(httpd_req_t *req, const char *str)
{
    if (!str || str[0] == '\0') {
        return ESP_OK;
    }
    return httpd_resp_sendstr_chunk(req, str);
}

static esp_err_t root_get(httpd_req_t *req)
{
    app_settings_t s;
    memset(&s, 0, sizeof(s));
    (void)app_store_load_settings(&s);

    char *esc = malloc(HTML_ESC_BUF);
    if (!esc) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HTML_HEAD);

    /*
     * Prefill SSIDs so a configured device does not show a blank name.
     * Never prefill Wi-Fi passwords: many browsers omit password values on
     * re-submit, which used to overwrite NVS with "" and force a bad reconnect.
     * A blank password on save keeps that slot's saved password.
     */
    static const char *const slot_ssid_names[APP_WIFI_SLOTS] = {"ssid", "ssid2", "ssid3"};
    static const char *const slot_pass_names[APP_WIFI_SLOTS] = {"pass", "pass2", "pass3"};
    static const char *const slot_ssid_labels[APP_WIFI_SLOTS] = {
        "Wi-Fi 1 名称 (SSID)",
        "Wi-Fi 2 名称 (SSID)",
        "Wi-Fi 3 名称 (SSID)",
    };
    static const char *const slot_pass_labels[APP_WIFI_SLOTS] = {
        "Wi-Fi 1 密码",
        "Wi-Fi 2 密码",
        "Wi-Fi 3 密码",
    };
    for (int i = 0; i < APP_WIFI_SLOTS; i++) {
        httpd_resp_sendstr_chunk(req, "<label>");
        httpd_resp_sendstr_chunk(req, slot_ssid_labels[i]);
        httpd_resp_sendstr_chunk(req, "</label><input name=");
        httpd_resp_sendstr_chunk(req, slot_ssid_names[i]);
        httpd_resp_sendstr_chunk(req, " maxlength=32 autocomplete=off value=\"");
        html_escape(s.wifi[i].ssid, esc, HTML_ESC_BUF);
        chunk_str(req, esc);
        httpd_resp_sendstr_chunk(req, "\">");

        httpd_resp_sendstr_chunk(req, "<label>");
        httpd_resp_sendstr_chunk(req, slot_pass_labels[i]);
        httpd_resp_sendstr_chunk(req,
            "</label><input type=password autocomplete=new-password name=");
        httpd_resp_sendstr_chunk(req, slot_pass_names[i]);
        if (s.wifi[i].pass[0] != '\0') {
            httpd_resp_sendstr_chunk(req, " placeholder=\"已保存可留空保留\">");
        } else if (i == 0) {
            httpd_resp_sendstr_chunk(req, " placeholder=\"首次必填；已保存可留空保留\">");
        } else {
            httpd_resp_sendstr_chunk(req, " placeholder=\"可选；无密码请填一个空格\">");
        }
    }

    html_escape(s.api_key, esc, HTML_ESC_BUF);
    httpd_resp_sendstr_chunk(req, "<label>DeepSeek API Key</label><input name=apikey required autocomplete=off value=\"");
    chunk_str(req, esc);
    httpd_resp_sendstr_chunk(req, "\">");

    html_escape(s.preference, esc, HTML_ESC_BUF);
    httpd_resp_sendstr_chunk(req,
        "<label>偏好提示词（喜欢读什么）</label>"
        "<textarea name=pref rows=4 placeholder=\"例如：轻松冷知识、简短笑话、健康小贴士\">");
    chunk_str(req, esc);
    httpd_resp_sendstr_chunk(req, "</textarea>");

    httpd_resp_sendstr_chunk(req, PAGE_HTML_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    free(esc);
    /* Do not log ssid/pass/apikey/pref contents. */
    ESP_LOGI(TAG, "config page served (prefill=%d)",
             (int)(s.has_wifi || s.has_api_key || s.preference[0] != '\0'));
    return ESP_OK;
}

static bool driver_sta_cred(char *ssid, size_t ssid_n, char *pass, size_t pass_n)
{
    wifi_config_t cur = { 0 };
    if (!ssid || !pass || ssid_n == 0 || pass_n == 0) return false;
    ssid[0] = '\0';
    pass[0] = '\0';
    if (s_state != APP_NET_STA_CONNECTED) return false;
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) != ESP_OK) return false;
    memcpy(ssid, cur.sta.ssid, ssid_n - 1);
    ssid[ssid_n - 1] = '\0';
    memcpy(pass, cur.sta.password, pass_n - 1);
    pass[pass_n - 1] = '\0';
    return true;
}

static esp_err_t send_html(httpd_req_t *req, const char *html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

/** Read POST body even when Content-Length is 0/missing (chunked / some phones). */
static esp_err_t read_post_body(httpd_req_t *req, char *buf, size_t buf_cap, int *out_len)
{
    if (!buf || buf_cap < 2 || !out_len) return ESP_ERR_INVALID_ARG;
    *out_len = 0;
    buf[0] = '\0';

    int total = req->content_len;
    int received = 0;

    if (total > 0) {
        if ((size_t)total >= buf_cap) {
            ESP_LOGW(TAG, "POST too large content_len=%d", total);
            return ESP_ERR_INVALID_SIZE;
        }
        int timeouts = 0;
        while (received < total) {
            int r = httpd_req_recv(req, buf + received, total - received);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts > 20) {
                    ESP_LOGW(TAG, "POST recv timeout after %d/%d", received, total);
                    return ESP_ERR_TIMEOUT;
                }
                continue;
            }
            if (r <= 0) {
                ESP_LOGW(TAG, "POST recv fail r=%d after %d/%d", r, received, total);
                return ESP_FAIL;
            }
            timeouts = 0;
            received += r;
        }
        buf[received] = '\0';
        *out_len = received;
        return ESP_OK;
    }

    /* content_len <= 0: drain until peer closes / no more data (cap buf_cap-1). */
    ESP_LOGW(TAG, "POST content_len=%d — draining up to %u bytes", total, (unsigned)(buf_cap - 1));
    int idle_timeouts = 0;
    while ((size_t)received < buf_cap - 1) {
        int r = httpd_req_recv(req, buf + received, (buf_cap - 1) - (size_t)received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (received > 0) break; /* got some data, then idle → done */
            if (++idle_timeouts >= 3) break;
            continue;
        }
        if (r == 0) break; /* peer closed */
        if (r < 0) {
            if (received > 0) break;
            ESP_LOGW(TAG, "POST drain recv fail r=%d", r);
            return ESP_FAIL;
        }
        received += r;
        idle_timeouts = 0;
    }
    buf[received] = '\0';
    *out_len = received;
    return (received > 0) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}


/**
 * Captive portal probes often POST to /. Never treat as settings save.
 * Drain body and redirect to the config page.
 */
static esp_err_t root_post(httpd_req_t *req)
{
    char sink[256];
    int total = req->content_len;
    if (total > 0) {
        int got = 0;
        while (got < total) {
            size_t want = (size_t)(total - got);
            if (want > sizeof(sink)) want = sizeof(sink);
            int r = httpd_req_recv(req, sink, want);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (r <= 0) break;
            got += r;
        }
    } else {
        /* Missing/zero Content-Length: brief drain, then stop. */
        for (int i = 0; i < 4; i++) {
            int r = httpd_req_recv(req, sink, sizeof(sink));
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (r <= 0) break;
        }
    }
    ESP_LOGI(TAG, "ignored POST / (captive probe) content_len=%d", req->content_len);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, "Redirect", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

typedef struct {
    bool stay_connected;
    bool auto_fetch;
    bool apply_sta;
} save_followup_t;

static void save_followup_task(void *arg)
{
    save_followup_t *f = (save_followup_t *)arg;
    /* Let the SoftAP TCP stack finish sending 「已保存」 before radio churn. */
    vTaskDelay(pdMS_TO_TICKS(750));

    if (s_saved_cb) {
        s_saved_cb(f->stay_connected, f->auto_fetch);
    }

    if (f->apply_sta) {
        /* Drop the in-flight STA attempt, then scan and join a visible saved slot. */
        apply_saved_sta(true);
    } else if (!f->stay_connected) {
        ESP_LOGW(TAG, "settings saved but ssid empty — stay SoftAP, no STA connect");
        ensure_softap_portal();
        set_state(APP_NET_AP_MODE);
    } else {
        ESP_LOGI(TAG, "settings saved; STA unchanged — skip reconnect (fetch=%d)",
                 (int)f->auto_fetch);
    }

    free(f);
    vTaskDelete(NULL);
}

static esp_err_t save_post(httpd_req_t *req)
{
    char *body = calloc(1, 4097);
    if (!body) {
        send_html(req,
                  "<!DOCTYPE html><html><meta charset=utf-8>"
                  "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
                  "<h2>内存不足</h2><p>请关闭其它页面后重试。</p>"
                  "<p><a href=\"/\" style=\"color:#3d9cf0\">返回</a></p></body></html>");
        return ESP_ERR_NO_MEM;
    }

    int body_len = 0;
    esp_err_t rerr = read_post_body(req, body, 4097, &body_len);
    if (rerr != ESP_OK) {
        ESP_LOGW(TAG, "save_post body read failed: %s len=%d content_len=%d",
                 esp_err_to_name(rerr), body_len, req->content_len);
        free(body);
        send_html(req,
                  "<!DOCTYPE html><html><meta charset=utf-8>"
                  "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
                  "<h2>提交失败</h2>"
                  "<p>没有收到表单内容（部分手机热点页会这样）。请用系统浏览器打开 "
                  "<b>http://192.168.4.1/</b> 再保存一次。</p>"
                  "<p><a href=\"/\" style=\"color:#3d9cf0\">返回修改</a></p></body></html>");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "save_post body_len=%d content_len=%d", body_len, req->content_len);

    /* Real browser form POSTs send this; captive probes usually do not. */
    {
        char ct[96] = {0};
        bool ct_ok = false;
        if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) == ESP_OK) {
            ct_ok = (strcasestr(ct, "application/x-www-form-urlencoded") != NULL);
        }
        if (!ct_ok) {
            ESP_LOGW(TAG, "save_post rejected: bad/missing Content-Type");
            free(body);
            send_html(req,
                      "<!DOCTYPE html><html><meta charset=utf-8>"
                      "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
                      "<h2>无效提交</h2>"
                      "<p>请从配置页点击「保存并联网」，不要用系统自动打开的探测页。</p>"
                      "<p><a href=\"/\" style=\"color:#3d9cf0\">打开配置页</a></p></body></html>");
            return ESP_OK;
        }
    }

    app_settings_t prev;
    memset(&prev, 0, sizeof(prev));
    (void)app_store_load_settings(&prev);

    app_settings_t s;
    memset(&s, 0, sizeof(s));
    static const char *const slot_ssid_keys[APP_WIFI_SLOTS] = {"ssid", "ssid2", "ssid3"};
    static const char *const slot_pass_keys[APP_WIFI_SLOTS] = {"pass", "pass2", "pass3"};
    char raw_ssid[APP_WIFI_SLOTS][APP_STORE_SSID_MAX];
    char raw_pass[APP_WIFI_SLOTS][APP_STORE_PASS_MAX];
    bool got_ssid_field[APP_WIFI_SLOTS];
    bool open_pass[APP_WIFI_SLOTS];
    memset(raw_ssid, 0, sizeof(raw_ssid));
    memset(raw_pass, 0, sizeof(raw_pass));
    for (int i = 0; i < APP_WIFI_SLOTS; i++) {
        got_ssid_field[i] = form_get(body, slot_ssid_keys[i], raw_ssid[i], sizeof(raw_ssid[i]));
        (void)form_get(body, slot_pass_keys[i], raw_pass[i], sizeof(raw_pass[i]));
        /* Detect the open-network space before trim_field eats it. */
        open_pass[i] = app_wifi_pass_is_open_marker(raw_pass[i]);
        trim_field(raw_ssid[i]);
        if (!open_pass[i]) trim_field(raw_pass[i]);
    }
    bool got_ssid = got_ssid_field[0];
    bool got_key = form_get(body, "apikey", s.api_key, sizeof(s.api_key));
    bool got_pref = form_get(body, "pref", s.preference, sizeof(s.preference));
    char cfg_tok[8] = {0};
    bool got_cfg = form_get(body, "cfg", cfg_tok, sizeof(cfg_tok));
    (void)got_pref;
    trim_field(s.api_key);
    {
        char *p = s.preference;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (p != s.preference) memmove(s.preference, p, strlen(p) + 1);
        size_t n = strlen(s.preference);
        while (n && (s.preference[n - 1] == ' ' || s.preference[n - 1] == '\t'
                     || s.preference[n - 1] == '\r' || s.preference[n - 1] == '\n')) {
            s.preference[--n] = '\0';
        }
    }
    strncpy(s.model, "deepseek-chat", sizeof(s.model) - 1);
    free(body);
    body = NULL;

    ESP_LOGI(TAG, "save form: got_cfg=%d got_ssid=%d got_key=%d ssid_len=%u/%u/%u key_len=%u pref_len=%u",
             (int)got_cfg, (int)got_ssid, (int)got_key,
             (unsigned)strlen(raw_ssid[0]), (unsigned)strlen(raw_ssid[1]),
             (unsigned)strlen(raw_ssid[2]),
             (unsigned)strlen(s.api_key), (unsigned)strlen(s.preference));

    /* Captive probes / forged POSTs lack our hidden cfg=1 token. */
    if (!got_cfg || strcmp(cfg_tok, "1") != 0) {
        ESP_LOGW(TAG, "save_post rejected: missing cfg token");
        send_html(req,
                  "<!DOCTYPE html><html><meta charset=utf-8>"
                  "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
                  "<h2>无效提交</h2>"
                  "<p>请打开配置页后点击「保存并联网」。</p>"
                  "<p><a href=\"/\" style=\"color:#3d9cf0\">打开配置页</a></p></body></html>");
        return ESP_OK;
    }
    /* Require explicit form fields — do not invent save from partial/probe bodies. */
    if (!got_ssid || !got_key) {
        ESP_LOGW(TAG, "save_post rejected: incomplete form fields");
        send_html(req,
                  "<!DOCTYPE html><html><meta charset=utf-8>"
                  "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
                  "<h2>表单不完整</h2>"
                  "<p>请用配置页填写 Wi-Fi 与 API Key 后再保存。</p>"
                  "<p><a href=\"/\" style=\"color:#3d9cf0\">返回修改</a></p></body></html>");
        return ESP_OK;
    }

    /*
     * Missing ssid2/ssid3 (older page or partial body) keeps that saved slot.
     * A present but empty name clears it. Blank password keeps the saved one.
     * A space-only password is an open network and does not keep the old one.
     */
    int need_pass_slot = -1;
    for (int i = 0; i < APP_WIFI_SLOTS; i++) {
        if (!got_ssid_field[i]) {
            s.wifi[i] = prev.wifi[i];
            continue;
        }
        app_wifi_merge_status_t ms = app_wifi_merge_slot(
            &s.wifi[i], raw_ssid[i], open_pass[i] ? "" : raw_pass[i], open_pass[i],
            &prev.wifi[i]);
        if (ms == APP_WIFI_MERGE_NEED_PASS && need_pass_slot < 0) need_pass_slot = i;
    }
    const bool had_key = prev.api_key[0] != '\0';
    if (s.api_key[0] == '\0' && had_key) {
        strncpy(s.api_key, prev.api_key, sizeof(s.api_key) - 1);
        s.api_key[sizeof(s.api_key) - 1] = '\0';
    }

    if (app_wifi_first_slot(s.wifi) < 0) {
        send_html(req,
                  "<!DOCTYPE html><html><meta charset=utf-8>"
                  "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
                  "<h2>请填写 Wi-Fi 名称</h2>"
                  "<p>至少填写一个 Wi-Fi 名称。空着的不会尝试。</p>"
                  "<p><a href=\"/\" style=\"color:#3d9cf0\">返回修改</a></p></body></html>");
        return ESP_OK;
    }
    if (need_pass_slot >= 0) {
        char page[640];
        snprintf(page, sizeof(page),
                 "<!DOCTYPE html><html><meta charset=utf-8>"
                 "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
                 "<h2>请填写 Wi-Fi %d 密码</h2>"
                 "<p>这个名称还没有保存过密码。若确认是无密码网络，请在密码框填一个空格后保存。</p>"
                 "<p><a href=\"/\" style=\"color:#3d9cf0\">返回修改</a></p></body></html>",
                 need_pass_slot + 1);
        send_html(req, page);
        return ESP_OK;
    }
    if (s.api_key[0] == '\0') {
        send_html(req,
                  "<!DOCTYPE html><html><meta charset=utf-8>"
                  "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
                  "<h2>请填写 API Key</h2>"
                  "<p>需要 DeepSeek API Key 才能拉取短文。请返回填写后再保存。</p>"
                  "<p><a href=\"/\" style=\"color:#3d9cf0\">返回修改</a></p></body></html>");
        return ESP_OK;
    }
    s.has_wifi = true;
    s.has_api_key = true;
    strncpy(s.wifi_ssid, s.wifi[0].ssid, sizeof(s.wifi_ssid) - 1);
    strncpy(s.wifi_pass, s.wifi[0].pass, sizeof(s.wifi_pass) - 1);

    esp_err_t serr = app_store_save_settings(&s);
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "NVS save_settings failed: %s", esp_err_to_name(serr));
        send_html(req,
                  "<!DOCTYPE html><html><meta charset=utf-8>"
                  "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
                  "<h2>保存失败</h2>"
                  "<p>写入设备存储出错，请重试。若反复失败请整包重刷固件。</p>"
                  "<p><a href=\"/\" style=\"color:#3d9cf0\">返回</a></p></body></html>");
        return ESP_OK;
    }

    /* Verify apikey actually landed in NVS. */
    app_settings_t verify;
    memset(&verify, 0, sizeof(verify));
    (void)app_store_load_settings(&verify);
    if (!verify.has_api_key || strcmp(verify.api_key, s.api_key) != 0
        || !verify.has_wifi || !app_wifi_sets_equal(verify.wifi, s.wifi)) {
        ESP_LOGE(TAG, "NVS verify mismatch after save (wifi=%d key=%d)",
                 (int)verify.has_wifi, (int)verify.has_api_key);
        send_html(req,
                  "<!DOCTYPE html><html><meta charset=utf-8>"
                  "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
                  "<h2>保存未生效</h2>"
                  "<p>配置写入后读回不一致，请再保存一次。</p>"
                  "<p><a href=\"/\" style=\"color:#3d9cf0\">返回</a></p></body></html>");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "NVS verified ssid_len=%u/%u/%u key_len=%u",
             (unsigned)strlen(verify.wifi[0].ssid),
             (unsigned)strlen(verify.wifi[1].ssid),
             (unsigned)strlen(verify.wifi[2].ssid),
             (unsigned)strlen(verify.api_key));

    char drv_ssid[APP_STORE_SSID_MAX];
    char drv_pass[APP_STORE_PASS_MAX];
    const bool have_drv = driver_sta_cred(drv_ssid, sizeof(drv_ssid), drv_pass, sizeof(drv_pass));
    const bool wifi_unchanged = app_wifi_sets_equal(s.wifi, prev.wifi);
    const app_save_action_t action = app_save_decide((app_save_facts_t) {
        .sta_connected = (s_state == APP_NET_STA_CONNECTED),
        .wifi_unchanged = wifi_unchanged,
        .driver_matches_new = have_drv && app_wifi_driver_keeps_link(
            s.wifi, prev.wifi, drv_ssid, drv_pass),
        .has_stored_article = app_store_has_article(),
        .ssid_nonempty = app_wifi_first_slot(s.wifi) >= 0,
    });
    const bool stay_connected = action.stay_connected;

    const char *ok;
    if (stay_connected && action.auto_fetch) {
        ok = "<!DOCTYPE html><html><meta charset=utf-8>"
             "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
             "<h2>已保存</h2>"
             "<p>设置已写入。请看设备屏幕：正在更新内容，或提示稍后再读。</p>"
             "<p style=\"opacity:.75;font-size:.9rem\">同一局域网可继续用屏幕上的 IP 打开本页。</p>"
             "</body></html>";
    } else if (stay_connected) {
        ok = "<!DOCTYPE html><html><meta charset=utf-8>"
             "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
             "<h2>已保存</h2>"
             "<p>设置已写入。正在读的短文保持不变。要换一篇请在设备上长按 OK。</p>"
             "<p style=\"opacity:.75;font-size:.9rem\">同一局域网可继续用屏幕上的 IP 打开本页。</p>"
             "</body></html>";
    } else if (action.auto_fetch) {
        ok = "<!DOCTYPE html><html><meta charset=utf-8>"
             "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
             "<h2>已保存</h2>"
             "<p>正在按 Wi-Fi 1、2、3 的顺序连接，连上一个就停止。连上后会自动拉取一篇短文。可关闭本页。</p>"
             "<p style=\"opacity:.75;font-size:.9rem\">若屏幕提示连不上，说明填写的网络都没连上。"
             "请检查名称和密码，重新连接设备热点（AIPassport-xxxx），再打开 192.168.4.1 修改。</p>"
             "</body></html>";
    } else {
        ok = "<!DOCTYPE html><html><meta charset=utf-8>"
             "<body style=\"font-family:sans-serif;background:#1a2332;color:#e8eef7;padding:24px\">"
             "<h2>已保存</h2>"
             "<p>正在按 Wi-Fi 1、2、3 的顺序连接，连上一个就停止。正在读的短文不会被替换。可关闭本页。</p>"
             "<p style=\"opacity:.75;font-size:.9rem\">若屏幕提示连不上，说明填写的网络都没连上。"
             "请检查名称和密码，重新连接设备热点（AIPassport-xxxx），再打开 192.168.4.1 修改。</p>"
             "</body></html>";
    }
    send_html(req, ok);

    /* Defer SoftAP/STA churn so the HTTP response can leave the radio first. */
    save_followup_t *f = calloc(1, sizeof(*f));
    if (!f) {
        ESP_LOGE(TAG, "followup alloc failed — connecting after short delay");
        vTaskDelay(pdMS_TO_TICKS(750));
        if (s_saved_cb) s_saved_cb(stay_connected, action.auto_fetch);
        if (action.apply_sta) apply_saved_sta(false);
        return ESP_OK;
    }
    f->stay_connected = stay_connected;
    f->auto_fetch = action.auto_fetch;
    f->apply_sta = action.apply_sta;
    if (xTaskCreate(save_followup_task, "save_fu", 6144, f, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "followup task create failed");
        free(f);
        vTaskDelay(pdMS_TO_TICKS(750));
        if (s_saved_cb) s_saved_cb(stay_connected, action.auto_fetch);
        /* Still on the httpd task — do not httpd_stop from inside the handler. */
        if (action.apply_sta) apply_saved_sta(false);
    }
    return ESP_OK;
}


static void stop_captive_dns(void)
{
    if (!s_dns) return;
    stop_dns_server(s_dns);
    s_dns = NULL;
    ESP_LOGI(TAG, "captive DNS stopped");
}

static void start_captive_dns(void)
{
    if (s_dns) return;
    dns_server_config_t cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&cfg);
    if (s_dns) {
        ESP_LOGI(TAG, "captive DNS started (*. -> SoftAP IP)");
    } else {
        ESP_LOGW(TAG, "captive DNS failed to start");
    }
}

/** Phone captive probes hit random paths; redirect so / is reachable. */
static esp_err_t captive_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    /* iOS needs a non-empty body to treat this as a captive portal. */
    httpd_resp_send(req, "Redirect to AI Passport config", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void stop_httpd_unlocked(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    ESP_LOGI(TAG, "httpd stopped");
}

static void stop_httpd(void)
{
    if (s_httpd_mu) xSemaphoreTake(s_httpd_mu, portMAX_DELAY);
    stop_httpd_unlocked();
    if (s_httpd_mu) xSemaphoreGive(s_httpd_mu);
}

/**
 * Force stop+start. httpd binds INADDR_ANY:80 — works for SoftAP 192.168.4.1
 * and later STA LAN IP, but after STA-only → APSTA the old server can leave
 * sockets that never accept SoftAP clients; callers must restart on that path.
 * NEVER call from the wifi/IP event task (stack ~2.3KB).
 */
static esp_err_t restart_httpd_unlocked(void)
{
    stop_httpd_unlocked();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 12;
    /*
     * Phones open many captive probes; too few sockets starve /. Too many
     * fail httpd_start on no-PSRAM C3. 5 + LRU is the measured sweet spot.
     */
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    /* 3-slot form keeps several app_settings_t locals on this task. */
    config.stack_size = 8192;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 10;
    config.backlog_conn = 3;
    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s free=%u largest=%u",
                 esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        s_httpd = NULL;
        return err;
    }
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t root_p = { .uri = "/", .method = HTTP_POST, .handler = root_post };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &root_p);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, captive_404);
    ESP_LOGI(TAG, "HTTP config server (re)started (:80) free=%u",
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

static esp_err_t restart_httpd(void)
{
    if (s_httpd_mu) xSemaphoreTake(s_httpd_mu, portMAX_DELAY);
    esp_err_t err = restart_httpd_unlocked();
    if (s_httpd_mu) xSemaphoreGive(s_httpd_mu);
    return err;
}

static esp_err_t start_httpd(void)
{
    if (s_httpd_mu) xSemaphoreTake(s_httpd_mu, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_httpd) err = restart_httpd_unlocked();
    if (s_httpd_mu) xSemaphoreGive(s_httpd_mu);
    return err;
}

/**
 * Bring SoftAP (+ APSTA if saved Wi-Fi). Call ONLY from app tasks — never
 * from the wifi/IP event task (httpd_start + set_mode overflow 2.3KB stack).
 *
 * Critical for outage path: after GOT_IP we run STA-only. Switching back to
 * APSTA while STA is mid-connect often leaves SoftAP "flagged" but not
 * beaconing. Settle disconnect first, verify mode, start AP DHCP.
 */
static void ensure_apsta_keepalive(void)
{
    if (s_stopping_for_sleep) return;
    app_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    (void)app_store_load_settings(&settings);
    wifi_mode_t want = settings_have_wifi(&settings) ? WIFI_MODE_APSTA : WIFI_MODE_AP;

    wifi_mode_t cur = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&cur);

    /*
     * STA-only → APSTA: disconnect first so the driver can raise SoftAP.
     * Calling set_mode(APSTA) during an active STA connect often "succeeds"
     * without ever broadcasting AIPassport-xxxx.
     */
    if (cur == WIFI_MODE_STA) {
        ESP_LOGI(TAG, "STA-only → SoftAP: disconnect before set_mode(%d)", (int)want);
        s_httpd_needs_softap_rebind = true;
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    esp_err_t err = esp_wifi_set_mode(want);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set mode %d: %s", (int)want, esp_err_to_name(err));
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set AP config: %s", esp_err_to_name(err));
    }
    if (settings_have_wifi(&settings)) {
        /* Keep the slot already being searched; do not force Wi-Fi 1 mid-rotation.
         * Skip set_config when the driver already holds that slot. */
        int idx = active_slot_index(settings.wifi);
        if (idx >= 0 && (s_slot_reload
                         || !sta_cred_is(settings.wifi[idx].ssid, settings.wifi[idx].pass))) {
            install_sta_slot(&settings, idx);
        }
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "wifi_start: %s", esp_err_to_name(err));
    }

    /* Let SoftAP come up before we claim success / restart STA connect. */
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_mode_t got = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&got);
    if (got == WIFI_MODE_AP || got == WIFI_MODE_APSTA) {
        s_softap_portal = true;
        s_sta_only_radio = false;
        ensure_softap_ip_dhcp();
        ESP_LOGI(TAG, "SoftAP radio verified mode=%d ssid=%s", (int)got, s_ap_ssid);
    } else {
        /* Do not lie to UI — phone cannot see a non-AP mode hotspot. */
        s_softap_portal = false;
        ESP_LOGE(TAG, "SoftAP NOT up after set_mode (got mode=%d want=%d)",
                 (int)got, (int)want);
    }
}

/**
 * After STA-only → APSTA, SoftAP netif/DHCP can be down even though the radio
 * beacons. Phones then "join" with no usable 192.168.4.1. Force classic AP IP.
 */
static void ensure_softap_ip_dhcp(void)
{
    if (!s_ap) return;
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ip_info.gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);

    esp_err_t err = esp_netif_dhcps_stop(s_ap);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "AP dhcps_stop: %s", esp_err_to_name(err));
    }
    err = esp_netif_set_ip_info(s_ap, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP set_ip_info: %s", esp_err_to_name(err));
    }
    err = esp_netif_dhcps_start(s_ap);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "AP dhcps_start: %s", esp_err_to_name(err));
    }
    esp_netif_ip_info_t got;
    memset(&got, 0, sizeof(got));
    if (esp_netif_get_ip_info(s_ap, &got) == ESP_OK) {
        ESP_LOGI(TAG, "SoftAP IP/DHCP ready " IPSTR " dhcps_err=%s",
                 IP2STR(&got.ip), esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "SoftAP get_ip_info failed after DHCP setup");
    }
}

/** One-shot watch so AP_STACONNECTED does not spawn a task storm. */
static volatile bool s_httpd_watch_busy;

static void portal_http_watch_task(void *arg)
{
    const char *why = arg ? (const char *)arg : "watch";
    /* Let SoftAP netif/DHCP settle after AP_START / set_mode(APSTA). */
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!s_stopping_for_sleep && !s_tls_busy && (s_softap_portal || wifi_mode_has_ap())) {
        s_fetch_pending = false;
        ensure_softap_ip_dhcp();
        start_captive_dns();
        /*
         * Always restart when rebind requested (phone joined / STA→APSTA).
         * Skipping "httpd already up" was the v0.16 SoftAP config-page bug:
         * LAN-era sockets stayed listening but SoftAP clients got no :80.
         */
        const bool need = !s_httpd || s_httpd_needs_softap_rebind;
        if (need) {
            for (int attempt = 0; attempt < 4; attempt++) {
                esp_err_t err = restart_httpd();
                if (err == ESP_OK) {
                    s_httpd_needs_softap_rebind = false;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            ESP_LOGI(TAG, "%s: SoftAP httpd watch done http=%d dns=%d",
                     why, s_httpd != NULL, s_dns != NULL);
        } else {
            ESP_LOGI(TAG, "%s: SoftAP httpd already OK", why);
        }
    }
    s_httpd_watch_busy = false;
    vTaskDelete(NULL);
}

/** httpd_start is unsafe on the wifi event stack — always defer. */
static void schedule_httpd_if_needed(const char *why)
{
    if (s_stopping_for_sleep || s_tls_busy) return;
    if (s_httpd && !s_httpd_needs_softap_rebind) return;
    if (s_httpd_watch_busy) return;
    /* Outage SoftAP must serve config even if a stale fetch window flag stuck. */
    s_fetch_pending = false;
    s_httpd_watch_busy = true;
    /* Pass a string literal only (static storage). */
    if (xTaskCreate(portal_http_watch_task, "ap_httpd", 6144,
                    (void *)(why ? why : "httpd"), 5, NULL) != pdPASS) {
        s_httpd_watch_busy = false;
        ESP_LOGW(TAG, "%s: could not defer httpd watch", why ? why : "httpd");
    } else {
        ESP_LOGI(TAG, "%s: deferred SoftAP httpd watch", why ? why : "httpd");
    }
}

/**
 * Bring SoftAP + httpd for provisioning. Idempotent when portal already healthy.
 * Always APSTA if saved Wi-Fi exists (STA keeps retrying home SSID).
 */
static void ensure_softap_portal(void)
{
    if (s_stopping_for_sleep) return;

    app_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    (void)app_store_load_settings(&settings);

    /*
     * LAN httpd started after GOT_IP while STA-only often leaves sockets that
     * do not accept SoftAP clients after set_mode(APSTA). Always restart httpd
     * when leaving STA-only or when httpd is missing — never skip that case.
     */
    const bool from_sta_only = s_sta_only_radio;
    const bool need_rebind = s_httpd_needs_softap_rebind || from_sta_only || !s_httpd;
    ensure_apsta_keepalive();
    ensure_softap_ip_dhcp();
    start_captive_dns();

    if (!need_rebind && s_softap_portal && wifi_mode_has_ap() && s_httpd) {
        ESP_LOGI(TAG, "SoftAP portal already healthy (skip httpd restart)");
        return;
    }

    /* Give SoftAP netif / DHCP a moment before binding :80. */
    vTaskDelay(pdMS_TO_TICKS(150));
    s_fetch_pending = false;

    for (int i = 0; i < 3; i++) {
        esp_err_t err = restart_httpd();
        if (err == ESP_OK) {
            s_httpd_needs_softap_rebind = false;
            break;
        }
        ESP_LOGW(TAG, "httpd retry %d/3 after SoftAP (heap free=%u largest=%u)",
                 i + 1,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ESP_LOGI(TAG, "SoftAP portal ready ssid=%s http=%d dns=%d mode=%s softap=%d from_sta_only=%d",
             s_ap_ssid, s_httpd != NULL, s_dns != NULL,
             settings_have_wifi(&settings) ? "APSTA" : "AP", (int)s_softap_portal,
             (int)from_sta_only);
}

static bool sta_cred_is(const char *ssid, const char *pass)
{
    wifi_config_t cur = { 0 };
    if (!ssid || esp_wifi_get_config(WIFI_IF_STA, &cur) != ESP_OK) return false;
    size_t n = strnlen(ssid, sizeof(cur.sta.ssid));
    if (n == 0 || memcmp(cur.sta.ssid, ssid, n) != 0) return false;
    if (n < sizeof(cur.sta.ssid) && cur.sta.ssid[n] != '\0') return false;
    const char *pw = pass ? pass : "";
    size_t pn = strnlen(pw, sizeof(cur.sta.password));
    if (memcmp(cur.sta.password, pw, pn) != 0) return false;
    if (pn < sizeof(cur.sta.password) && cur.sta.password[pn] != '\0') return false;
    return true;
}

static bool settings_have_wifi(const app_settings_t *s)
{
    return s && app_wifi_first_slot(s->wifi) >= 0;
}

static int active_slot_index(const app_wifi_cred_t slots[APP_WIFI_SLOTS])
{
    if (!slots) return -1;
    if (s_active_slot >= 0 && s_active_slot < APP_WIFI_SLOTS
        && app_wifi_slot_filled(&slots[s_active_slot])) {
        return s_active_slot;
    }
    return app_wifi_first_slot(slots);
}

static app_wifi_cred_t s_wifi_cache[APP_WIFI_SLOTS];
static bool s_wifi_cache_valid;

static bool load_wifi_slots(app_wifi_cred_t slots[APP_WIFI_SLOTS])
{
    if (!slots) return false;
    app_settings_t *tmp = calloc(1, sizeof(*tmp));
    if (!tmp) {
        if (s_wifi_cache_valid) {
            memcpy(slots, s_wifi_cache, sizeof(s_wifi_cache));
            return app_wifi_first_slot(slots) >= 0;
        }
        memset(slots, 0, sizeof(app_wifi_cred_t) * APP_WIFI_SLOTS);
        return false;
    }
    (void)app_store_load_settings(tmp);
    memcpy(slots, tmp->wifi, sizeof(tmp->wifi));
    memcpy(s_wifi_cache, tmp->wifi, sizeof(tmp->wifi));
    s_wifi_cache_valid = true;
    bool have = tmp->has_wifi;
    free(tmp);
    return have;
}

/** Point the driver at one saved slot. Safe while STA is disconnected. */
static void install_sta_slot(const app_settings_t *s, int idx)
{
    if (!s || idx < 0 || idx >= APP_WIFI_SLOTS || !app_wifi_slot_filled(&s->wifi[idx])) return;
    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, s->wifi[idx].ssid, sizeof(sta.sta.ssid));
    strncpy((char *)sta.sta.password, s->wifi[idx].pass, sizeof(sta.sta.password));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set STA slot %d: %s", idx + 1, esp_err_to_name(err));
        s_slot_reload = true;
        return;
    }
    s_active_slot = idx;
    s_slot_reload = !sta_cred_is(s->wifi[idx].ssid, s->wifi[idx].pass);
    if (s_slot_reload) {
        ESP_LOGW(TAG, "STA slot %d profile did not stick", idx + 1);
    } else {
        ESP_LOGI(TAG, "STA profile slot %d ssid_len=%u",
                 idx + 1, (unsigned)strlen(s->wifi[idx].ssid));
    }
}

static void persist_wifi_pref_if_dirty(void)
{
    int slot = s_pref_dirty;
    if (slot < 0 || slot >= APP_WIFI_SLOTS) return;
    if (app_store_save_wifi_pref(slot) == ESP_OK) {
        if (s_pref_dirty == slot) s_pref_dirty = -1;
    }
}

static bool scan_recent(void)
{
    if (s_last_scan_tick == 0) return false;
    return (xTaskGetTickCount() - s_last_scan_tick) < pdMS_TO_TICKS(2000);
}

static bool reason_ap_absent(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return true;
    default:
        return false;
    }
}

/** Drop a scan result the driver is still holding. */
static void sta_scan_release(void)
{
    uint16_t zero = 0;
    wifi_ap_record_t discard;
    (void)esp_wifi_scan_get_ap_records(&zero, &discard);
}

/**
 * Blocking scan. Call only off the Wi-Fi event task: SCAN_DONE is delivered
 * on that task, so a blocking scan there deadlocks.
 * Returns 1 on a finished scan, 0 when the scan did not run (caller may try
 * one slot), and -1 when the radio is busy (caller must not connect).
 */
static int sta_collect_scan(void)
{
    wifi_scan_config_t cfg = { 0 };
    cfg.show_hidden = false;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    s_visible_n = 0;
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err == ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "STA scan busy: %s", esp_err_to_name(err));
        return -1;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA scan start: %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK) {
        sta_scan_release();
        ESP_LOGW(TAG, "STA scan ap_num failed");
        return 0;
    }
    if (num == 0) {
        sta_scan_release();
        s_last_scan_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "STA scan saw 0 APs");
        return 1;
    }
    /* s_scan_recs is BSS so a scan during httpd does not need a heap block. */
    uint16_t cap = num > STA_SCAN_MAX ? STA_SCAN_MAX : num;
    uint16_t got = cap;
    err = esp_wifi_scan_get_ap_records(&got, s_scan_recs);
    if (err != ESP_OK) {
        sta_scan_release();
        ESP_LOGW(TAG, "STA scan records: %s", esp_err_to_name(err));
        return 0;
    }
    for (uint16_t i = 0; i < got && s_visible_n < STA_SCAN_MAX; i++) {
        if (s_scan_recs[i].ssid[0] == '\0') continue;
        memcpy(s_visible[s_visible_n], s_scan_recs[i].ssid, APP_WIFI_SSID_MAX - 1);
        s_visible[s_visible_n][APP_WIFI_SSID_MAX - 1] = '\0';
        bool dup = false;
        for (int j = 0; j < s_visible_n; j++) {
            if (strncmp(s_visible[j], s_visible[s_visible_n], APP_WIFI_SSID_MAX) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) s_visible_n++;
    }
    s_last_scan_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "STA scan aps=%u unique=%d", (unsigned)num, s_visible_n);
    return 1;
}

static void publish_try_order(const app_wifi_cred_t slots[APP_WIFI_SLOTS])
{
    const char *ptrs[STA_SCAN_MAX];
    int nvis = s_visible_n;
    if (nvis > STA_SCAN_MAX) nvis = STA_SCAN_MAX;
    for (int i = 0; i < nvis; i++) ptrs[i] = s_visible[i];
    s_try_n = app_wifi_visible_try_order(slots, ptrs, nvis, s_pref_slot, s_try_order);
    s_try_valid = true;
    ESP_LOGI(TAG, "visible order n=%d slots=%d/%d/%d pref=%d",
             s_try_n,
             s_try_n > 0 ? s_try_order[0] + 1 : 0,
             s_try_n > 1 ? s_try_order[1] + 1 : 0,
             s_try_n > 2 ? s_try_order[2] + 1 : 0,
             s_pref_slot >= 0 ? s_pref_slot + 1 : 0);
}

static int fallback_slot(const app_wifi_cred_t slots[APP_WIFI_SLOTS])
{
    if (s_pref_slot >= 0 && s_pref_slot < APP_WIFI_SLOTS
        && app_wifi_slot_filled(&slots[s_pref_slot])) {
        return s_pref_slot;
    }
    return app_wifi_first_slot(slots);
}

static void connect_installed_slot(const app_settings_t *settings, int slot, const char *why)
{
    install_sta_slot(settings, slot);
    if (s_stopping_for_sleep || s_state == APP_NET_STA_CONNECTED) return;
    esp_err_t cerr = esp_wifi_connect();
    ESP_LOGI(TAG, "%s connect slot=%d err=%s",
             why ? why : "sta", slot + 1, esp_err_to_name(cerr));
    if (!s_cred_gave_up && s_state != APP_NET_STA_CONNECTED) {
        set_state(APP_NET_STA_CONNECTING);
    }
}

/**
 * No saved SSID was in the scan. Do not esp_wifi_connect(): that would burn
 * a join timeout per invisible network. Cred-apply fails immediately. Other
 * searches take the SoftAP path after STA_EMPTY_SCAN_MAX empty scans.
 */
static void none_visible_path(const char *why)
{
    s_need_scan = true;
    s_try_valid = true;
    s_try_n = 0;
    s_slot_reload = false;
    s_slot_fails = 0;
    ESP_LOGI(TAG, "no saved SSID in scan (%s)", why ? why : "?");
    if (s_cred_gave_up) return;
    if (s_cred_watch) {
        s_cred_watch = false;
        s_cred_gave_up = true;
        set_state(APP_NET_STA_FAILED);
        if (!(s_softap_portal && wifi_mode_has_ap())) schedule_softap_for_outage();
        ESP_LOGW(TAG, "saved networks not visible — join failed, SoftAP for reconfig");
        return;
    }
    s_empty_scans++;
    if (s_empty_scans >= STA_EMPTY_SCAN_MAX) {
        s_empty_scans = 0;
        s_retry = 0;
        if (s_state != APP_NET_STA_FAILED) {
            set_state(APP_NET_STA_FAILED);
            ESP_LOGW(TAG, "no visible saved SSID — SoftAP reconfig path");
        }
        schedule_softap_for_outage();
        return;
    }
    if (s_state != APP_NET_STA_CONNECTED) set_state(APP_NET_STA_CONNECTING);
    /* SoftAP keepalive paces the next scan. STA-only schedules a short rescan. */
    if (!(s_softap_portal && wifi_mode_has_ap())) {
        schedule_scan_pick(STA_EMPTY_RESCAN_MS);
    }
}

/**
 * Scan, intersect with saved slots, connect the first ordered match.
 * `from_apply` is the post-save path, which must not abort on s_sta_apply_busy.
 */
static void sta_pick_and_connect(const app_wifi_cred_t slots[APP_WIFI_SLOTS],
                                 const char *why, bool from_apply)
{
    if (!slots || s_stopping_for_sleep) return;
    if (s_state == APP_NET_STA_CONNECTED) return;
    if (s_pick_busy) return;
    if (!from_apply && s_sta_apply_busy) return;

    s_pick_busy = true;
    persist_wifi_pref_if_dirty();

    app_settings_t *settings = calloc(1, sizeof(*settings));
    if (!settings) {
        ESP_LOGW(TAG, "pick alloc failed (%s)", why ? why : "?");
        s_pick_busy = false;
        return;
    }
    memcpy(settings->wifi, slots, sizeof(settings->wifi));

    int scanned = sta_collect_scan();
    if (!from_apply && (s_sta_apply_busy || s_stopping_for_sleep
                        || s_state == APP_NET_STA_CONNECTED)) {
        free(settings);
        s_pick_busy = false;
        return;
    }
    if (scanned < 0) {
        free(settings);
        s_pick_busy = false;
        return;
    }
    if (scanned == 0) {
        int slot = fallback_slot(slots);
        if (slot < 0) {
            free(settings);
            s_pick_busy = false;
            none_visible_path(why);
            return;
        }
        s_try_order[0] = slot;
        s_try_order[1] = -1;
        s_try_order[2] = -1;
        s_try_n = 1;
        s_try_valid = true;
        s_need_scan = false;
        s_slot_fails = 0;
        ESP_LOGW(TAG, "scan failed — try slot %d only (%s)", slot + 1, why ? why : "?");
        connect_installed_slot(settings, slot, why);
        free(settings);
        s_pick_busy = false;
        return;
    }

    publish_try_order(slots);
    if (s_try_n <= 0) {
        free(settings);
        s_pick_busy = false;
        none_visible_path(why);
        return;
    }

    s_empty_scans = 0;
    s_need_scan = false;
    s_slot_fails = 0;
    connect_installed_slot(settings, s_try_order[0], why);
    free(settings);
    s_pick_busy = false;
}

static void scan_pick_task(void *arg)
{
    (void)arg;
    int delay = s_scan_delay_ms;
    if (delay > 0) vTaskDelay(pdMS_TO_TICKS(delay));
    if (!s_stopping_for_sleep && !s_sta_apply_busy && s_state != APP_NET_STA_CONNECTED) {
        app_wifi_cred_t slots[APP_WIFI_SLOTS];
        if (load_wifi_slots(slots)) sta_pick_and_connect(slots, "scan", false);
    }
    s_scan_task_busy = false;
    if (s_pending_rescan_ms >= 0 && !s_stopping_for_sleep && !s_sta_apply_busy
        && s_state != APP_NET_STA_CONNECTED) {
        int next = s_pending_rescan_ms;
        s_pending_rescan_ms = -1;
        schedule_scan_pick(next);
    }
    vTaskDelete(NULL);
}

static void schedule_scan_pick(int delay_ms)
{
    if (delay_ms < 0) delay_ms = 0;
    if (s_stopping_for_sleep) return;
    if (s_scan_task_busy) {
        if (s_pending_rescan_ms < 0 || delay_ms < s_pending_rescan_ms) {
            s_pending_rescan_ms = delay_ms;
        }
        return;
    }
    s_scan_delay_ms = delay_ms;
    s_pending_rescan_ms = -1;
    s_scan_task_busy = true;
    if (xTaskCreate(scan_pick_task, "sta_scan", 4096, NULL, 5, NULL) != pdPASS) {
        s_scan_task_busy = false;
        ESP_LOGW(TAG, "scan task failed");
        if (s_try_valid && s_try_n > 0 && !s_need_scan) {
            esp_wifi_connect();
        } else if (!(s_softap_portal && wifi_mode_has_ap())) {
            schedule_softap_for_outage();
        }
    }
}

/**
 * Count a join failure against the active visible slot.
 * Stays on that slot until CRED_JOIN_FAIL_MAX (a short flap retries it).
 * An absent-AP reason, or a used-up budget, moves to the next slot that was
 * in the same scan. When that list is done, the next attempt scans again
 * instead of joining saved SSIDs the scan did not see.
 * Does not call Wi-Fi APIs — safe on the event task.
 */
static void note_slot_join_fail(const app_wifi_cred_t slots[APP_WIFI_SLOTS], uint8_t reason)
{
    if (!slots || !join_fail_reason(reason)) return;
    int idx = active_slot_index(slots);
    if (idx < 0) return;
    s_active_slot = idx;

    if (!s_try_valid || s_need_scan) {
        s_need_scan = true;
        s_slot_reload = true;
        ESP_LOGI(TAG, "STA join fail before a fresh scan slot=%d reason=%u",
                 idx + 1, reason);
        return;
    }

    if (!reason_ap_absent(reason) && (s_slot_fails + 1) < CRED_JOIN_FAIL_MAX) {
        s_slot_fails++;
        ESP_LOGI(TAG, "STA join fail slot %d %d/%d reason=%u",
                 idx + 1, s_slot_fails, CRED_JOIN_FAIL_MAX, reason);
        return;
    }

    int next = app_wifi_order_after(s_try_order, s_try_n, idx);
    s_slot_fails = 0;
    if (next >= 0) {
        s_active_slot = next;
        s_slot_reload = true;
        s_need_scan = false;
        ESP_LOGI(TAG, "STA join slot %d -> visible slot %d reason=%u",
                 idx + 1, next + 1, reason);
        return;
    }

    s_need_scan = true;
    s_try_valid = false;
    s_slot_reload = true;
    ESP_LOGI(TAG, "STA visible list exhausted from slot %d reason=%u", idx + 1, reason);
    if (s_cred_watch) {
        s_cred_watch = false;
        s_cred_gave_up = true;
    }
}

/** set_config off the Wi-Fi event task, then connect the visible slot note_slot picked. */
static volatile bool s_slot_task_busy;

static void slot_switch_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_stopping_for_sleep && !s_sta_apply_busy && !s_pick_busy && !s_need_scan
        && s_slot_reload && s_state != APP_NET_STA_CONNECTED) {
        app_settings_t s;
        memset(&s, 0, sizeof(s));
        (void)app_store_load_settings(&s);
        install_sta_slot(&s, active_slot_index(s.wifi));
        if (!s_sta_apply_busy && !s_stopping_for_sleep && !s_need_scan
            && s_state != APP_NET_STA_CONNECTED) {
            esp_err_t cerr = esp_wifi_connect();
            ESP_LOGI(TAG, "slot switch connect slot=%d err=%s",
                     s_active_slot + 1, esp_err_to_name(cerr));
        }
    }
    s_slot_task_busy = false;
    vTaskDelete(NULL);
}

static void schedule_slot_switch(void)
{
    if (s_stopping_for_sleep) return;
    if (s_slot_task_busy) return;
    s_slot_task_busy = true;
    if (xTaskCreate(slot_switch_task, "sta_slot", 4096, NULL, 5, NULL) != pdPASS) {
        s_slot_task_busy = false;
        /*
         * A missing later attempt stops STA recovery until wake. Connect only
         * when the current profile is a slot the scan actually saw.
         */
        if (s_try_valid && s_try_n > 0 && !s_need_scan) {
            esp_wifi_connect();
            ESP_LOGW(TAG, "slot switch task failed — connect kept scanned profile");
        } else {
            schedule_scan_pick(0);
            ESP_LOGW(TAG, "slot switch task failed — rescan scheduled");
        }
    }
}

static bool join_fail_reason(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return true;
    default:
        return false;
    }
}

/**
 * Install SSID/password and connect now.
 *
 * esp_wifi_set_config() during an in-flight association updates the stored
 * profile but esp_wifi_connect() returns ESP_ERR_WIFI_CONN and the current
 * attempt keeps the previous SSID. SoftAP keepalive only reloads the profile
 * when the mode is AP-only, and outage mode is already APSTA, so a save used
 * to leave 「重连中」 on the old network until deep-sleep boot reloaded NVS.
 * Disconnect first, while keepalive is stopped, then set_config + connect.
 * SoftAP stays up so a bad password can be fixed without another wake.
 */
static void apply_saved_sta(bool rebind_httpd)
{
    app_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    (void)app_store_load_settings(&settings);
    const int first = app_wifi_first_slot(settings.wifi);
    if (first < 0) {
        ensure_softap_portal();
        set_state(APP_NET_AP_MODE);
        return;
    }

    const bool from_sta_only = s_sta_only_radio;
    s_sta_apply_busy = true;
    s_cred_watch = false;
    s_cred_gave_up = false;
    s_slot_fails = 0;
    s_active_slot = -1;
    s_slot_reload = true;
    s_need_scan = true;
    s_try_valid = false;
    s_empty_scans = 0;
    s_pending_rescan_ms = -1;

    stop_sta_softap_keepalive();
    for (int i = 0; i < 40 && s_sta_ka_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_sta_ka_task) {
        ESP_LOGW(TAG, "STA keepalive still running during credential apply");
    }
    /* Let an in-flight scan finish and skip its connect before we retune. */
    for (int i = 0; i < 80 && (s_pick_busy || s_scan_task_busy); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    (void)esp_wifi_disconnect();
    /* Drain STA_DISCONNECTED while s_sta_apply_busy blocks a reconnect. */
    vTaskDelay(pdMS_TO_TICKS(250));

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "apply STA set_mode APSTA: %s", esp_err_to_name(err));
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "apply STA set AP: %s", esp_err_to_name(err));
    }
    /* Placeholder profile until the scan below chooses a visible slot. */
    install_sta_slot(&settings, first);

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "apply STA wifi_start: %s", esp_err_to_name(err));
    }

    s_softap_portal = true;
    s_sta_only_radio = false;
    ensure_softap_ip_dhcp();
    start_captive_dns();
    if (from_sta_only) s_httpd_needs_softap_rebind = true;
    /*
     * Rebind only off the httpd task. schedule_httpd_if_needed() can stop
     * the server, which is unsafe from inside the /save handler. A first
     * provision fetch window must also stay set so GOT_IP does not start httpd.
     */
    if (rebind_httpd && !s_fetch_pending
        && (from_sta_only || !s_httpd || s_httpd_needs_softap_rebind)) {
        schedule_httpd_if_needed("cred apply");
    }

    s_retry = 0;
    s_ip[0] = '\0';
    if (s_wifi_eg) {
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
    set_state(APP_NET_STA_CONNECTING);

    /*
     * Same scan order as wake: last success first if it is visible, otherwise
     * the other in-range saved names. Absent names are not joined, so a new
     * network saved after leaving the old one connects without those timeouts.
     */
    s_cred_watch = true;
    s_slot_fails = 0;
    vTaskDelay(pdMS_TO_TICKS(100));
    sta_pick_and_connect(settings.wifi, "apply", true);
    ESP_LOGI(TAG, "apply STA scan slot=%d softap=%d",
             s_active_slot + 1, (int)wifi_mode_has_ap());

    s_sta_apply_busy = false;
    if (s_sta_ka_task == NULL) {
        ensure_sta_softap_keepalive();
    } else {
        s_sta_ka_wanted = true;
    }
}

/**
 * While SoftAP is up for reconfig, keep STA searching saved networks that are
 * actually visible. A fresh scan runs when the candidate list is stale.
 * Runs off the wifi event task; paces attempts so SoftAP beacons stay up.
 */
static void sta_softap_keepalive_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "STA SoftAP keepalive on (every %d ms)", STA_SOFTAP_RECONNECT_MS);
    while (s_sta_ka_wanted && !s_stopping_for_sleep) {
        if (s_state == APP_NET_STA_CONNECTED) {
            break;
        }
        if (s_sta_apply_busy) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        app_settings_t s;
        memset(&s, 0, sizeof(s));
        (void)app_store_load_settings(&s);
        if (!settings_have_wifi(&s)) {
            break;
        }

        /* SoftAP may briefly flap during set_mode — wait, don't exit. */
        if (!s_softap_portal && !wifi_mode_has_ap()) {
            vTaskDelay(pdMS_TO_TICKS(400));
            if (s_state == APP_NET_STA_CONNECTED || !s_sta_ka_wanted || s_stopping_for_sleep) {
                break;
            }
            if (!s_softap_portal && !wifi_mode_has_ap() && !s_softap_for_outage) {
                break;
            }
        }

        if (s_state != APP_NET_STA_CONNECTED && !s_stopping_for_sleep && !s_tls_busy
            && !s_sta_apply_busy && !s_pick_busy) {
            if (s_need_scan || !s_try_valid) {
                /* Just scanned (boot / previous pass): wait out the pace interval. */
                if (!scan_recent()) sta_pick_and_connect(s.wifi, "keepalive", false);
            } else {
            wifi_mode_t m = WIFI_MODE_NULL;
            const bool ap_only = esp_wifi_get_mode(&m) == ESP_OK && m == WIFI_MODE_AP;
            /*
             * APSTA keepalive used to call connect() only. After a save that
             * did not stick, that retried the previous SSID forever.
             */
            const int idx = active_slot_index(s.wifi);
            const bool profile_ok = idx >= 0
                && !s_slot_reload
                && sta_cred_is(s.wifi[idx].ssid, s.wifi[idx].pass);
            if (ap_only || !profile_ok) {
                if (!ap_only) {
                    ESP_LOGW(TAG, "keepalive: STA profile != slot %d — reload", idx + 1);
                    (void)esp_wifi_disconnect();
                    vTaskDelay(pdMS_TO_TICKS(150));
                    if (s_sta_apply_busy || !s_sta_ka_wanted || s_stopping_for_sleep) {
                        continue;
                    }
                } else {
                    ESP_LOGW(TAG, "keepalive: AP-only → APSTA for saved SSID scan");
                    (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
                }
                install_sta_slot(&s, idx);
                (void)esp_wifi_start();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (s_sta_apply_busy) continue;
            esp_err_t cerr = esp_wifi_connect();
            ESP_LOGI(TAG, "SoftAP keepalive STA reconnect slot=%d: %s (ap=%d)",
                     active_slot_index(s.wifi) + 1, esp_err_to_name(cerr), (int)wifi_mode_has_ap());
            /* Leave the bad-credential screen up; searching continues below. */
            if (!s_cred_gave_up && s_state != APP_NET_STA_CONNECTED) {
                set_state(APP_NET_STA_CONNECTING);
            }
            }
        }

        for (int i = 0; i < (STA_SOFTAP_RECONNECT_MS / 250) && s_sta_ka_wanted; i++) {
            if (s_state == APP_NET_STA_CONNECTED || s_stopping_for_sleep) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    ESP_LOGI(TAG, "STA SoftAP keepalive exit state=%d wanted=%d",
             (int)s_state, (int)s_sta_ka_wanted);
    s_sta_ka_task = NULL;
    vTaskDelete(NULL);
}

static void ensure_sta_softap_keepalive(void)
{
    if (s_stopping_for_sleep || s_sta_apply_busy) return;
    app_settings_t s;
    memset(&s, 0, sizeof(s));
    (void)app_store_load_settings(&s);
    if (!settings_have_wifi(&s)) return;

    s_sta_ka_wanted = true;
    if (s_sta_ka_task) return;
    if (xTaskCreate(sta_softap_keepalive_task, "sta_ka", 6144, NULL, 5, &s_sta_ka_task) != pdPASS) {
        s_sta_ka_task = NULL;
        s_sta_ka_wanted = false;
        ESP_LOGW(TAG, "STA SoftAP keepalive task create failed");
    }
}

static void stop_sta_softap_keepalive(void)
{
    s_sta_ka_wanted = false;
}

/**
 * SoftAP restore + optional STA retry — always off the wifi event task.
 * Debounce ~3s so brief Wi-Fi blips do not flap SoftAP.
 */
static void portal_outage_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 30; i++) {
        if (s_stopping_for_sleep || s_state == APP_NET_STA_CONNECTED) {
            ESP_LOGI(TAG, "SoftAP outage task cancelled (state=%d)", (int)s_state);
            s_softap_for_outage = false;
            s_portal_task = NULL;
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_stopping_for_sleep && s_state != APP_NET_STA_CONNECTED) {
        s_fetch_pending = false;
        ensure_softap_portal();
        if (!s_softap_portal || !wifi_mode_has_ap() || !s_httpd) {
            ESP_LOGW(TAG, "outage portal incomplete softap=%d http=%d — retry",
                     (int)(s_softap_portal && wifi_mode_has_ap()), s_httpd != NULL);
            vTaskDelay(pdMS_TO_TICKS(300));
            ensure_softap_portal();
        }
        app_settings_t s;
        memset(&s, 0, sizeof(s));
        (void)app_store_load_settings(&s);
        if (settings_have_wifi(&s) && s_state != APP_NET_STA_CONNECTED) {
            s_retry = 0;
            /* SoftAP stays up for reconfig; keepalive keeps scanning home SSID.
             * Do not clear a config-save failure latch or restart its UI. */
            ensure_sta_softap_keepalive();
            ESP_LOGI(TAG, "outage SoftAP up=%d; STA keepalive armed",
                     (int)(s_softap_portal && wifi_mode_has_ap()));
            if (!s_cred_gave_up && !s_sta_apply_busy) {
                set_state(APP_NET_STA_CONNECTING);
            }
        }
    }
    s_portal_task = NULL;
    vTaskDelete(NULL);
}

static void schedule_softap_for_outage(void)
{
    if (s_stopping_for_sleep || s_tls_busy) return;

    const bool ap_on = wifi_mode_has_ap();
    if (s_softap_portal && !ap_on) {
        ESP_LOGW(TAG, "SoftAP flag set but mode has no AP — force restore");
        s_softap_portal = false;
    }
    /* Already broadcasting + httpd — do not flap. */
    if (s_softap_portal && ap_on && s_httpd) return;
    if (s_portal_task) return;
    s_softap_for_outage = true;
    if (xTaskCreate(portal_outage_task, "ap_outage", 6144, NULL, 5, &s_portal_task) != pdPASS) {
        s_portal_task = NULL;
        s_softap_for_outage = false;
        ESP_LOGW(TAG, "portal task create failed — will retry next disconnect");
    }
}

/**
 * GOT_IP follow-up off the IP event stack: drop SoftAP (STA-only) + httpd.
 * set_mode on the event task is unsafe and also raced SoftAP outage restore.
 */
static void got_ip_followup_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(50));
    persist_wifi_pref_if_dirty();
    if (s_stopping_for_sleep) {
        vTaskDelete(NULL);
        return;
    }
    if (s_state == APP_NET_STA_CONNECTED) {
        stop_sta_softap_keepalive();
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GOT_IP set STA-only: %s", esp_err_to_name(err));
        } else {
            s_softap_portal = false;
            s_sta_only_radio = true;
            s_softap_for_outage = false;
            /* Next SoftAP restore must restart httpd (LAN sockets ≠ SoftAP). */
            s_httpd_needs_softap_rebind = true;
            stop_captive_dns();
            ESP_LOGI(TAG, "GOT_IP follow-up — SoftAP dropped (STA-only)");
        }
    }
    if (s_tls_busy || s_fetch_pending) {
        stop_httpd();
        ESP_LOGI(TAG, "GOT_IP follow-up — defer httpd (tls/fetch)");
    } else {
        /* LAN config page on STA IP; SoftAP is down. */
        (void)restart_httpd();
        /* Keep needs_softap_rebind=true so a later outage SoftAP rebinds. */
        ESP_LOGI(TAG, "GOT_IP follow-up — httpd for LAN config ip=%s",
                 s_ip[0] ? s_ip : "?");
    }
    vTaskDelete(NULL);
}

/** SoftAP came up without httpd (race) — start server off the event stack. */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_STA_START) {
        /* connect() from app_net_start / save_followup / portal task */
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        uint8_t reason = 0;
        if (data) {
            reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        }
        if (s_stopping_for_sleep) {
            ESP_LOGI(TAG, "STA disconnect ignored (stopping for sleep)");
            return;
        }
        /* apply_saved_sta() owns this disconnect; do not reconnect the old SSID. */
        if (s_sta_apply_busy || s_pick_busy) {
            s_ip[0] = '\0';
            ESP_LOGI(TAG, "STA disconnect during %s (reason=%u)",
                     s_sta_apply_busy ? "credential apply" : "scan pick", reason);
            return;
        }

        s_ip[0] = '\0';
        s_disc_streak++;

        app_wifi_cred_t disc_slots[APP_WIFI_SLOTS];
        const bool have_wifi = load_wifi_slots(disc_slots);

        /* Unconfigured SoftAP boot: ignore stray STA disconnects — no work. */
        if (!have_wifi) {
            s_retry = 0;
            if (s_state != APP_NET_AP_MODE) set_state(APP_NET_AP_MODE);
            return;
        }

        if (s_tls_busy) {
            ESP_LOGW(TAG, "STA lost during TLS — SoftAP after restore_httpd");
        }

        /* Budget counts only the slot we are actually joining. Invisible saved
         * SSIDs are not tried. The failure popup latches when a credential
         * apply has exhausted the visible list (or the scan showed none). */
        note_slot_join_fail(disc_slots, reason);

        /*
         * SoftAP already broadcasting: keep searching home SSID via keepalive
         * task (paced). Do NOT skip connect() on this event without a deferred
         * attempt — that deadlocked STA recovery in v0.15–v0.17.
         * After a bad save, leave STA_FAILED on screen but keep the keepalive.
         */
        if (s_softap_portal && wifi_mode_has_ap()) {
            /* SoftAP visible but config page dead if httpd never came back. */
            if (!s_tls_busy && (!s_httpd || s_httpd_needs_softap_rebind)) {
                schedule_httpd_if_needed("STA_DISC softap-up");
            }
            ensure_sta_softap_keepalive();
            if (s_cred_gave_up) {
                if (s_state != APP_NET_STA_FAILED) {
                    set_state(APP_NET_STA_FAILED);
                    ESP_LOGW(TAG, "new Wi-Fi join failed — SoftAP stays for reconfig");
                }
            } else if (s_state != APP_NET_STA_CONNECTED) {
                set_state(APP_NET_STA_CONNECTING);
            }
            ESP_LOGI(TAG, "SoftAP up — STA keepalive (streak=%d http=%d reason=%u)",
                     s_disc_streak, s_httpd != NULL, reason);
            return;
        }

        /*
         * Event task must stay light: only esp_wifi_connect + flags.
         * SoftAP / set_mode / httpd happen in portal_outage_task after fail wave.
         */
        if (s_retry < STA_RETRY_MAX && !s_cred_gave_up) {
            s_retry++;
            if (s_need_scan || !s_try_valid) {
                schedule_scan_pick(0);
            } else if (s_slot_reload) {
                schedule_slot_switch();
            } else {
                esp_wifi_connect();
            }
            set_state(APP_NET_STA_CONNECTING);
            ESP_LOGI(TAG, "STA retry %d/%d slot=%d (streak=%d reason=%u)",
                     s_retry, STA_RETRY_MAX, s_active_slot + 1, s_disc_streak, reason);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            if (!s_tls_busy && !s_cred_gave_up) s_fetch_pending = false;
            if (s_cred_watch) {
                s_cred_watch = false;
                s_cred_gave_up = true;
            }
            set_state(APP_NET_STA_FAILED);
            /* Deferred SoftAP once; keep STA reconnect light on this event task. */
            schedule_softap_for_outage();
            s_retry = 0;
            /* Do not storm-connect here — portal task will reconnect gently. */
            ESP_LOGW(TAG, "STA failed wave — SoftAP scheduled, keep STA searching");
        }
    } else if (id == WIFI_EVENT_AP_START) {
        s_softap_portal = true;
        s_sta_only_radio = false;
        if (s_state == APP_NET_IDLE || s_state == APP_NET_AP_MODE) {
            set_state(APP_NET_AP_MODE);
        }
        /* DNS is small; httpd_start is NOT safe here (2.3KB event stack). */
        start_captive_dns();
        /* SoftAP radio up — ensure :80 (missing or needs SoftAP rebind). */
        if (!s_httpd || s_httpd_needs_softap_rebind) {
            schedule_httpd_if_needed("AP_START");
        }
    } else if (id == WIFI_EVENT_AP_STOP) {
        s_softap_portal = false;
        stop_captive_dns();
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "phone joined SoftAP AID=%d http=%d dns=%d",
                 e ? e->aid : -1, s_httpd != NULL, s_dns != NULL);
        /* Never httpd_start on this stack — defer. Force SoftAP :80 rebind. */
        if (!s_tls_busy) {
            start_captive_dns();
            s_httpd_needs_softap_rebind = true;
            schedule_httpd_if_needed("AP_STACONNECTED");
        }
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        s_disc_streak = 0;
        s_softap_for_outage = false;
        s_cred_watch = false;
        s_cred_gave_up = false;
        s_slot_fails = 0;
        s_slot_reload = false;
        s_empty_scans = 0;
        s_need_scan = false;
        if (s_active_slot >= 0 && s_active_slot < APP_WIFI_SLOTS
            && s_active_slot != s_pref_slot) {
            s_pref_slot = s_active_slot;
            s_pref_dirty = s_active_slot;
        }
        stop_sta_softap_keepalive();
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        set_state(APP_NET_STA_CONNECTED);
        ESP_LOGI(TAG, "got ip %s", s_ip);
        /* set_mode(STA) + httpd off this stack — see got_ip_followup_task. */
        if (xTaskCreate(got_ip_followup_task, "ip_radio", 6144, NULL, 5, NULL) != pdPASS) {
            ESP_LOGW(TAG, "GOT_IP: follow-up task failed");
        }
    }
}

esp_err_t app_net_start(app_net_cb_t cb)
{
    s_cb = cb;
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip, NULL));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "AIPassport-%02X%02X", mac[4], mac[5]);

    memset(&s_ap_cfg, 0, sizeof(s_ap_cfg));
    strncpy((char *)s_ap_cfg.ap.ssid, s_ap_ssid, sizeof(s_ap_cfg.ap.ssid));
    s_ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    s_ap_cfg.ap.channel = 6;
    s_ap_cfg.ap.max_connection = 4;
    s_ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    /* Quiet captive-probe noise in logs. */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    app_settings_t settings;
    app_store_load_settings(&settings);
    s_pref_slot = app_store_load_wifi_pref();

    if (!s_httpd_mu) {
        s_httpd_mu = xSemaphoreCreateMutex();
    }

    if (settings_have_wifi(&settings)) {
        const int boot_slot = app_wifi_first_slot(settings.wifi);
        s_active_slot = -1;
        s_slot_fails = 0;
        s_slot_reload = true;
        s_need_scan = true;
        s_try_valid = false;
        s_empty_scans = 0;
        wifi_config_t sta = { 0 };
        strncpy((char *)sta.sta.ssid, settings.wifi[boot_slot].ssid, sizeof(sta.sta.ssid));
        strncpy((char *)sta.sta.password, settings.wifi[boot_slot].pass, sizeof(sta.sta.password));
        /*
         * Boot APSTA + SoftAP portal + httpd on THIS app task (not the wifi
         * event task). v0.14–v0.16 booted STA-only and only raised SoftAP after
         * a fail wave; that left freshly-flashed devices with a visible hotspot
         * but a dead config page when NVS still had Wi-Fi, and made outage
         * restore fragile. GOT_IP follow-up still drops SoftAP → STA-only and
         * rebinds LAN httpd (scenarios 2 and 5).
         */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_start());
        vTaskDelay(pdMS_TO_TICKS(150));
        s_softap_portal = true;
        s_sta_only_radio = false;
        ensure_softap_ip_dhcp();
        start_captive_dns();
        for (int i = 0; i < 3; i++) {
            if (start_httpd() == ESP_OK) break;
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        s_httpd_needs_softap_rebind = false;
        set_state(APP_NET_STA_CONNECTING);
        /* Scan on this app task, then join the last-success slot if it is visible. */
        sta_pick_and_connect(settings.wifi, "boot", false);
        /* SoftAP is already up at boot — arm keepalive so a missed scan
         * cannot stall STA forever (event-task throttle bug). */
        ensure_sta_softap_keepalive();
        ESP_LOGI(TAG, "boot APSTA slot=%d pref=%d softap=%s http=%d dns=%d",
                 s_active_slot + 1, s_pref_slot + 1, s_ap_ssid,
                 s_httpd != NULL, s_dns != NULL);
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        /* Let AP_START settle on event task before httpd on THIS (app) task. */
        vTaskDelay(pdMS_TO_TICKS(150));
        s_softap_portal = true;
        s_sta_only_radio = false;
        ensure_softap_ip_dhcp();
        start_captive_dns();
        for (int i = 0; i < 3; i++) {
            if (start_httpd() == ESP_OK) break;
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        s_httpd_needs_softap_rebind = false;
        set_state(APP_NET_AP_MODE);
        ESP_LOGI(TAG, "provision SoftAP ssid=%s url=http://192.168.4.1/ http=%d dns=%d",
                 s_ap_ssid, s_httpd != NULL, s_dns != NULL);
    }
    return ESP_OK;
}

esp_err_t app_net_wait_sta_ip(int timeout_ms)
{
    const int step = 100;
    int waited = 0;
    if (timeout_ms < 0) timeout_ms = 0;
    while (waited <= timeout_ms) {
        if (s_state == APP_NET_STA_CONNECTED && s_ip[0] != '\0') {
            /* Prefer STA-only radio before HTTPS (GOT_IP normally drops SoftAP). */
            if (s_softap_portal && !s_tls_busy) {
                esp_wifi_set_mode(WIFI_MODE_STA);
                s_softap_portal = false;
                s_sta_only_radio = true;
                stop_captive_dns();
            }
            return ESP_OK;
        }
        /* Bad-credential latch still has keepalive searching — wait for IP. */
        if (s_state == APP_NET_AP_MODE
            || (s_state == APP_NET_STA_FAILED && !s_cred_gave_up)) {
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    return ESP_ERR_TIMEOUT;
}

void app_net_begin_fetch_window(void)
{
    /* Flag only — may run on httpd task inside /save; do not httpd_stop here. */
    s_fetch_pending = true;
    ESP_LOGI(TAG, "fetch window armed (GOT_IP will skip httpd; worker stops it)");
}

void app_net_prepare_for_tls(void)
{
    s_tls_busy = true;
    s_fetch_pending = true;
    ESP_LOGI(TAG, "prepare_for_tls heap free=%u largest=%u state=%d",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (int)s_state);
    stop_httpd();
    /*
     * Drop SoftAP if STA is up — APSTA + httpd starve TLS on no-PSRAM C3.
     * Never switch to STA-only when unconfigured / SoftAP-only, or the user
     * loses the only config path (192.168.4.1).
     */
    if (s_state == APP_NET_STA_CONNECTED || s_ip[0] != '\0') {
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (mode_err != ESP_OK) {
            ESP_LOGW(TAG, "set STA-only: %s", esp_err_to_name(mode_err));
        }
        s_softap_portal = false;
        s_sta_only_radio = true;
        stop_captive_dns();
    } else {
        ESP_LOGW(TAG, "prepare_for_tls: keep SoftAP (STA not up)");
    }
    /* Give Wi-Fi / LWIP a moment to release SoftAP buffers. */
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "after prepare free=%u largest=%u http=%d softap=%d",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (int)(s_httpd != NULL), (int)s_softap_portal);
}

void app_net_restore_httpd(void)
{
    s_tls_busy = false;
    s_fetch_pending = false;

    app_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    (void)app_store_load_settings(&settings);

    /* Unconfigured: SoftAP portal is the only way in — never LAN-only. */
    if (!settings_have_wifi(&settings)) {
        ESP_LOGW(TAG, "restore: no Wi-Fi creds — SoftAP portal");
        ensure_softap_portal();
        if (s_state != APP_NET_AP_MODE) set_state(APP_NET_AP_MODE);
        return;
    }

    if (s_state == APP_NET_STA_CONNECTED) {
        /* Restart so sockets rebind after SoftAP/httpd teardown for TLS. */
        esp_err_t err = restart_httpd();
        ESP_LOGI(TAG, "httpd restored (STA ip=%s) err=%s free=%u",
                 s_ip[0] ? s_ip : "?", esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size());
        if (err != ESP_OK) {
            /* Never leave the device without a config path. */
            ESP_LOGE(TAG, "STA httpd failed; falling back to SoftAP portal");
            ensure_softap_portal();
        }
        return;
    }
    /* STA lost during/after TLS: SoftAP for reconfig + keep STA trying. */
    ESP_LOGW(TAG, "restore after TLS but STA not up (state=%d); SoftAP+APSTA", (int)s_state);
    ensure_softap_portal();
    if (settings_have_wifi(&settings) && s_state != APP_NET_AP_MODE) {
        ensure_apsta_keepalive();
        ensure_sta_softap_keepalive();
        set_state(APP_NET_STA_CONNECTING);
    }
}

void app_net_stop_for_sleep(void)
{
    ESP_LOGI(TAG, "stop radio for deep sleep (http=%d softap=%d)",
             (int)(s_httpd != NULL), (int)s_softap_portal);
    /* Must be first: STA_DISCONNECTED must not call ensure_softap / wifi_start. */
    s_stopping_for_sleep = true;
    persist_wifi_pref_if_dirty();
    s_tls_busy = false;
    s_fetch_pending = false;
    s_softap_for_outage = false;
    stop_sta_softap_keepalive();
    stop_httpd();
    stop_captive_dns();
    esp_wifi_disconnect();
    /* Let the ignored disconnect event drain before wifi_stop. */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
    s_softap_portal = false;
    s_sta_only_radio = false;
    s_ip[0] = '\0';
    s_state = APP_NET_IDLE;
    vTaskDelay(pdMS_TO_TICKS(100));
}
