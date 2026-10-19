#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/** ≥2200–2500 汉字 headroom (UTF-8 ≈3 B/字) + NUL; NVS blob. */
#define APP_ARTICLE_MAX_BYTES 8192
#define APP_PAGE_MAX_CHARS 480
#define APP_PAGE_MAX_COUNT 96

/** Body layout — must match app_ui body box + font_toilet_20 metrics. */
#define APP_BODY_WIDTH_PX   200
#define APP_BODY_GLYPH_PX   20
#define APP_BODY_LINE_H     24
#define APP_BODY_LINES      10

typedef struct {
    char text[APP_ARTICLE_MAX_BYTES];
    size_t len;
    uint16_t page_off[APP_PAGE_MAX_COUNT];
    uint16_t page_len[APP_PAGE_MAX_COUNT];
    uint8_t page_count;
} app_article_t;

size_t app_utf8_codepoints(const char *s, size_t byte_len);

bool app_article_paginate(app_article_t *art, size_t max_chars_per_page);

/**
 * Wrap into lines that fit max_width_px, then pack exactly lines_per_page
 * lines per page. A line that does not fully fit starts the next page.
 * No characters are discarded while page_count < APP_PAGE_MAX_COUNT.
 */
bool app_article_paginate_fitted(app_article_t *art,
                                 size_t max_width_px,
                                 size_t glyph_px,
                                 size_t lines_per_page);

bool app_article_get_page(const app_article_t *art, unsigned page,
                          char *out, size_t out_size);
