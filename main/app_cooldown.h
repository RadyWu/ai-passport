#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define APP_COOLDOWN_SEC (10 * 60)
#define APP_SESSION_REMIND_SEC (5 * 60)
#define APP_IDLE_SLEEP_SEC (10 * 60)
#define APP_ARTICLE_TARGET_MIN 1500
#define APP_ARTICLE_TARGET_MAX 2000

/** Remaining whole minutes until cooldown ends (ceil). 0 if ready. */
static inline int app_cooldown_remaining_minutes(int64_t last_fetch_unix_sec,
                                                 int64_t now_unix_sec)
{
    if (last_fetch_unix_sec <= 0) return 0;
    int64_t elapsed = now_unix_sec - last_fetch_unix_sec;
    if (elapsed < 0) elapsed = 0;
    int64_t remain = (int64_t)APP_COOLDOWN_SEC - elapsed;
    if (remain <= 0) return 0;
    return (int)((remain + 59) / 60);
}

static inline bool app_cooldown_ready(int64_t last_fetch_unix_sec,
                                      int64_t now_unix_sec)
{
    return app_cooldown_remaining_minutes(last_fetch_unix_sec, now_unix_sec) == 0;
}

/** Format session timer as M:SS (minutes may exceed 9). buf >= 8 */
static inline void app_format_session_mss(char *buf, unsigned buf_len,
                                          int64_t session_sec)
{
    if (!buf || buf_len < 4) return;
    if (session_sec < 0) session_sec = 0;
    int m = (int)(session_sec / 60);
    int s = (int)(session_sec % 60);
    if (m > 999) m = 999;
    snprintf(buf, buf_len, "%d:%02d", m, s);
}
