#include "app_deepseek.h"
#include "app_article.h"
#include "app_cooldown.h"
#include "app_store.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_tls_errors.h"
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_deepseek";

#define DS_REQ_BUF  3072
/** JSON envelope + ~2000 Chinese chars; keep under SoftAP-free heap peak. */
#define DS_RESP_BUF 14336

static char s_last_detail[120];
static int s_last_http_status;

static void set_detail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_detail, sizeof(s_last_detail), fmt, ap);
    va_end(ap);
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} accum_t;

static void log_heap(const char *step)
{
    ESP_LOGI(TAG, "%s: free=%u largest=%u",
             step,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    accum_t *a = (accum_t *)evt->user_data;
    if (!a) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        ESP_LOGI(TAG, "HTTP connected");
    } else if (evt->event_id == HTTP_EVENT_HEADERS_SENT) {
        ESP_LOGI(TAG, "HTTP headers sent");
    } else if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key) {
        if (strcasecmp(evt->header_key, "Content-Length") == 0) {
            ESP_LOGI(TAG, "Content-Length: %s", evt->header_value ? evt->header_value : "?");
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (a->len + (size_t)evt->data_len + 1 > a->cap) {
            a->overflow = true;
            ESP_LOGE(TAG, "response overflow at %u+%d (cap %u)",
                     (unsigned)a->len, evt->data_len, (unsigned)a->cap);
            return ESP_ERR_NO_MEM;
        }
        memcpy(a->buf + a->len, evt->data, evt->data_len);
        a->len += evt->data_len;
        a->buf[a->len] = '\0';
    } else if (evt->event_id == HTTP_EVENT_ERROR) {
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
    }
    return ESP_OK;
}

