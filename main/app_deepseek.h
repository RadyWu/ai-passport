#pragma once
#include "app_store.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

/**
 * Fetch article. On success *out_heap is malloc'd (caller frees) with text.
 * During TLS only the response buffer is held — article buffer allocated after HTTP.
 * Strips a trailing ##题目## marker from the body when present.
 */
esp_err_t app_deepseek_fetch(const app_settings_t *settings, char **out_heap);

/**
 * Find the last ##题目## marker in article, copy the title into topic_out,
 * and remove the marker (plus surrounding blank lines) from article in place.
 * Returns true if a non-empty title was extracted.
 */
bool app_deepseek_extract_topic(char *article, char *topic_out, size_t topic_sz);

/** Short Chinese UI message for a fetch error (never NULL). */
const char *app_deepseek_err_msg(esp_err_t err);
/** Extra detail from last fetch (HTTP status / heap); never NULL, may be "". */
const char *app_deepseek_last_detail(void);
/** Last HTTP status from perform (0 if none). */
int app_deepseek_last_http_status(void);
