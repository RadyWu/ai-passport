#include "app_ui.h"
#include "app_store.h"
#include "app_cooldown.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "font_toilet_14.h"
#include "font_toilet_20.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ui";

/* Teal chrome; body text is soft cool-white for contrast without harsh #FFF. */
#define COL_BG        0x14202B
#define COL_BODY      0xE8EEF7
#define COL_ACCENT    0x3D9CF0
#define COL_MUTED     0x8AA0B5
#define COL_WARN      0xE8A317
#define COL_BAR       0x0E1620

static lv_obj_t *s_scr;
static lv_obj_t *s_wifi;
static lv_obj_t *s_session;
static lv_obj_t *s_clock;
static lv_obj_t *s_bat;
static lv_obj_t *s_body;
static lv_obj_t *s_page_lbl;
static lv_obj_t *s_hint;
static lv_obj_t *s_tip_panel;
static lv_obj_t *s_tip_lbl;
static lv_obj_t *s_modal;
static lv_obj_t *s_modal_title;
static lv_obj_t *s_modal_body;

static app_article_t s_art;
static unsigned s_page;
static bool s_confirm;
static bool s_wifi_fail_modal;
static bool s_want_fetch;
static bool s_want_sleep;
static bool s_reminded;
static int64_t s_last_activity_us;
static int64_t s_last_ok_us;
static int s_ok_tap_count;
static int64_t s_last_up_us;
static int s_up_tap_count;
static char s_page_buf[APP_PAGE_MAX_CHARS * 3 + 8];
static uint8_t s_bl_level = 4; /* 1..6, default middle = 60% */
/* 6 steps: level 1 = 暗夜 (~5%), then former 20..100. */
static const uint8_t s_bl_percent[6] = { 5, 20, 40, 60, 80, 100 };
static int64_t s_tip_until_us; /* 0 = sticky or hidden; else auto-hide deadline */
static lv_font_t s_font_chrome;
static lv_font_t s_font_body;

static void style_chrome(lv_obj_t *o, lv_color_t color)
{
    lv_obj_set_style_text_font(o, &s_font_chrome, 0);
    lv_obj_set_style_text_color(o, color, 0);
}

static void style_body(lv_obj_t *o, lv_color_t color)
{
    lv_obj_set_style_text_font(o, &s_font_body, 0);
    lv_obj_set_style_text_color(o, color, 0);
    /* line_space=0: pitch == font.line_height (== APP_BODY_LINE_H). Extra space
     * previously made the last line taller than the box and clipped/lost it. */
    lv_obj_set_style_text_line_space(o, 0, 0);
}


static void hide_tip_locked(void)
{
    s_tip_until_us = 0;
    if (!s_tip_panel) return;
    if (s_tip_lbl) lv_label_set_text(s_tip_lbl, "");
    lv_obj_add_flag(s_tip_panel, LV_OBJ_FLAG_HIDDEN);
}

/** auto_clear_us: 0 = stay until cleared/replaced; else hide after that many us. */
static void show_tip_locked(const char *msg, int64_t auto_clear_us)
{
    if (!s_tip_panel || !s_tip_lbl) return;
    if (!msg || !msg[0]) {
        hide_tip_locked();
        return;
    }
    lv_label_set_text(s_tip_lbl, msg);
    lv_obj_clear_flag(s_tip_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_tip_panel);
    s_tip_until_us = auto_clear_us > 0
        ? (esp_timer_get_time() + auto_clear_us)
        : 0;
}

static void refresh_page(void)
{
    if (!s_body) return;
    if (s_art.page_count == 0) {
        /* No second intro here — SAMPLE_ARTICLE in main.c is the only定稿.
         * Showing another welcome list caused boot flicker (两套文案抢显示). */
        lv_label_set_text(s_body, "");
        lv_label_set_text(s_page_lbl, "—");
        return;
    }
    if (s_page >= s_art.page_count) s_page = s_art.page_count - 1;
    if (!app_article_get_page(&s_art, s_page, s_page_buf, sizeof(s_page_buf))) {
        lv_label_set_text(s_body, "(空)");
    } else {
        lv_label_set_text(s_body, s_page_buf);
    }
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%u / %u", s_page + 1, (unsigned)s_art.page_count);
    lv_label_set_text(s_page_lbl, tmp);
}

static void hide_modal(void)
{
    if (s_modal) lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    s_confirm = false;
    s_wifi_fail_modal = false;
}

