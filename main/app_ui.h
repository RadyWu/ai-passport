#pragma once
#include "app_article.h"
#include "app_net.h"
#include "bsp_button.h"
#include <stdbool.h>
#include <stdint.h>

void app_ui_init(void);
void app_ui_set_article(const app_article_t *art);
/** Copy + paginate into UI static storage (no extra heap article). */
void app_ui_set_article_text(const char *text);
void app_ui_set_net_state(app_net_state_t st, const char *ip);
void app_ui_set_status_msg(const char *msg);
/** Bottom tip over page+hint for ~1s (brightness / sleep). */
void app_ui_flash_tip(const char *msg);
void app_ui_show_confirm_fetch(bool show);
/** Modal after the first SoftAP/LAN config save; fetch is started by main. */
void app_ui_show_settings_updating(void);
/** Bad SSID/password after a config save. SoftAP stays up for another try. */
void app_ui_show_wifi_join_failed(void);
/** Hide that modal after the new network actually connects. */
void app_ui_clear_wifi_join_failed(void);
void app_ui_show_cooldown(int remain_min);
/** Dismissible modal for fetch / save-path errors (Wi-Fi vs API vs cooldown). */
void app_ui_show_fetch_error(const char *title, const char *body);
/** Dismissible tip: Wi-Fi lost mid-use; SoftAP available for reconfig. */
void app_ui_show_net_lost(void);
void app_ui_show_session_remind(bool show);
void app_ui_tick(int64_t session_sec, int hour, int minute, bool time_valid);
void app_ui_on_key(bsp_btn_t btn, bsp_btn_ev_t ev);
bool app_ui_confirm_visible(void);
bool app_ui_wants_fetch(void);
void app_ui_clear_fetch_request(void);
bool app_ui_wants_sleep(void);
void app_ui_clear_sleep_request(void);
void app_ui_note_activity(void);
int64_t app_ui_last_activity_us(void);
unsigned app_ui_current_page(void);
/** Restore page after load (clamped); does not write NVS. */
void app_ui_set_page(unsigned page);
/** Hide settings-updating / other non-confirm modal. */
void app_ui_hide_modal(void);
/** Apply brightness step 1..6 via bsp_display_backlight (1=暗夜 ~5%). */
void app_ui_apply_brightness(uint8_t level_1_to_6);
uint8_t app_ui_brightness_level(void);
