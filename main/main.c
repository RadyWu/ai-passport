// AI护照 · 解闷阅读（蹲厕短文）— 独立 UI，不复用 demo 菜单外壳。
#include "app_article.h"
#include "app_cooldown.h"
#include "app_deepseek.h"
#include "app_net.h"
#include "app_sleep.h"
#include "app_store.h"
#include "app_ui.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "esp_tls_errors.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>

static const char *TAG = "bathroom";

#define INPUT_QUEUE_DEPTH 8
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

typedef enum {
    WORK_NONE = 0,
    WORK_FETCH,
    WORK_SLEEP,
} work_cmd_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static TaskHandle_t s_worker;
static volatile bool s_input_ready;
static key_guard_t s_key_guard;
static app_settings_t s_settings;
static int64_t s_session_start_us;
static int64_t s_last_fetch_unix;
static volatile work_cmd_t s_work_cmd;
static volatile bool s_fetch_after_save;
static bool s_time_ok;
static bool s_sntp_started;
static int s_frozen_hour;
static int s_frozen_min;
static bool s_have_frozen_clock;
static app_net_state_t s_prev_net = APP_NET_IDLE;
static bool s_was_online;
static bool s_net_lost_modal_shown;

static const char *SAMPLE_ARTICLE =
    "欢迎使用蹲坑阅读。请连接设备热点，在配置页填写 DeepSeek API Key 和偏好提示词。上下按钮单击翻页，长按调节亮度。长按OK键获取新内容。双击OK按钮或十分钟无操作，进入深度睡眠，可按任意键唤醒。";

static void apply_article_text(const char *text)
{
    /* Paginate into UI static s_art — no extra ~5KB heap during/after TLS. */
    app_ui_set_article_text(text);
}

static void load_or_sample_article(void)
{
    char *buf = malloc(APP_ARTICLE_MAX_BYTES);
    if (!buf) {
        apply_article_text(SAMPLE_ARTICLE);
        return;
    }
    buf[0] = '\0';
    int64_t ts = 0;
    esp_err_t err = app_store_load_article(buf, APP_ARTICLE_MAX_BYTES, &ts);
    if (err == ESP_OK && buf[0]) {
        s_last_fetch_unix = ts;
        s_settings.last_fetch_unix = ts;
        ESP_LOGI(TAG, "loaded article from NVS (%u bytes)", (unsigned)strnlen(buf, APP_ARTICLE_MAX_BYTES));
        apply_article_text(buf);
        /* Resume reading position after deep-sleep wake. */
        unsigned pg = app_store_load_page();
        app_ui_set_page(pg);
        ESP_LOGI(TAG, "restored page %u", pg);
    } else {
        ESP_LOGI(TAG, "no stored article (%s), using sample", esp_err_to_name(err));
        apply_article_text(SAMPLE_ARTICLE);
    }
    free(buf);
}

static void on_net(app_net_state_t st, const char *ip)
{
    s_prev_net = st;
    app_ui_set_net_state(st, ip);

    if (st == APP_NET_STA_FAILED) {
        if (app_net_cred_join_gave_up()) {
            /* Dismiss 「正在更新内容」. Keep s_fetch_after_save so a late
             * GOT_IP can still pull the first article. Reconfig never armed it.
             * SoftAP + STA keepalive stay up for an immediate correction. */
            app_ui_show_wifi_join_failed();
            ESP_LOGW(TAG, "Wi-Fi join failed after config save");
        } else {
            /* Abort post-save fetch; SoftAP is scheduled by app_net (APSTA keeps STA). */
            s_fetch_after_save = false;
            /* Modal only after retry wave fails — skips brief Wi-Fi blips. */
            if (s_was_online && !s_net_lost_modal_shown) {
                s_net_lost_modal_shown = true;
                app_ui_show_net_lost();
                ESP_LOGW(TAG, "network lost mid-use (STA_FAILED after retries)");
            }
        }
    }

    if (st == APP_NET_STA_CONNECTED) {
        app_ui_clear_wifi_join_failed();
        s_was_online = true;
        s_net_lost_modal_shown = false;
        ESP_LOGI(TAG, "STA up ip=%s free_heap=%u",
                 ip ? ip : "?", (unsigned)esp_get_free_heap_size());
        setenv("TZ", "CST-8", 1);
        tzset();
        if (!s_sntp_started) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_setservername(1, "ntp.tencent.com");
            esp_sntp_setservername(2, "pool.ntp.org");
            esp_sntp_init();
            s_sntp_started = true;
            ESP_LOGI(TAG, "SNTP started (aliyun/tencent/pool)");
        }
        /* Config-save path: pull once as soon as STA is up (bypass long-press). */
        if (s_fetch_after_save && s_work_cmd == WORK_NONE) {
            s_fetch_after_save = false;
            s_work_cmd = WORK_FETCH;
            ESP_LOGI(TAG, "queue fetch after config save");
        } else if (s_fetch_after_save && s_work_cmd != WORK_NONE) {
            ESP_LOGW(TAG, "GOT_IP fetch deferred: worker busy (cmd=%d)", (int)s_work_cmd);
        }
    }
}