static void show_modal(const char *title, const char *body)
{
    if (!s_modal) return;
    lv_label_set_text(s_modal_title, title);
    lv_label_set_text(s_modal_body, body);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

void app_ui_init(void)
{
    s_font_chrome = font_toilet_14;
    s_font_chrome.fallback = &lv_font_montserrat_14;
    s_font_body = font_toilet_20;
    s_font_body.fallback = &lv_font_montserrat_14;

    s_last_activity_us = esp_timer_get_time();
    s_bl_level = app_store_load_brightness();
    bsp_display_backlight(s_bl_percent[s_bl_level - 1]);
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Status bar */
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_set_size(bar, 240, 28);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BAR), 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Left cluster: Wi-Fi + clock. Center: session. Right: battery. */
    s_wifi = lv_label_create(bar);
    style_chrome(s_wifi, lv_color_hex(COL_MUTED));
    lv_obj_set_pos(s_wifi, 6, 6);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI " --");

    s_clock = lv_label_create(bar);
    style_chrome(s_clock, lv_color_hex(COL_MUTED));
    lv_obj_set_pos(s_clock, 68, 6);
    lv_label_set_text(s_clock, "--:--");

    s_session = lv_label_create(bar);
    style_chrome(s_session, lv_color_hex(COL_ACCENT));
    lv_obj_set_style_text_align(s_session, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_session, 56);
    lv_obj_set_pos(s_session, 108, 6);
    lv_label_set_text(s_session, "0:00");

    s_bat = lv_label_create(bar);
    style_chrome(s_bat, lv_color_hex(COL_MUTED));
    lv_obj_align(s_bat, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_label_set_text(s_bat, "--%");

    /* Body text directly on teal background — no white card. */
    s_body = lv_label_create(s_scr);
    style_body(s_body, lv_color_hex(COL_BODY));
    lv_obj_set_size(s_body, APP_BODY_WIDTH_PX, APP_BODY_LINES * APP_BODY_LINE_H);
    lv_obj_set_pos(s_body, 20, 36);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);

    s_page_lbl = lv_label_create(s_scr);
    style_chrome(s_page_lbl, lv_color_hex(COL_MUTED));
    lv_obj_align(s_page_lbl, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_label_set_text(s_page_lbl, "—");

    s_hint = lv_label_create(s_scr);
    style_chrome(s_hint, lv_color_hex(COL_MUTED));
    lv_obj_set_width(s_hint, 220);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_label_set_text(s_hint, "长按OK拉取  长按上下调亮度");

    /* Bottom tip: covers page number + IP/config hint; larger body font. */
    s_tip_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_tip_panel, 240, 48);
    lv_obj_align(s_tip_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(s_tip_panel, 0, 0);
    lv_obj_set_style_border_width(s_tip_panel, 0, 0);
    lv_obj_set_style_bg_color(s_tip_panel, lv_color_hex(COL_BAR), 0);
    lv_obj_set_style_bg_opa(s_tip_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_tip_panel, 4, 0);
    lv_obj_clear_flag(s_tip_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_tip_panel, LV_OBJ_FLAG_HIDDEN);
    s_tip_lbl = lv_label_create(s_tip_panel);
    style_body(s_tip_lbl, lv_color_hex(COL_WARN));
    lv_obj_set_width(s_tip_lbl, 232);
    lv_obj_set_style_text_align(s_tip_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_tip_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_tip_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_tip_lbl, "");

    /* Modal */
    s_modal = lv_obj_create(s_scr);
    lv_obj_set_size(s_modal, 210, 140);
    lv_obj_center(s_modal);
    lv_obj_set_style_radius(s_modal, 14, 0);
    lv_obj_set_style_border_width(s_modal, 2, 0);
    lv_obj_set_style_border_color(s_modal, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(COL_BAR), 0);
    lv_obj_set_style_pad_all(s_modal, 12, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    s_modal_title = lv_label_create(s_modal);
    style_chrome(s_modal_title, lv_color_hex(COL_ACCENT));
    lv_obj_set_width(s_modal_title, 180);
    lv_label_set_text(s_modal_title, "");

    s_modal_body = lv_label_create(s_modal);
    style_chrome(s_modal_body, lv_color_hex(COL_BODY));
    lv_obj_set_width(s_modal_body, 180);
    lv_obj_set_pos(s_modal_body, 0, 28);
    lv_label_set_long_mode(s_modal_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_modal_body, "");

    lv_obj_t *tip = lv_label_create(s_modal);
    style_chrome(tip, lv_color_hex(COL_MUTED));
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_label_set_text(tip, "按任意键关闭");

    hide_modal();
    refresh_page();
    lv_screen_load(s_scr);
    ESP_LOGI(TAG, "reader UI ready (body %dpx)", APP_BODY_GLYPH_PX);
}

void app_ui_set_article(const app_article_t *art)
{
    if (!art) return;
    s_art = *art;
    s_page = 0;
    if (!bsp_lvgl_lock(500)) return;
    hide_modal();
    refresh_page();
    bsp_lvgl_unlock();
}

void app_ui_set_article_text(const char *text)
{
    memset(&s_art, 0, sizeof(s_art));
    if (text && text[0]) {
        strncpy(s_art.text, text, sizeof(s_art.text) - 1);
        /* 2px slack vs label box so LVGL wrap never needs an 11th line. */
        if (!app_article_paginate_fitted(&s_art, APP_BODY_WIDTH_PX - 2, APP_BODY_GLYPH_PX, APP_BODY_LINES)) {
            ESP_LOGW(TAG, "paginate failed");
        }
    }
    s_page = 0;
    if (!bsp_lvgl_lock(500)) return;
    hide_modal();
    refresh_page();
    bsp_lvgl_unlock();
}

void app_ui_set_net_state(app_net_state_t st, const char *ip)
{
    if (!bsp_lvgl_lock(500)) return;
    switch (st) {
    case APP_NET_AP_MODE: {
        lv_label_set_text(s_wifi, LV_SYMBOL_WIFI " 配网");
        {
            const char *ap = app_net_ap_ssid();
            char h[72];
            snprintf(h, sizeof(h), "连%s 开192.168.4.1",
                     (ap && ap[0]) ? ap : "AIPassport-xxxx");
            lv_label_set_text(s_hint, h);
        }
        break;
    }
    case APP_NET_STA_CONNECTING:
        lv_label_set_text(s_wifi, LV_SYMBOL_WIFI " ...");
        /* SoftAP may already be up (boot / outage portal). STA still retries. */
        if (app_net_softap_portal_active()) {
            const char *ap = app_net_ap_ssid();
            char h[72];
            snprintf(h, sizeof(h), "重连中 可连%s",
                     (ap && ap[0]) ? ap : "热点");
            lv_label_set_text(s_hint, h);
        } else {
            lv_label_set_text(s_hint, "正在重连 Wi-Fi…");
        }
        break;
    case APP_NET_STA_CONNECTED:
        lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
        if (ip && ip[0]) {
            char h[64];
            snprintf(h, sizeof(h), "配置页 http://%s/", ip);
            lv_label_set_text(s_hint, h);
        }
        break;
    case APP_NET_STA_FAILED: {
        lv_label_set_text(s_wifi, LV_SYMBOL_WARNING);
        {
            const char *ap = app_net_ap_ssid();
            char h[72];
            snprintf(h, sizeof(h), "连%s 开192.168.4.1",
                     (ap && ap[0]) ? ap : "AIPassport-xxxx");
            lv_label_set_text(s_hint, h);
        }
        /* Mid-use loss uses app_ui_show_net_lost(); here tip only (no second modal). */
        show_tip_locked("Wi-Fi 未连上，可连热点重配", 5000000LL);
        break;
    }
    default:
        lv_label_set_text(s_wifi, LV_SYMBOL_WIFI " --");
        break;
    }
    bsp_lvgl_unlock();
}

void app_ui_set_status_msg(const char *msg)
{
    if (!bsp_lvgl_lock(500)) return;
    /* Sticky bottom tip (fetch progress / errors); empty clears. */
    show_tip_locked(msg, 0);
    bsp_lvgl_unlock();
}

void app_ui_flash_tip(const char *msg)
{
    if (!bsp_lvgl_lock(500)) return;
    show_tip_locked(msg, 1000000LL); /* 1s */
    bsp_lvgl_unlock();
}

void app_ui_show_settings_updating(void)
{
    if (!bsp_lvgl_lock(500)) return;
    s_confirm = false;
    s_wifi_fail_modal = false;
    show_modal("设置已保存", "设置修改完成，正在更新内容");
    bsp_lvgl_unlock();
}

void app_ui_show_wifi_join_failed(void)
{
    if (!bsp_lvgl_lock(500)) return;
    s_confirm = false;
    s_wifi_fail_modal = true;
    show_modal("Wi-Fi 连不上",
               "请检查名称和密码。\n热点仍开着，可马上修改。");
    if (s_modal) lv_obj_move_foreground(s_modal);
    show_tip_locked("Wi-Fi 连不上，请检查名称和密码", 0);
    bsp_lvgl_unlock();
}

void app_ui_clear_wifi_join_failed(void)
{
    if (!s_wifi_fail_modal) return;
    if (!bsp_lvgl_lock(500)) return;
    if (s_wifi_fail_modal) {
        hide_modal();
        hide_tip_locked();
    }
    bsp_lvgl_unlock();
}

void app_ui_show_confirm_fetch(bool show)
{
    if (!bsp_lvgl_lock(500)) return;
    if (show) {
        s_confirm = true;
        show_modal("获取新内容", "是否获取新内容？\n短按 OK 开始拉取。");
    } else {
        hide_modal();
    }
    bsp_lvgl_unlock();
}

void app_ui_show_cooldown(int remain_min)
{
    if (!bsp_lvgl_lock(500)) return;
    char body[96];
    snprintf(body, sizeof(body), "刚才已经读过了哦，%d 分钟后再来吧~", remain_min);
    show_modal("稍后再读", body);
    s_confirm = false;
    bsp_lvgl_unlock();
}

void app_ui_show_fetch_error(const char *title, const char *body)
{
    if (!bsp_lvgl_lock(500)) return;
    s_confirm = false;
    show_modal(title && title[0] ? title : "获取失败",
               body && body[0] ? body : "请检查网络与 API Key");
    show_tip_locked(body && body[0] ? body : "获取失败", 0);
    bsp_lvgl_unlock();
}

void app_ui_show_net_lost(void)
{
    if (!bsp_lvgl_lock(500)) return;
    s_confirm = false;
    show_modal("网络断开",
               "Wi-Fi 已断开，可继续阅读已缓存短文。\n"
               "需要重配时请连设备热点，打开 192.168.4.1");
    show_tip_locked("网络断开，可连热点重配", 5000000LL);
    bsp_lvgl_unlock();
}

void app_ui_show_session_remind(bool show)
{
    if (!show) return;
    if (s_reminded) return;
    s_reminded = true;
    if (!bsp_lvgl_lock(500)) return;
    show_modal("温馨提示", "已经蹲了5分钟了，该起来了哦～");
    s_confirm = false;
    bsp_lvgl_unlock();
}

void app_ui_tick(int64_t session_sec, int hour, int minute, bool time_valid)
{
    if (!bsp_lvgl_lock(200)) return;
    char mss[12];
    app_format_session_mss(mss, sizeof(mss), session_sec);
    lv_label_set_text(s_session, mss);
    if (time_valid) {
        char clk[8];
        snprintf(clk, sizeof(clk), "%02d:%02d", hour, minute);
        lv_label_set_text(s_clock, clk);
    } else {
        lv_label_set_text(s_clock, "--:--");
    }
    if (s_bat) {
        int soc = bsp_battery_soc();
        if (soc < 0) {
            lv_label_set_text(s_bat, "--%");
            lv_obj_set_style_text_color(s_bat, lv_color_hex(COL_MUTED), 0);
        } else {
            char b[12];
            snprintf(b, sizeof(b), "%d%%", soc);
            lv_label_set_text(s_bat, b);
            lv_obj_set_style_text_color(s_bat,
                soc < 20 ? lv_color_hex(COL_WARN) : lv_color_hex(COL_MUTED), 0);
        }
    }
    if (s_tip_until_us && esp_timer_get_time() >= s_tip_until_us) {
        hide_tip_locked();
    }
    bsp_lvgl_unlock();
    if (session_sec >= APP_SESSION_REMIND_SEC) app_ui_show_session_remind(true);
}

void app_ui_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

int64_t app_ui_last_activity_us(void) { return s_last_activity_us; }
bool app_ui_confirm_visible(void) { return s_confirm && s_modal && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN); }
bool app_ui_wants_fetch(void) { return s_want_fetch; }
void app_ui_clear_fetch_request(void) { s_want_fetch = false; }
bool app_ui_wants_sleep(void) { return s_want_sleep; }
void app_ui_clear_sleep_request(void) { s_want_sleep = false; }

