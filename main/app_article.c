#include "app_article.h"
#include <stdlib.h>
#include <string.h>

#define APP_WRAP_LINES_MAX (APP_PAGE_MAX_COUNT * 12)

static size_t utf8_char_len(unsigned char c)
{
    if ((c & 0x80u) == 0) return 1;
    if ((c & 0xE0u) == 0xC0u) return 2;
    if ((c & 0xF0u) == 0xE0u) return 3;
    if ((c & 0xF8u) == 0xF0u) return 4;
    return 1;
}

static size_t bounded_len(const char *s, size_t maxn)
{
    size_t i = 0;
    while (i < maxn && s[i]) i++;
    return i;
}

size_t app_utf8_codepoints(const char *s, size_t byte_len)
{
    size_t n = 0, i = 0;
    while (i < byte_len && s[i]) {
        size_t cl = utf8_char_len((unsigned char)s[i]);
        if (i + cl > byte_len) break;
        i += cl;
        n++;
    }
    return n;
}

bool app_article_paginate(app_article_t *art, size_t max_chars_per_page)
{
    if (!art || max_chars_per_page == 0) return false;
    art->page_count = 0;
    art->len = bounded_len(art->text, APP_ARTICLE_MAX_BYTES - 1);
    art->text[art->len] = '\0';
    if (art->len == 0) return false;

    size_t i = 0;
    while (i < art->len && art->page_count < APP_PAGE_MAX_COUNT) {
        size_t start = i;
        size_t chars = 0;
        while (i < art->len && chars < max_chars_per_page) {
            size_t cl = utf8_char_len((unsigned char)art->text[i]);
            if (i + cl > art->len) break;
            i += cl;
            chars++;
        }
        art->page_off[art->page_count] = (uint16_t)start;
        art->page_len[art->page_count] = (uint16_t)(i - start);
        art->page_count++;
        if (i == start) break;
    }
    return art->page_count > 0;
}

static size_t advance_px(const char *s, size_t cl, size_t glyph_px)
{
    unsigned char c = (unsigned char)s[0];
    /* font_toilet_20: CJK adv is 20px; Latin is often 11–14px and some ~18px.
     * Old half-width (~10) packed too many ASCII chars per line, so LVGL wrapped
     * past APP_BODY_LINES and the last 1–2 glyphs looked "swallowed". */
    if (cl == 1 && c < 0x80) {
        if (c == 'W' || c == 'M' || c == 'w' || c == 'm' ||
            c == '@' || c == '%' || c == 'Q' || c == '#') {
            return glyph_px;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9')) {
            return (glyph_px * 3) / 4; /* ~15px at glyph 20 */
        }
        return (glyph_px + 1) / 2; /* narrow punctuation / space */
    }
    return glyph_px;
}

bool app_article_paginate_fitted(app_article_t *art,
                                 size_t max_width_px,
                                 size_t glyph_px,
                                 size_t lines_per_page)
{
    if (!art || max_width_px == 0 || glyph_px == 0 || lines_per_page == 0) {
        return false;
    }
    art->page_count = 0;
    art->len = bounded_len(art->text, APP_ARTICLE_MAX_BYTES - 1);
    art->text[art->len] = '\0';
    if (art->len == 0) return false;

    /* Pass 1: wrap into line byte ranges [off, len). Heap to avoid large stack. */
    uint16_t *line_off = (uint16_t *)malloc(APP_WRAP_LINES_MAX * sizeof(uint16_t));
    uint16_t *line_len = (uint16_t *)malloc(APP_WRAP_LINES_MAX * sizeof(uint16_t));
    if (!line_off || !line_len) {
        free(line_off);
        free(line_len);
        return app_article_paginate(art, 80); /* degraded fallback */
    }
    size_t nlines = 0;

    size_t i = 0;
    while (i < art->len && nlines < APP_WRAP_LINES_MAX) {
        size_t start = i;
        size_t line_w = 0;

        if ((unsigned char)art->text[i] == '\n') {
            /* Empty visual line from explicit newline (consume the \n). */
            line_off[nlines] = (uint16_t)start;
            line_len[nlines] = 1;
            nlines++;
            i++;
            continue;
        }

        while (i < art->len) {
            unsigned char c = (unsigned char)art->text[i];
            if (c == '\n') {
                /* Soft end of line; keep \n with this line so next page/line
                 * does not re-show it. */
                i++;
                break;
            }
            size_t cl = utf8_char_len(c);
            if (i + cl > art->len) break;
            size_t adv = advance_px(art->text + i, cl, glyph_px);
            if (adv > max_width_px) adv = max_width_px;

            if (line_w > 0 && line_w + adv > max_width_px) {
                /* Wrap before this char — char starts the next line. */
                break;
            }
            line_w += adv;
            i += cl;
        }

        line_off[nlines] = (uint16_t)start;
        line_len[nlines] = (uint16_t)(i - start);
        nlines++;
        if (i == start) {
            /* Safety: force progress on pathological input. */
            size_t cl = utf8_char_len((unsigned char)art->text[i]);
            if (i + cl > art->len) cl = 1;
            i += cl;
        }
    }

    /* Pass 2: pack lines into pages of at most lines_per_page. */
    size_t li = 0;
    while (li < nlines && art->page_count < APP_PAGE_MAX_COUNT) {
        size_t take = lines_per_page;
        if (li + take > nlines) take = nlines - li;
        size_t page_start = line_off[li];
        size_t last = li + take - 1;
        size_t page_end = (size_t)line_off[last] + (size_t)line_len[last];
        art->page_off[art->page_count] = (uint16_t)page_start;
        art->page_len[art->page_count] = (uint16_t)(page_end - page_start);
        art->page_count++;
        li += take;
    }

    /* If wrap lines exceeded buffer or pages exhausted mid-text, still report
     * what we have; callers may log. Remaining bytes after last page are only
     * possible when APP_PAGE_MAX_COUNT / APP_WRAP_LINES_MAX is hit. */
    free(line_off);
    free(line_len);
    return art->page_count > 0;
}

bool app_article_get_page(const app_article_t *art, unsigned page,
                          char *out, size_t out_size)
{
    if (!art || !out || out_size == 0) return false;
    if (page >= art->page_count) return false;
    size_t n = art->page_len[page];
    if (n >= out_size) n = out_size - 1;
    memcpy(out, art->text + art->page_off[page], n);
    out[n] = '\0';
    return true;
}
