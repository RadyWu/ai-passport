#include "app_store.h"
#include "app_article.h"
#include "app_debug_log.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Slot 0 keeps the v1.0.x keys. Slots 1 and 2 are additive. */
static const char *const WIFI_SSID_KEYS[APP_STORE_WIFI_SLOTS] = {"ssid", "ssid2", "ssid3"};
static const char *const WIFI_PASS_KEYS[APP_STORE_WIFI_SLOTS] = {"pass", "pass2", "pass3"};

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

/** Keep wifi_ssid/wifi_pass as the slot-0 mirror and has_wifi as "any slot". */
static void normalize_wifi(app_settings_t *s)
{
    if (!s) return;
    if (!app_wifi_slot_filled(&s->wifi[0]) && s->wifi_ssid[0] != '\0') {
        copy_str(s->wifi[0].ssid, sizeof(s->wifi[0].ssid), s->wifi_ssid);
        copy_str(s->wifi[0].pass, sizeof(s->wifi[0].pass), s->wifi_pass);
    }
    copy_str(s->wifi_ssid, sizeof(s->wifi_ssid), s->wifi[0].ssid);
    copy_str(s->wifi_pass, sizeof(s->wifi_pass), s->wifi[0].pass);
    s->has_wifi = app_wifi_first_slot(s->wifi) >= 0;
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
    for (int i = 0; i < APP_STORE_WIFI_SLOTS; i++) {
        len = sizeof(out->wifi[i].ssid);
        (void)nvs_get_str(h, WIFI_SSID_KEYS[i], out->wifi[i].ssid, &len);
        len = sizeof(out->wifi[i].pass);
        (void)nvs_get_str(h, WIFI_PASS_KEYS[i], out->wifi[i].pass, &len);
    }
    len = sizeof(out->api_key);
    if (nvs_get_str(h, "apikey", out->api_key, &len) == ESP_OK) out->has_api_key = out->api_key[0] != '\0';
    len = sizeof(out->preference);
    (void)nvs_get_str(h, "pref", out->preference, &len);
    /* Discard any legacy NVS model; firmware locks to deepseek-chat. */
    copy_str(out->model, sizeof(out->model), "deepseek-chat");
    (void)nvs_get_i64(h, "last_ts", &out->last_fetch_unix);
    nvs_close(h);
    normalize_wifi(out);
    return ESP_OK;
}

