#include "app_prompt.h"
#include "app_cooldown.h"
#include "app_debug_log.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/** Append visible characters. Returns true if input remained that did not fit. */
static bool append_visible(char *dst, size_t cap, size_t *o, const char *s)
{
    if (!s) return false;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        int need;
        int k;
        if (c < 0x20) {
            s++;
            continue;
        }
        need = utf8_len(c);
        if (need < 1) need = 1;
        for (k = 1; k < need; k++) {
            unsigned char n = (unsigned char)s[k];
            if (n == 0 || (n & 0xC0) != 0x80) {
                need = 1;
                break;
            }
        }
        if (*o + (size_t)need >= cap) return true;
        for (k = 0; k < need; k++) dst[(*o)++] = *s++;
    }
    return false;
}

int app_prompt_build_system(char *out, size_t out_sz,
                            const char *preference,
                            const char (*topics)[APP_PROMPT_TOPIC_MAX],
                            uint32_t salt)
{
    char built[APP_PROMPT_SYS_MAX];
    char head[640];
    char avoid[APP_PROMPT_TOPIC_SLOTS * APP_PROMPT_TOPIC_MAX + 64];
    char tail[40];
    size_t o = 0;
    size_t ao = 0;
    bool cut = false;
    const char *pref;
    int i;

    if (!out || out_sz == 0) return -1;
    out[0] = '\0';

    snprintf(head, sizeof(head),
             "你是中文短文写手。写一篇约%d到%d字中文短文，适合蹲厕约3到4分钟。"
             "禁止AI套话开场。先输出正文，正文结束后另起一行，"
             "用##题目##格式写出本篇简短中文主题（两个井号之间只有题目，不要书名号或其他符号）。"
             "读者不应把##行当成正文的一部分。"
             "这次的文章要跟已写过的话题完全无关；在用户偏好领域内大胆换题材、角度与例子，"
             "避免同质化或小幅改写。偏好：",
             APP_ARTICLE_TARGET_MIN, APP_ARTICLE_TARGET_MAX);
    cut = append_visible(built, sizeof(built), &o, head) || cut;

    pref = (preference && preference[0]) ? preference : "轻松有趣的冷知识或生活小故事";
    cut = append_visible(built, sizeof(built), &o, pref) || cut;
    cut = append_visible(built, sizeof(built), &o, "。") || cut;

    avoid[0] = '\0';
    if (topics) {
        for (i = 0; i < APP_PROMPT_TOPIC_SLOTS; i++) {
            int n;
            if (!topics[i][0]) continue;
            /* Trailing ； after every title, including the last, so the
             * next title and the following 种子 stay separated. */
            n = snprintf(avoid + ao, sizeof(avoid) - ao, "%s；", topics[i]);
            if (n < 0) break;
            if ((size_t)n >= sizeof(avoid) - ao) {
                ao = sizeof(avoid) - 1;
                avoid[ao] = '\0';
                break;
            }
            ao += (size_t)n;
        }
    }
    if (avoid[0]) {
        cut = append_visible(built, sizeof(built), &o, "以下话题已写过，勿重复：") || cut;
        cut = append_visible(built, sizeof(built), &o, avoid) || cut;
    }

    snprintf(tail, sizeof(tail), "种子%u。", (unsigned)salt);
    cut = append_visible(built, sizeof(built), &o, tail) || cut;
    built[o] = '\0';

    if (cut) {
        char marked[APP_PROMPT_SYS_MAX];
        app_debug_capture(marked, sizeof(marked), built, false, true);
        app_debug_capture(out, out_sz, marked, false, false);
    } else {
        app_debug_capture(out, out_sz, built, false, false);
    }
    return (int)strlen(out);
}