void app_ui_apply_brightness(uint8_t level_1_to_6)
{
    if (level_1_to_6 < 1) level_1_to_6 = 1;
    if (level_1_to_6 > 6) level_1_to_6 = 6;
    s_bl_level = level_1_to_6;
    bsp_display_backlight(s_bl_percent[s_bl_level - 1]);
}

uint8_t app_ui_brightness_level(void)
{
    return s_bl_level;
}

unsigned app_ui_current_page(void) { return s_page; }

void app_ui_set_page(unsigned page)
{
    if (s_art.page_count == 0) {
        s_page = 0;
        return;
    }
    if (page >= s_art.page_count) page = s_art.page_count - 1;
    s_page = page;
    if (!bsp_lvgl_lock(500)) return;
    refresh_page();
    bsp_lvgl_unlock();
}

void app_ui_hide_modal(void)
{
    if (!bsp_lvgl_lock(500)) return;
    hide_modal();
    bsp_lvgl_unlock();
}

void app_ui_on_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    app_ui_note_activity();

    if (!bsp_lvgl_lock(500)) return;

    /* Modals: any handled key dismisses. Confirm-fetch: OK confirms pull. */
    if (s_modal && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN)) {
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE) {
            if (s_confirm && btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                hide_modal();
                s_want_fetch = true;
                s_ok_tap_count = 0;
                bsp_lvgl_unlock();
                return;
            }
            /* Settings-updating / cooldown / remind / errors: dismiss only.
             * Fetch already queued in background when applicable. */
            hide_modal();
            s_ok_tap_count = 0;
            /* Do not treat this OK as half of a double-tap for sleep. */
            bsp_lvgl_unlock();
            return;
        }
        bsp_lvgl_unlock();
        return;
    }

    /* Long-press UP/DOWN: brightness only (short press still pages). */
    if (ev == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        if (btn == BSP_BTN_UP && s_bl_level < 6) s_bl_level++;
        else if (btn == BSP_BTN_DOWN && s_bl_level > 1) s_bl_level--;
        bsp_display_backlight(s_bl_percent[s_bl_level - 1]);
        (void)app_store_save_brightness(s_bl_level);
        char msg[24];
        snprintf(msg, sizeof(msg), "亮度 %u/6", (unsigned)s_bl_level);
        show_tip_locked(msg, 1000000LL); /* 1s over page+hint */
        bsp_lvgl_unlock();
        return;
    }

    /* Any other key dismisses a timed tip early. */
    if (s_tip_until_us) hide_tip_locked();

    /* Prefer BSP hardware DOUBLE (iot_button); also accept two CLICKs. */
    if (btn == BSP_BTN_OK && ev == BSP_BTN_DOUBLE) {
        s_ok_tap_count = 0;
        s_up_tap_count = 0;
        show_tip_locked("进入深度睡眠…", 1000000LL);
        s_want_sleep = true;
        bsp_lvgl_unlock();
        return;
    }

    /* Double-click UP → first page (hardware DOUBLE or two CLICKs). */
    if (btn == BSP_BTN_UP && ev == BSP_BTN_DOUBLE) {
        s_up_tap_count = 0;
        s_ok_tap_count = 0;
        if (s_art.page_count) {
            s_page = 0;
            refresh_page();
            (void)app_store_save_page(0);
            show_tip_locked("已回第一页", 800000LL);
        }
        bsp_lvgl_unlock();
        return;
    }

    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        int64_t now = esp_timer_get_time();
        /* 700ms window — 400ms was too tight vs human double-tap + driver. */
        if (now - s_last_ok_us < 700000) s_ok_tap_count++;
        else s_ok_tap_count = 1;
        s_last_ok_us = now;
        s_up_tap_count = 0;
        if (s_ok_tap_count >= 2) {
            s_ok_tap_count = 0;
            show_tip_locked("进入深度睡眠…", 1000000LL);
            s_want_sleep = true;
            bsp_lvgl_unlock();
            return;
        }
    }

    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP && s_art.page_count) {
            int64_t now = esp_timer_get_time();
            if (now - s_last_up_us < 700000) s_up_tap_count++;
            else s_up_tap_count = 1;
            s_last_up_us = now;
            s_ok_tap_count = 0;
            if (s_up_tap_count >= 2) {
                s_up_tap_count = 0;
                s_page = 0;
                refresh_page();
                (void)app_store_save_page(0);
                show_tip_locked("已回第一页", 800000LL);
            } else if (s_page > 0) {
                s_page--;
                refresh_page();
                (void)app_store_save_page((uint16_t)s_page);
            }
        } else if (btn == BSP_BTN_DOWN && s_art.page_count) {
            s_up_tap_count = 0;
            s_ok_tap_count = 0;
            if (s_page + 1 < s_art.page_count) {
                s_page++;
                refresh_page();
                (void)app_store_save_page((uint16_t)s_page);
            }
        }
    }
    bsp_lvgl_unlock();
}
