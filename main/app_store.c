#include "app_store.h"
#include "app_article.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_store";
static const char *NS = "toilet";

esp_err_t app_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

esp_err_t app_store_load_settings(app_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    copy_str(out->model, sizeof(out->model), "deepseek-chat");

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_OK;

    size_t len;
    len = sizeof(out->wifi_ssid);
    if (nvs_get_str(h, "ssid", out->wifi_ssid, &len) == ESP_OK) out->has_wifi = out->wifi_ssid[0] != '\0';
    len = sizeof(out->wifi_pass);
    (void)nvs_get_str(h, "pass", out->wifi_pass, &len);
    len = sizeof(out->api_key);
    if (nvs_get_str(h, "apikey", out->api_key, &len) == ESP_OK) out->has_api_key = out->api_key[0] != '\0';
    len = sizeof(out->preference);
    (void)nvs_get_str(h, "pref", out->preference, &len);
    /* Discard any legacy NVS model; firmware locks to deepseek-chat. */
    copy_str(out->model, sizeof(out->model), "deepseek-chat");
    (void)nvs_get_i64(h, "last_ts", &out->last_fetch_unix);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t app_store_save_settings(const app_settings_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", in->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", in->wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, "apikey", in->api_key);
    if (err == ESP_OK) err = nvs_set_str(h, "pref", in->preference);
    if (err == ESP_OK) err = nvs_set_str(h, "model", "deepseek-chat");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings save failed: %s (ssid_len=%u key_len=%u)",
                 esp_err_to_name(err),
                 (unsigned)strnlen(in->wifi_ssid, sizeof(in->wifi_ssid)),
                 (unsigned)strnlen(in->api_key, sizeof(in->api_key)));
    } else {
        ESP_LOGI(TAG, "settings saved ssid_len=%u key_len=%u pref_len=%u (api key not logged)",
                 (unsigned)strnlen(in->wifi_ssid, sizeof(in->wifi_ssid)),
                 (unsigned)strnlen(in->api_key, sizeof(in->api_key)),
                 (unsigned)strnlen(in->preference, sizeof(in->preference)));
    }
    return err;
}

esp_err_t app_store_save_article(const char *text, int64_t fetch_unix)
{
    if (!text) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open for article: %s", esp_err_to_name(err));
        return err;
    }
    /* Older builds stored "article" as string; erase so blob write cannot hit type mismatch. */
    (void)nvs_erase_key(h, "article");
    size_t n = strnlen(text, APP_ARTICLE_MAX_BYTES);
    if (n >= APP_ARTICLE_MAX_BYTES) n = APP_ARTICLE_MAX_BYTES - 1;
    err = nvs_set_blob(h, "article", text, n + 1); /* include NUL */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob article (%u bytes): %s",
                 (unsigned)(n + 1), esp_err_to_name(err));
    }
    if (err == ESP_OK) err = nvs_set_i64(h, "last_ts", fetch_unix);
    /* New article always starts at page 0. */
    if (err == ESP_OK) err = nvs_set_u16(h, "art_pg", 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "article saved (%u bytes)", (unsigned)(n + 1));
    }
    return err;
}

esp_err_t app_store_load_article(char *buf, size_t buf_size, int64_t *fetch_unix)
{
    if (!buf || buf_size == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    if (fetch_unix) *fetch_unix = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = buf_size;
    err = nvs_get_blob(h, "article", buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        len = buf_size;
        err = nvs_get_str(h, "article", buf, &len);
    }
    if (err == ESP_OK) {
        if (len == 0) {
            buf[0] = '\0';
        } else if (len >= buf_size) {
            buf[buf_size - 1] = '\0';
        } else if (buf[len - 1] != '\0') {
            buf[len] = '\0';
        }
    }
    if (fetch_unix) (void)nvs_get_i64(h, "last_ts", fetch_unix);
    nvs_close(h);
    return err;
}

esp_err_t app_store_load_topics(char topics[APP_STORE_TOPIC_SLOTS][APP_STORE_TOPIC_MAX])
{
    if (!topics) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < APP_STORE_TOPIC_SLOTS; i++) topics[i][0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
    for (int i = 0; i < APP_STORE_TOPIC_SLOTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "tp%d", i);
        size_t len = APP_STORE_TOPIC_MAX;
        (void)nvs_get_str(h, key, topics[i], &len);
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t app_store_push_topic(const char *title)
{
    if (!title || !title[0]) return ESP_ERR_INVALID_ARG;
    char topics[APP_STORE_TOPIC_SLOTS][APP_STORE_TOPIC_MAX];
    app_store_load_topics(topics);
    /* Shift down */
    for (int i = APP_STORE_TOPIC_SLOTS - 1; i > 0; i--) {
        copy_str(topics[i], APP_STORE_TOPIC_MAX, topics[i - 1]);
    }
    copy_str(topics[0], APP_STORE_TOPIC_MAX, title);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    for (int i = 0; i < APP_STORE_TOPIC_SLOTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "tp%d", i);
        err = nvs_set_str(h, key, topics[i]);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void app_store_clear_api_key_log_safe(void) {}

uint8_t app_store_load_brightness(void)
{
    /* Default middle of 6 steps = 60% (same as old default 3/5). */
    uint8_t lvl = 4;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return lvl;

    uint8_t v = 0;
    uint8_t flag = 0;
    const bool has = (nvs_get_u8(h, "bl_lvl", &v) == ESP_OK && v >= 1 && v <= 6);
    const bool migrated = (nvs_get_u8(h, "bl6", &flag) == ESP_OK && flag == 1);

    if (has) {
        /* Old firmware stored 1..5 with {20,40,60,80,100}. Insert 暗夜 as
         * new level 1 once: old N → N+1 so old middle 3 (60%) → new 4 (60%). */
        if (!migrated && v <= 5) {
            v = (uint8_t)(v + 1);
            if (v > 6) v = 6;
            (void)nvs_set_u8(h, "bl_lvl", v);
        }
        lvl = v;
    }
    if (!migrated) {
        (void)nvs_set_u8(h, "bl6", 1);
        (void)nvs_commit(h);
    }
    nvs_close(h);
    if (lvl < 1) lvl = 1;
    if (lvl > 6) lvl = 6;
    return lvl;
}

esp_err_t app_store_save_brightness(uint8_t level_1_to_6)
{
    if (level_1_to_6 < 1 || level_1_to_6 > 6) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "bl_lvl", level_1_to_6);
    if (err == ESP_OK) err = nvs_set_u8(h, "bl6", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint16_t app_store_load_page(void)
{
    nvs_handle_t h;
    uint16_t page = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint16_t v = 0;
    if (nvs_get_u16(h, "art_pg", &v) == ESP_OK) page = v;
    nvs_close(h);
    return page;
}

esp_err_t app_store_save_page(uint16_t page)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, "art_pg", page);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