static void on_settings_saved(bool stay_connected, bool auto_fetch)
{
    /* Runs on httpd task after NVS write + save response. */
    app_store_load_settings(&s_settings);

    /*
     * Later saves only reconnect. Auto-fetch would replace the article the
     * user is still reading. Long-press OK remains the manual pull.
     */
    if (!auto_fetch) {
        s_fetch_after_save = false;
        /* Drop 「网络断开」/「正在更新内容」 so the current article stays readable. */
        app_ui_hide_modal();
        ESP_LOGI(TAG, "settings saved; STA only, article kept (stay=%d)",
                 (int)stay_connected);
        return;
    }

    /* Save must not bypass the 10-minute cooldown with a failing fetch UI. */
    time_t now = time(NULL);
    int64_t now_sec = (now > 1700000000) ? (int64_t)now
                      : (int64_t)(esp_timer_get_time() / 1000000LL);
    int64_t last = s_last_fetch_unix;
    if (last <= 0) last = s_settings.last_fetch_unix;
    int remain = app_cooldown_remaining_minutes(last, now_sec);
    if (now < 1700000000 && last > 1700000000) {
        remain = 0;
    }
    if (remain > 0) {
        s_fetch_after_save = false;
        app_ui_show_cooldown(remain);
        ESP_LOGI(TAG, "settings saved; skip fetch (cooldown %d min)", remain);
        return;
    }

    app_ui_show_settings_updating();
    s_fetch_after_save = true;
    /* Prevent GOT_IP from starting httpd while we wait to fetch. */
    app_net_begin_fetch_window();
    /*
     * Always go through do_fetch's wait/retry path via the worker.
     * stay_connected: queue now (STA already up).
     * Else: on_net(GOT_IP) will queue when connected — do not fetch mid-reconnect.
     */
    if (stay_connected && app_net_get_state() == APP_NET_STA_CONNECTED
        && s_work_cmd == WORK_NONE) {
        s_fetch_after_save = false;
        s_work_cmd = WORK_FETCH;
        ESP_LOGI(TAG, "queue fetch after save (STA up, will wait for DNS/time)");
    } else if (stay_connected && s_work_cmd != WORK_NONE) {
        /* Worker busy: do not leave fetch_pending forever with no httpd restore. */
        ESP_LOGW(TAG, "settings saved but worker busy — clear fetch window, keep httpd");
        s_fetch_after_save = false;
        app_net_restore_httpd();
    } else {
        ESP_LOGI(TAG, "settings saved; fetch armed for GOT_IP (stay=%d state=%d)",
                 (int)stay_connected, (int)app_net_get_state());
    }
}

/** True when wall clock looks sane (SNTP done) — needed for TLS cert verify. */
static bool wall_clock_ok(void)
{
    return time(NULL) > 1700000000;
}

/** Best-effort DNS check for DeepSeek before burning heap on TLS. */
static bool dns_ready_deepseek(void)
{
    struct addrinfo hints = { 0 };
    struct addrinfo *res = NULL;
    hints.ai_family = AF_INET; /* match HTTP_ADDR_TYPE_INET */
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo("api.deepseek.com", "443", &hints, &res);
    if (r != 0 || !res) {
        ESP_LOGW(TAG, "DNS not ready for api.deepseek.com (gai=%d)", r);
        return false;
    }
    freeaddrinfo(res);
    return true;
}

