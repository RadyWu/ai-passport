#include "app_cooldown.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(app_cooldown_remaining_minutes(0, 100) == 0);
    assert(app_cooldown_ready(0, 100));
    /* last=1000, now=1000+4min => remain 6 min (10 min cooldown) */
    assert(app_cooldown_remaining_minutes(1000, 1000 + 4 * 60) == 6);
    /* exactly expired */
    assert(app_cooldown_remaining_minutes(1000, 1000 + 10 * 60) == 0);
    /* 1 second left => ceil to 1 minute */
    assert(app_cooldown_remaining_minutes(1000, 1000 + 10 * 60 - 1) == 1);

    char buf[16];
    app_format_session_mss(buf, sizeof(buf), 0);
    assert(strcmp(buf, "0:00") == 0);
    app_format_session_mss(buf, sizeof(buf), 65);
    assert(strcmp(buf, "1:05") == 0);
    app_format_session_mss(buf, sizeof(buf), 5 * 60);
    assert(strcmp(buf, "5:00") == 0);
    puts("test_app_cooldown: PASS");
    return 0;
}