esp_err_t app_store_save_settings(const app_settings_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    app_settings_t stored = *in;
    normalize_wifi(&stored);

    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    for (int i = 0; i < APP_STORE_WIFI_SLOTS && err == ESP_OK; i++) {
        err = nvs_set_str(h, WIFI_SSID_KEYS[i], stored.wifi[i].ssid);
        if (err == ESP_OK) err = nvs_set_str(h, WIFI_PASS_KEYS[i], stored.wifi[i].pass);
    }
    if (err == ESP_OK) err = nvs_set_str(h, "apikey", stored.api_key);
    if (err == ESP_OK) err = nvs_set_str(h, "pref", stored.preference);
    if (err == ESP_OK) err = nvs_set_str(h, "model", "deepseek-chat");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings save failed: %s (ssid_len=%u/%u/%u key_len=%u)",
                 esp_err_to_name(err),
                 (unsigned)strnlen(stored.wifi[0].ssid, sizeof(stored.wifi[0].ssid)),
                 (unsigned)strnlen(stored.wifi[1].ssid, sizeof(stored.wifi[1].ssid)),
                 (unsigned)strnlen(stored.wifi[2].ssid, sizeof(stored.wifi[2].ssid)),
                 (unsigned)strnlen(stored.api_key, sizeof(stored.api_key)));
    } else {
        ESP_LOGI(TAG, "settings saved ssid_len=%u/%u/%u key_len=%u pref_len=%u (secrets not logged)",
                 (unsigned)strnlen(stored.wifi[0].ssid, sizeof(stored.wifi[0].ssid)),
                 (unsigned)strnlen(stored.wifi[1].ssid, sizeof(stored.wifi[1].ssid)),
                 (unsigned)strnlen(stored.wifi[2].ssid, sizeof(stored.wifi[2].ssid)),
                 (unsigned)strnlen(stored.api_key, sizeof(stored.api_key)),
                 (unsigned)strnlen(stored.preference, sizeof(stored.preference)));
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

bool app_store_has_article(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    /* One-byte probe: a real article returns INVALID_LENGTH with the full size. */
    uint8_t probe = 0;
    size_t len = sizeof(probe);
    esp_err_t err = nvs_get_blob(h, "article", &probe, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        char ch = 0;
        len = 1;
        err = nvs_get_str(h, "article", &ch, &len);
        if (err == ESP_OK) {
            nvs_close(h);
            return ch != '\0';
        }
    }
    nvs_close(h);
    if (err == ESP_ERR_NVS_INVALID_LENGTH) return len > 1;
    return err == ESP_OK && len > 1 && probe != 0;
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

/*
 * Debug strings share the 64 KiB NVS partition with Wi-Fi, the article blob,
 * and topic keys. Live entries after a full article plus two prompts and two
 * raw replies stay under half of the ~1890 usable entries (15 pages x 126),
 * including the extra page a raw-blob update needs before the old value is
 * erased. Caps: prompt 2048 (string), raw 8192 (blob).
 */

static void finish_nvs_string(char *buf, size_t buf_sz, size_t len, esp_err_t err)
{
    if (!buf || buf_sz == 0) return;
    if (err != ESP_OK || len == 0) {
        buf[0] = '\0';
        return;
    }
    if (len >= buf_sz) buf[buf_sz - 1] = '\0';
    else if (buf[len - 1] != '\0') buf[len] = '\0';
}

static esp_err_t load_debug_slot(const char *index_key, const char *key0, const char *key1,
                                 bool is_blob, int position, char *buf, size_t buf_sz)
{
    nvs_handle_t h;
    uint8_t packed = 0;
    app_debug_ring_t ring;
    int slot;
    const char *key;
    size_t len;
    esp_err_t err;

    if (!buf || buf_sz == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    if (position < 0 || position >= APP_DEBUG_SLOTS) return ESP_ERR_INVALID_ARG;

    err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_OK;

    (void)nvs_get_u8(h, index_key, &packed);
    app_debug_ring_unpack(packed, &ring);
    slot = app_debug_ring_at(&ring, position);
    if (slot < 0) {
        nvs_close(h);
        return ESP_OK;
    }
    key = slot ? key1 : key0;
    len = buf_sz;
    err = is_blob ? nvs_get_blob(h, key, buf, &len) : nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        return ESP_OK;
    }
    if (err != ESP_OK) {
        buf[0] = '\0';
        return err;
    }
    finish_nvs_string(buf, buf_sz, len, err);
    return ESP_OK;
}

static esp_err_t write_debug_value(nvs_handle_t h, const char *key, bool is_blob,
                                   const char *text, bool keep_tail, size_t cap)
{
    static const size_t shrink[] = { 4096, 2048, 1024, 512 };
    char *stored;
    size_t n;
    esp_err_t err;
    size_t i;

    if (cap == 0) return ESP_ERR_INVALID_ARG;
    stored = malloc(cap);
    if (!stored) return ESP_ERR_NO_MEM;
    n = app_debug_capture(stored, cap, text, keep_tail, false);
    err = is_blob ? nvs_set_blob(h, key, stored, n + 1) : nvs_set_str(h, key, stored);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        (void)nvs_erase_key(h, key);
        err = is_blob ? nvs_set_blob(h, key, stored, n + 1) : nvs_set_str(h, key, stored);
    }
    for (i = 0; err == ESP_ERR_NVS_NOT_ENOUGH_SPACE && i < sizeof(shrink) / sizeof(shrink[0]); i++) {
        size_t next = shrink[i];
        if (next >= cap) continue;
        n = app_debug_capture(stored, next, text, keep_tail, false);
        ESP_LOGW(TAG, "debug %s shrink to %u (len %u)", key, (unsigned)next, (unsigned)n);
        err = is_blob ? nvs_set_blob(h, key, stored, n + 1) : nvs_set_str(h, key, stored);
        cap = next;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "debug %s stored len=%u", key, (unsigned)n);
    } else {
        ESP_LOGW(TAG, "debug %s store failed: %s", key, esp_err_to_name(err));
    }
    free(stored);
    return err;
}

static esp_err_t push_debug_slot(const char *index_key, const char *key0, const char *key1,
                                 bool is_blob, bool keep_tail, size_t cap, const char *text)
{
    nvs_handle_t h;
    uint8_t packed = 0;
    app_debug_ring_t ring;
    int slot;
    const char *key;
    esp_err_t err;

    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    (void)nvs_get_u8(h, index_key, &packed);
    app_debug_ring_unpack(packed, &ring);
    slot = app_debug_ring_push(&ring);
    key = slot ? key1 : key0;
    err = write_debug_value(h, key, is_blob, text, keep_tail, cap);
    if (err == ESP_OK) err = nvs_set_u8(h, index_key, app_debug_ring_pack(&ring));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_store_push_debug_prompt(const char *text)
{
    return push_debug_slot("spi", "sp0", "sp1", false, false, APP_DEBUG_PROMPT_MAX, text);
}

esp_err_t app_store_push_debug_raw(const char *text)
{
    return push_debug_slot("lri", "lr0", "lr1", true, true, APP_DEBUG_RAW_MAX, text);
}

esp_err_t app_store_load_debug_prompt(int position, char *buf, size_t buf_sz)
{
    return load_debug_slot("spi", "sp0", "sp1", false, position, buf, buf_sz);
}

esp_err_t app_store_load_debug_raw(int position, char *buf, size_t buf_sz)
{
    return load_debug_slot("lri", "lr0", "lr1", true, position, buf, buf_sz);
}

int app_store_load_wifi_pref(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return -1;
    esp_err_t err = nvs_get_u8(h, "wpref", &v);
    nvs_close(h);
    if (err != ESP_OK || v < 1 || v > APP_STORE_WIFI_SLOTS) return -1;
    return (int)v - 1;
}

esp_err_t app_store_save_wifi_pref(int slot_index)
{
    if (slot_index < 0 || slot_index >= APP_STORE_WIFI_SLOTS) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "wpref", (uint8_t)(slot_index + 1));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "last-success Wi-Fi slot %d saved", slot_index + 1);
    } else {
        ESP_LOGW(TAG, "last-success Wi-Fi slot save: %s", esp_err_to_name(err));
    }
    return err;
}