static bool fetch_err_retryable(esp_err_t err)
{
    return err == ESP_ERR_TIMEOUT
        || err == ESP_ERR_NO_MEM
        || err == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT
        || err == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME
        || err == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST
        || err == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT
        || err == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED
        || err == ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED
        || err == ESP_FAIL;
}

static void report_fetch_error(esp_err_t err)
{
    const char *msg = app_deepseek_err_msg(err);
    if (!msg || !msg[0]) msg = "获取失败，检查网络与密钥";
    const char *detail = app_deepseek_last_detail();
    char body[160];
    if (detail && detail[0]) {
        snprintf(body, sizeof(body), "%s（%s）", msg, detail);
    } else {
        snprintf(body, sizeof(body), "%s", msg);
    }
    const char *title = "获取失败";
    if (err == ESP_ERR_INVALID_STATE) title = "缺少 API Key";
    else if (err == ESP_ERR_NO_MEM) title = "内存不足";
    else if (err == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) title = "网络未就绪";
    else if (err == ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED
             || err == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED) title = "安全连接失败";
    else if (err == ESP_FAIL) {
        int st = app_deepseek_last_http_status();
        if (st == 401) title = "密钥无效";
        else if (st == 402) title = "额度不足";
        else if (st == 429) title = "请求过频";
        else title = "服务返回错误";
    }
    app_ui_show_fetch_error(title, body);
    ESP_LOGE(TAG, "fetch failed: %s (%s) detail=%s",
             esp_err_to_name(err), msg, detail ? detail : "");
}

/**
 * Wait for STA IP, SNTP, DNS, and a large enough contiguous heap block.
 * Post-SoftAP fetch used to race httpd/SoftAP for RAM: clock OK but TLS OOM
 * → generic 获取失败; long-press later worked once SoftAP/httpd were gone.
 */
