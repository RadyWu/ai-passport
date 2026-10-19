#include "app_article.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_no_loss(const char *text, size_t width, size_t glyph, size_t lines)
{
    app_article_t art;
    memset(&art, 0, sizeof(art));
    strncpy(art.text, text, sizeof(art.text) - 1);
    assert(app_article_paginate_fitted(&art, width, glyph, lines));
    /* Concatenate all pages — must equal original (byte-identical). */
    char rebuilt[APP_ARTICLE_MAX_BYTES];
    size_t o = 0;
    for (unsigned p = 0; p < art.page_count; p++) {
        char page[APP_PAGE_MAX_CHARS * 3 + 8];
        assert(app_article_get_page(&art, p, page, sizeof(page)));
        size_t n = strlen(page);
        assert(o + n < sizeof(rebuilt));
        memcpy(rebuilt + o, page, n);
        o += n;
    }
    rebuilt[o] = '\0';
    assert(strcmp(rebuilt, text) == 0);
}

int main(void)
{
    app_article_t art;
    memset(&art, 0, sizeof(art));
    const char *s = "一二三四五六七八九十"; /* 10 chars */
    strncpy(art.text, s, sizeof(art.text) - 1);
    assert(app_utf8_codepoints(art.text, strlen(art.text)) == 10);
    assert(app_article_paginate(&art, 4));
    assert(art.page_count == 3);
    char page[64];
    assert(app_article_get_page(&art, 0, page, sizeof(page)));
    assert(app_utf8_codepoints(page, strlen(page)) == 4);

    /* Fitted: width 40 / glyph 10 → 4 CJK/line; 2 lines/page → 8 chars/page */
    check_no_loss(s, 40, 10, 2);
    memset(&art, 0, sizeof(art));
    strncpy(art.text, s, sizeof(art.text) - 1);
    assert(app_article_paginate_fitted(&art, 40, 10, 2));
    assert(art.page_count == 2);
    assert(app_article_get_page(&art, 0, page, sizeof(page)));
    assert(app_utf8_codepoints(page, strlen(page)) == 8);
    assert(app_article_get_page(&art, 1, page, sizeof(page)));
    assert(app_utf8_codepoints(page, strlen(page)) == 2);

    /* Newlines + wrap: nothing lost across pages */
    const char *longish =
        "甲乙丙丁戊己庚辛\n壬癸子丑寅卯辰巳\n午未申酉戌亥天地"
        "人你我他上下左右前后东西南北春夏秋冬";
    check_no_loss(longish, 40, 10, 3);

    /* Last line that cannot fit moves wholly — first page ends on boundary */
    memset(&art, 0, sizeof(art));
    strncpy(art.text, "一二三四五六七八", sizeof(art.text) - 1); /* 8 chars */
    assert(app_article_paginate_fitted(&art, 40, 10, 1)); /* 4 chars/line, 1 line/page */
    assert(art.page_count == 2);
    assert(app_article_get_page(&art, 0, page, sizeof(page)));
    assert(app_utf8_codepoints(page, strlen(page)) == 4);
    assert(app_article_get_page(&art, 1, page, sizeof(page)));
    assert(app_utf8_codepoints(page, strlen(page)) == 4);
    check_no_loss("一二三四五六七八", 40, 10, 1);


    /* Guard: struct is too large for ESP32-C3 8KB task stacks (was causing reboot
     * after fetch when do_fetch put app_article_t on the worker stack). */
    assert(sizeof(app_article_t) > 4096);
    {
        app_article_t *art = calloc(1, sizeof(*art));
        assert(art);
        const char *body =
            "这是一篇用于堆上分页的较长示意短文，确保不在栈上分配整篇结构体。"
            "abcdefghijKLMNOP" /* mix ASCII */
            "一二三四五六七八九十百千万";
        strncpy(art->text, body, sizeof(art->text) - 1);
        assert(app_article_paginate_fitted(art, APP_BODY_WIDTH_PX, APP_BODY_GLYPH_PX, APP_BODY_LINES));
        assert(art->page_count >= 1);
        char page[APP_PAGE_MAX_CHARS * 3 + 8];
        assert(app_article_get_page(art, 0, page, sizeof(page)));
        assert(page[0] != '\0');
        free(art);
    }

    puts("test_app_article: PASS");
    return 0;
}