static void json_escape(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 2 < out_sz; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (o + 3 >= out_sz) break;
            out[o++] = '\\';
            out[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            continue;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

/** Prefer choices[].message.content over any earlier "content" key. */
static bool extract_content(const char *json, char *out, size_t out_sz)
{
    const char *p = strstr(json, "\"message\"");
    if (p) {
        const char *c = strstr(p, "\"content\"");
        if (c) p = c;
        else p = strstr(json, "\"content\"");
    } else {
        p = strstr(json, "\"content\"");
    }
    if (!p) return false;
    p = strchr(p + 9, '"');
    if (!p) return false;
    p++;
    size_t o = 0;
    while (*p && o + 1 < out_sz) {
        if (*p == '\\' && p[1]) {
            char n = p[1];
            if (n == 'n') { out[o++] = '\n'; p += 2; continue; }
            if (n == 'r') { out[o++] = '\r'; p += 2; continue; }
            if (n == 't') { out[o++] = '\t'; p += 2; continue; }
            if (n == '"' || n == '\\' || n == '/') { out[o++] = n; p += 2; continue; }
            if (n == 'u' && p[2] && p[3] && p[4] && p[5]) {
                char hex[5] = { p[2], p[3], p[4], p[5], 0 };
                unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                if (cp < 0x80) {
                    out[o++] = (char)cp;
                } else if (cp < 0x800 && o + 2 < out_sz) {
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else if (o + 3 < out_sz) {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                p += 6;
                continue;
            }
            p += 2;
            continue;
        }
        if (*p == '"') break;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

bool app_deepseek_extract_topic(char *article, char *topic_out, size_t topic_sz)
{
    if (topic_out && topic_sz) topic_out[0] = '\0';
    if (!article || !topic_out || topic_sz == 0) return false;

    /* Prefer the last ##title## (model instructed to put it after the body). */
    char *open = NULL;
    char *close = NULL;
    for (char *p = article; *p; p++) {
        if (p[0] == '#' && p[1] == '#') {
            char *q = strstr(p + 2, "##");
            if (!q) break;
            open = p;
            close = q;
            p = q + 1; /* continue; loop ++ will advance */
        }
    }
    if (!open || !close || close <= open + 2) return false;

    size_t tlen = (size_t)(close - (open + 2));
    while (tlen && (unsigned char)open[2 + tlen - 1] <= ' ') tlen--;
    size_t start = 0;
    while (start < tlen && (unsigned char)open[2 + start] <= ' ') start++;
    tlen -= start;
    if (tlen == 0) return false;
    if (tlen >= topic_sz) tlen = topic_sz - 1;
    memcpy(topic_out, open + 2 + start, tlen);
    topic_out[tlen] = '\0';

    /* Remove this marker; then scrub any other ##...## left in the body. */
    {
        char *after = close + 2;
        size_t rest = strlen(after);
        memmove(open, after, rest + 1);
    }
    for (;;) {
        char *a = strstr(article, "##");
        if (!a) break;
        char *b = strstr(a + 2, "##");
        if (!b) break;
        char *after = b + 2;
        memmove(a, after, strlen(after) + 1);
    }
    size_t body_len = strlen(article);
    while (body_len && (article[body_len - 1] == '\n' || article[body_len - 1] == '\r'
                        || article[body_len - 1] == ' ' || article[body_len - 1] == '\t')) {
        article[--body_len] = '\0';
    }
    /* Also drop a trailing blank line after removals. */
    while (body_len >= 2 && article[body_len - 1] == '\n' && article[body_len - 2] == '\n') {
        article[--body_len] = '\0';
    }
    return true;
}

const char *app_deepseek_err_msg(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "";
    case ESP_ERR_INVALID_STATE: return "未配置 API Key";
    case ESP_ERR_NO_MEM: return "内存不足，请重试";
    case ESP_ERR_TIMEOUT: return "网络超时";
    case ESP_ERR_NOT_FOUND: return "解析回复失败";
    case ESP_FAIL: return "服务返回错误";
    case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT: return "TLS 连接超时";
    case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME: return "DNS 解析失败";
    case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST: return "无法连接服务器";
    case ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT: return "TLS 握手超时";
    case ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED: return "TLS 握手失败，稍后重试";
    case ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED: return "证书校验失败，等待时钟同步";
    default: return "获取失败，检查网络与密钥";
    }
}


const char *app_deepseek_last_detail(void)
{
    return s_last_detail;
}

int app_deepseek_last_http_status(void)
{
    return s_last_http_status;
}

esp_err_t app_deepseek_fetch(const app_settings_t *settings, char **out_heap)
{
    if (!settings || !out_heap) return ESP_ERR_INVALID_ARG;
    *out_heap = NULL;
    s_last_detail[0] = '\0';
    s_last_http_status = 0;
    if (!settings->has_api_key || settings->api_key[0] == '\0') return ESP_ERR_INVALID_STATE;

    log_heap("fetch start");

    /* Phase A: build request JSON, then free helpers before TLS. */
    char *pref_esc = calloc(1, APP_STORE_PROMPT_MAX * 2);
    char *avoid = calloc(1, APP_STORE_TOPIC_SLOTS * APP_STORE_TOPIC_MAX + 64);
    char *body = malloc(DS_REQ_BUF);
    if (!pref_esc || !avoid || !body) {
        set_detail("req OOM free=%u largest=%u",
                   (unsigned)esp_get_free_heap_size(),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        free(pref_esc); free(avoid); free(body);
        return ESP_ERR_NO_MEM;
    }
    json_escape(settings->preference, pref_esc, APP_STORE_PROMPT_MAX * 2);

    char topics[APP_STORE_TOPIC_SLOTS][APP_STORE_TOPIC_MAX];
    app_store_load_topics(topics);
    size_t ao = 0;
    for (int i = 0; i < APP_STORE_TOPIC_SLOTS; i++) {
        if (!topics[i][0]) continue;
        char esc[APP_STORE_TOPIC_MAX * 2];
        json_escape(topics[i], esc, sizeof(esc));
        int n = snprintf(avoid + ao, APP_STORE_TOPIC_SLOTS * APP_STORE_TOPIC_MAX + 64 - ao,
                         "%s%s", ao ? "；" : "", esc);
        if (n > 0) ao += (size_t)n;
    }

    uint32_t salt = (uint32_t)(esp_timer_get_time() / 1000);
    const char *pref = pref_esc[0] ? pref_esc : "轻松有趣的冷知识或生活小故事";
    int blen = snprintf(body, DS_REQ_BUF,
             "{\"model\":\"deepseek-chat\",\"messages\":["
             "{\"role\":\"system\",\"content\":"
             "\"你是中文短文写手。写一篇约%d到%d字中文短文，适合蹲厕约3到4分钟。"
             "禁止AI套话开场。先输出正文，正文结束后另起一行，"
             "用##题目##格式写出本篇简短中文主题（两个井号之间只有题目，不要书名号或其他符号）。"
             "读者不应把##行当成正文的一部分。"
             "这次的文章要跟已写过的话题完全无关；在用户偏好领域内大胆换题材、角度与例子，"
             "避免同质化或小幅改写。偏好：%s。%s%s种子%u。\"},"
             "{\"role\":\"user\",\"content\":\"请直接写正文，最后一行用##题目##标注主题。\"}"
             "],\"temperature\":1.0,\"max_tokens\":2400}",
             APP_ARTICLE_TARGET_MIN, APP_ARTICLE_TARGET_MAX,
             pref,
             avoid[0] ? "以下话题已写过，勿重复：" : "",
             avoid[0] ? avoid : "",
             (unsigned)salt);
    free(pref_esc);
    free(avoid);
    pref_esc = avoid = NULL;
    if (blen < 0 || blen >= DS_REQ_BUF) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "request JSON %d bytes", blen);
    log_heap("after req build");

    /* Phase B: response buffer + HTTPS (peak TLS + resp only). */
    char *resp = malloc(DS_RESP_BUF);
    char *auth = calloc(1, APP_STORE_API_KEY_MAX + 32);
    if (!resp || !auth) {
        set_detail("resp OOM free=%u largest=%u",
                   (unsigned)esp_get_free_heap_size(),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        free(body); free(resp); free(auth);
        return ESP_ERR_NO_MEM;
    }
    accum_t acc = { .buf = resp, .cap = DS_RESP_BUF, .len = 0, .overflow = false };
    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .event_handler = http_event,
        .user_data = &acc,
        .timeout_ms = 60000,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Home routers often lack a working IPv6 route; AAAA-first fails TLS. */
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body); free(resp); free(auth);
        return ESP_ERR_NO_MEM;
    }
    snprintf(auth, APP_STORE_API_KEY_MAX + 32, "Bearer %s", settings->api_key);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    free(auth);
    auth = NULL;

    log_heap("before http perform");
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);
    body = NULL;

    ESP_LOGI(TAG, "perform err=%s status=%d len=%u ovf=%d dt=%dms",
             esp_err_to_name(err), status, (unsigned)acc.len, (int)acc.overflow, (int)dt_ms);
    log_heap("after http");

    s_last_http_status = status;
    if (acc.overflow) {
        set_detail("回复过大 heap_free=%u", (unsigned)esp_get_free_heap_size());
        free(resp);
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        set_detail("err=%s heap=%u", esp_err_to_name(err),
                   (unsigned)esp_get_free_heap_size());
        free(resp);
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP %d head=%.100s", status, resp);
        if (status == 401) set_detail("HTTP 401 密钥无效");
        else if (status == 402) set_detail("HTTP 402 余额不足");
        else if (status == 429) set_detail("HTTP 429 请求过频");
        else set_detail("HTTP %d", status);
        free(resp);
        return ESP_FAIL;
    }
    char *out = malloc(APP_ARTICLE_MAX_BYTES);
    if (!out) {
        free(resp);
        return ESP_ERR_NO_MEM;
    }
    out[0] = '\0';
    if (!extract_content(resp, out, APP_ARTICLE_MAX_BYTES)) {
        ESP_LOGE(TAG, "parse fail head=%.120s", resp);
        free(resp);
        free(out);
        return ESP_ERR_NOT_FOUND;
    }
    free(resp);
    *out_heap = out;
    ESP_LOGI(TAG, "ok codepoints~%u",
             (unsigned)app_utf8_codepoints(out, strnlen(out, APP_ARTICLE_MAX_BYTES)));
    return ESP_OK;
}
