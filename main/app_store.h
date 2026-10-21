#pragma once
#include "app_wifi_slots.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define APP_STORE_API_KEY_MAX 128
#define APP_STORE_PROMPT_MAX 512
#define APP_STORE_MODEL_MAX 64
#define APP_STORE_SSID_MAX APP_WIFI_SSID_MAX
#define APP_STORE_PASS_MAX APP_WIFI_PASS_MAX
#define APP_STORE_TOPIC_MAX 64
#define APP_STORE_TOPIC_SLOTS 5
#define APP_STORE_WIFI_SLOTS APP_WIFI_SLOTS

typedef struct {
    /** Slot 0 mirror. Prefer `wifi[]` for new code. NVS key remains "ssid"/"pass". */
    char wifi_ssid[APP_STORE_SSID_MAX];
    char wifi_pass[APP_STORE_PASS_MAX];
    /**
     * Up to 3 networks, tried in order. Empty SSID slots are skipped.
     * Slot 0 uses the original NVS keys "ssid"/"pass" so v1.0.x devices keep Wi-Fi 1.
     * Slots 1 and 2 use "ssid2"/"pass2" and "ssid3"/"pass3".
     */
    app_wifi_cred_t wifi[APP_STORE_WIFI_SLOTS];
    char api_key[APP_STORE_API_KEY_MAX];
    char preference[APP_STORE_PROMPT_MAX];
    char model[APP_STORE_MODEL_MAX];
    int64_t last_fetch_unix;
    /** True when any slot has a non-empty SSID. */
    bool has_wifi;
    bool has_api_key;
} app_settings_t;

esp_err_t app_store_init(void);
esp_err_t app_store_load_settings(app_settings_t *out);
esp_err_t app_store_save_settings(const app_settings_t *in);
esp_err_t app_store_save_article(const char *text, int64_t fetch_unix);
esp_err_t app_store_load_article(char *buf, size_t buf_size, int64_t *fetch_unix);
/** True when NVS holds a fetched article (not the built-in sample). */
bool app_store_has_article(void);

/** Recent article titles from ##题目## (newest first). Empty slots are "". */
esp_err_t app_store_load_topics(char topics[APP_STORE_TOPIC_SLOTS][APP_STORE_TOPIC_MAX]);
/** Push a new title to the front; drops the oldest (keeps last 5). */
esp_err_t app_store_push_topic(const char *title);

void app_store_clear_api_key_log_safe(void);

/** Backlight step 1..6 (default 4 = 60%; level 1 = 暗夜 ~5%). */
uint8_t app_store_load_brightness(void);
esp_err_t app_store_save_brightness(uint8_t level_1_to_6);

/** Article page index (0-based) for resume after deep sleep. */
uint16_t app_store_load_page(void);
esp_err_t app_store_save_page(uint16_t page);

/**
 * Last two DeepSeek debug strings, newest first, kept across reboot.
 * Namespace "toilet". A third push overwrites only the oldest slot.
 *
 *   spi  u8     prompt ring (count<<2 | newest slot)
 *   sp0  str    system prompt slot, APP_DEBUG_PROMPT_MAX including NUL
 *   sp1  str    the other prompt slot
 *   lri  u8     raw-reply ring, same packing
 *   lr0  blob   assistant text, APP_DEBUG_RAW_MAX including NUL
 *   lr1  blob   the other raw slot
 *
 * Prompts are NVS strings (under the ~4000-byte page limit). Raw replies are
 * blobs so they can hold a full parsed article. Store failures are returned
 * and must not fail the article fetch. position 0 is newest; a missing slot
 * writes "".
 */
esp_err_t app_store_push_debug_prompt(const char *text);
esp_err_t app_store_push_debug_raw(const char *text);
esp_err_t app_store_load_debug_prompt(int position, char *buf, size_t buf_sz);
esp_err_t app_store_load_debug_raw(int position, char *buf, size_t buf_sz);