static esp_err_t wait_ready_for_https(void)
{
    esp_err_t w = app_net_wait_sta_ip(25000);
    if (w != ESP_OK) {
        ESP_LOGE(TAG, "wait STA IP: %s (state=%d)", esp_err_to_name(w),
                 (int)app_net_get_state());
        return w;
    }
    /* SoftAP/httpd must be down BEFORE SNTP wait so heap can coalesce. */
    app_net_prepare_for_tls();
    vTaskDelay(pdMS_TO_TICKS(1000));

    for (int i = 0; i < 60 && !wall_clock_ok(); i++) { /* ~18s */
        if ((i % 5) == 0) {
            ESP_LOGI(TAG, "waiting SNTP before TLS (%d/60)", i);
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (!wall_clock_ok()) {
        ESP_LOGW(TAG, "SNTP not ready; attempting TLS anyway");
    } else {
        ESP_LOGI(TAG, "wall clock OK for TLS");
    }

    for (int i = 0; i < 12 && !dns_ready_deepseek(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* mbedTLS + JSON need a sizable contiguous block on no-PSRAM C3. */
    for (int i = 0; i < 30; i++) {
        size_t free_sz = esp_get_free_heap_size();
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        ESP_LOGI(TAG, "pre-TLS heap free=%u largest=%u (%d/30)",
                 (unsigned)free_sz, (unsigned)largest, i);
        if (largest >= 48000 && free_sz >= 70000) break;
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    return ESP_OK;
}

static void do_fetch(void)
{
    ESP_LOGI(TAG, "do_fetch begin heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    app_ui_show_confirm_fetch(false);
    app_ui_set_status_msg("正在生成短文…");
    app_store_load_settings(&s_settings);
    if (!s_settings.has_api_key || !s_settings.api_key[0]) {
        app_ui_show_fetch_error("缺少 API Key", "请在配置页填写 DeepSeek API Key");
        ESP_LOGE(TAG, "fetch aborted: no api key");
        /* on_settings_saved may have armed fetch window already — restore portal. */
        app_net_restore_httpd();
        return;
    }

    /* Stop httpd immediately so GOT_IP cannot restart it during SNTP wait. */
    app_net_begin_fetch_window();

    if (wait_ready_for_https() != ESP_OK) {
        app_ui_show_fetch_error("未联网", "Wi-Fi 未连接成功，请检查账号密码后重配");
        app_net_restore_httpd();
        return;
    }
    if (app_net_get_state() != APP_NET_STA_CONNECTED) {
        app_ui_show_fetch_error("未联网", "请先完成配网");
        ESP_LOGE(TAG, "fetch aborted: STA not connected (state=%d)",
                 (int)app_net_get_state());
        app_net_restore_httpd();
        return;
    }

    /* prepare_for_tls already ran inside wait_ready; settle once more. */
    vTaskDelay(pdMS_TO_TICKS(500));
    if (app_net_get_state() != APP_NET_STA_CONNECTED) {
        app_ui_show_fetch_error("网络中断", "准备联网时断开，请重试");
        ESP_LOGE(TAG, "fetch aborted: STA lost after prepare_for_tls");
        app_net_restore_httpd();
        return;
    }

    char *buf = NULL;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        free(buf);
        buf = NULL;
        if (attempt > 1) {
            ESP_LOGW(TAG, "fetch attempt%d after settle (%s)",
                     attempt, esp_err_to_name(err));
            app_ui_set_status_msg("重试拉取短文…");
            vTaskDelay(pdMS_TO_TICKS(1500 * attempt));
            if (app_net_get_state() != APP_NET_STA_CONNECTED) {
                (void)app_net_wait_sta_ip(10000);
            }
            app_net_prepare_for_tls();
            vTaskDelay(pdMS_TO_TICKS(400));
            if (!dns_ready_deepseek()) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (app_net_get_state() != APP_NET_STA_CONNECTED) continue;
        err = app_deepseek_fetch(&s_settings, &buf);
        if (err == ESP_OK && buf) break;
        if (!fetch_err_retryable(err)) break;
    }
    if (err != ESP_OK || !buf) {
        report_fetch_error(err != ESP_OK ? err : ESP_FAIL);
        free(buf);
        app_net_restore_httpd();
        return;
    }
    time_t now = time(NULL);
    if (now < 1700000000) {
        /* clock not synced yet: use boot-relative pseudo to still enforce cooldown */
        now = (time_t)(esp_timer_get_time() / 1000000LL);
    }
    {
        char topic[APP_STORE_TOPIC_MAX];
        if (app_deepseek_extract_topic(buf, topic, sizeof(topic))) {
            ESP_LOGI(TAG, "topic=%s", topic);
            (void)app_store_push_topic(topic);
        } else {
            ESP_LOGW(TAG, "no ##topic## marker; article shown as-is");
        }
    }
    /* Save after strip so NVS/display never keep ##题目##. */
    esp_err_t save_err = app_store_save_article(buf, (int64_t)now);
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS article save failed: %s (still showing fetched text)",
                 esp_err_to_name(save_err));
    }
    s_last_fetch_unix = (int64_t)now;
    s_settings.last_fetch_unix = s_last_fetch_unix;

    /* Display immediately from RAM — do not wait for reboot / NVS. */
    apply_article_text(buf);
    free(buf);
    app_net_restore_httpd();
    app_ui_set_status_msg("");
    ESP_LOGI(TAG, "article updated");
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_work_cmd == WORK_FETCH) {
            s_work_cmd = WORK_NONE;
            do_fetch();
        } else if (s_work_cmd == WORK_SLEEP) {
            s_work_cmd = WORK_NONE;
            (void)app_store_save_page((uint16_t)app_ui_current_page());
            app_ui_flash_tip("进入深度睡眠…");
            vTaskDelay(pdMS_TO_TICKS(800));
            if (!app_sleep_enter_deep()) {
                /* Sleep aborted — never treat esp_restart as sleep. Re-arm keys. */
                ESP_LOGW(TAG, "deep sleep aborted; re-init buttons");
                (void)bsp_button_init(on_key, NULL);
                app_ui_flash_tip("请再双击 OK 入睡");
            }
        } else if (s_fetch_after_save
                   && app_net_get_state() == APP_NET_STA_CONNECTED) {
            /* Deferred post-save fetch when GOT_IP raced a busy worker. */
            s_fetch_after_save = false;
            s_work_cmd = WORK_FETCH;
            ESP_LOGI(TAG, "queue deferred fetch after config save");
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void process_input(const input_event_t *input)
{
    /* Long-press OK: cooldown or confirm */
    if (input->btn == BSP_BTN_OK && input->event == BSP_BTN_LONG) {
        app_ui_note_activity();
        time_t now = time(NULL);
        int64_t now_sec = (now > 1700000000) ? (int64_t)now
                          : (int64_t)(esp_timer_get_time() / 1000000LL);
        /* Prefer stored NVS timestamp */
        int64_t last = s_last_fetch_unix;
        if (last <= 0) last = s_settings.last_fetch_unix;
        int remain = app_cooldown_remaining_minutes(last, now_sec);
        /* If wall clock invalid and last was wall-clock based, treat carefully:
           when both are boot-relative after reboot, last from NVS may be absolute
           unix — then remain may look huge. If now is boot-relative (< 1700000000)
           and last looks like unix, skip cooldown block only if last is unix and now isn't. */
        if (now < 1700000000 && last > 1700000000) {
            /* After reboot before SNTP: allow confirm (cannot compute accurately). */
            remain = 0;
        }
        if (remain > 0) {
            app_ui_show_cooldown(remain);
        } else {
            app_ui_show_confirm_fetch(true);
        }
        return;
    }

    app_ui_on_key(input->btn, input->event);

    if (app_ui_wants_fetch()) {
        app_ui_clear_fetch_request();
        if (s_work_cmd == WORK_NONE) s_work_cmd = WORK_FETCH;
    }
    if (app_ui_wants_sleep()) {
        app_ui_clear_sleep_request();
        if (s_work_cmd == WORK_NONE) s_work_cmd = WORK_SLEEP;
    }
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            process_input(&input);
        }
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "wake key released");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void ui_timer_cb(void *arg)
{
    (void)arg;
    int64_t session = (esp_timer_get_time() - s_session_start_us) / 1000000LL;
    time_t now = time(NULL);
    struct tm tmv;
    bool valid = now > 1700000000 && localtime_r(&now, &tmv);
    int hour = 0;
    int minute = 0;
    bool show_clock = false;
    if (valid) {
        hour = tmv.tm_hour;
        minute = tmv.tm_min;
        s_frozen_hour = hour;
        s_frozen_min = minute;
        s_have_frozen_clock = true;
        s_time_ok = true;
        show_clock = true;
    } else if (s_have_frozen_clock) {
        /* Keep last good wall time while offline — avoid 1970 / garbage flicker. */
        hour = s_frozen_hour;
        minute = s_frozen_min;
        show_clock = true;
    }
    app_ui_tick(session, hour, minute, show_clock);

    int64_t idle = esp_timer_get_time() - app_ui_last_activity_us();
    if (idle > (int64_t)APP_IDLE_SLEEP_SEC * 1000000LL) {
        if (s_work_cmd == WORK_NONE) s_work_cmd = WORK_SLEEP;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI护照 解闷阅读 启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    const esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "boot reset_reason=%d wake_causes=0x%lx",
             (int)rr, (unsigned long)wake_causes);
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "GPIO wake: arm key guard");
    } else if (rr == ESP_RST_SW) {
        ESP_LOGW(TAG, "software restart (not GPIO deep-sleep wake)");
    }

    ESP_ERROR_CHECK(app_store_init());
    app_store_load_settings(&s_settings);
    s_last_fetch_unix = s_settings.last_fetch_unix;

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed");
        return;
    }
    app_ui_apply_brightness(app_store_load_brightness());
    /* Skip bsp_audio_init: this app is display-only; I2S DMA steals contiguous
     * heap that TLS needs right after SoftAP provisioning on no-PSRAM C3. */
    (void)bsp_battery_init();

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    xTaskCreate(input_task, "input", 4096, NULL, 5, &s_input_task);
    xTaskCreate(worker_task, "worker", 28672, NULL, 4, &s_worker);

    esp_err_t button_err = bsp_button_init(on_key, NULL);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(button_err));
    } else if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }

    s_session_start_us = esp_timer_get_time();
    if (bsp_lvgl_lock(1000)) {
        app_ui_init();
        bsp_lvgl_unlock();
        s_input_ready = true;
    }
    load_or_sample_article();

    /* SoftAP bottom-bar hint is set by app_ui_set_net_state(APP_NET_AP_MODE).
     * Do not put a toast over the article — that caused yellow/white ghosting. */
    app_net_set_settings_saved_cb(on_settings_saved);
    app_net_start(on_net);

    esp_timer_handle_t periodic;
    const esp_timer_create_args_t targs = {
        .callback = &ui_timer_cb,
        .name = "ui_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &periodic));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic, 500000)); /* 0.5s */

    ESP_LOGI(TAG, "ready");
}
