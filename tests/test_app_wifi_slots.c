#include "app_wifi_slots.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static app_wifi_cred_t cred(const char *ssid, const char *pass)
{
    app_wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    if (ssid) strncpy(c.ssid, ssid, sizeof(c.ssid) - 1);
    if (pass) strncpy(c.pass, pass, sizeof(c.pass) - 1);
    return c;
}

static void set_slot(app_wifi_cred_t slots[APP_WIFI_SLOTS], int i,
                     const char *ssid, const char *pass)
{
    slots[i] = cred(ssid, pass);
}

static app_wifi_fail_action_t fail_n(const app_wifi_cred_t slots[APP_WIFI_SLOTS],
                                     int slot, int n, int budget, int *next)
{
    return app_wifi_on_fail(slots, slot, n, budget, next);
}

int main(void)
{
    app_wifi_cred_t slots[APP_WIFI_SLOTS];
    memset(slots, 0, sizeof(slots));

    /* No networks: nothing to try. */
    assert(app_wifi_first_slot(slots) == -1);
    assert(app_wifi_next_slot(slots, -1) == -1);

    /* One filled SSID is valid, wherever it sits. Empty slots are skipped. */
    set_slot(slots, 1, "cafe", "pw");
    assert(app_wifi_first_slot(slots) == 1);
    assert(app_wifi_next_slot(slots, 1) == -1);
    assert(app_wifi_slot_filled(&slots[0]) == false);

    /* Order is always 1 → 2 → 3 among non-empty slots, including a hole. */
    set_slot(slots, 1, "", "");
    set_slot(slots, 0, "home", "h");
    set_slot(slots, 2, "office", "o");
    assert(app_wifi_first_slot(slots) == 0);
    assert(app_wifi_next_slot(slots, 0) == 2);
    assert(app_wifi_next_slot(slots, 2) == -1);

    int next = -1;
    /* Budget stays on the same slot, then advances, then reports all failed. */
    assert(fail_n(slots, 0, 1, 3, &next) == APP_WIFI_FAIL_KEEP);
    assert(next == 0);
    assert(fail_n(slots, 0, 2, 3, &next) == APP_WIFI_FAIL_KEEP);
    assert(fail_n(slots, 0, 3, 3, &next) == APP_WIFI_FAIL_ADVANCE);
    assert(next == 2);
    assert(fail_n(slots, 2, 3, 3, &next) == APP_WIFI_FAIL_EXHAUSTED);
    assert(next == 0);

    /* A single slot exhausts in place and wraps to itself for continued search. */
    memset(slots, 0, sizeof(slots));
    set_slot(slots, 0, "only", "x");
    assert(fail_n(slots, 0, 3, 3, &next) == APP_WIFI_FAIL_EXHAUSTED);
    assert(next == 0);

    /* All three filled: 0 then 1 then 2, and only then exhausted. */
    set_slot(slots, 1, "two", "y");
    set_slot(slots, 2, "three", "z");
    assert(fail_n(slots, 0, 3, 3, &next) == APP_WIFI_FAIL_ADVANCE);
    assert(next == 1);
    assert(fail_n(slots, 1, 3, 3, &next) == APP_WIFI_FAIL_ADVANCE);
    assert(next == 2);
    assert(fail_n(slots, 2, 1, 3, &next) == APP_WIFI_FAIL_KEEP);
    assert(next == 2);
    assert(fail_n(slots, 2, 3, 3, &next) == APP_WIFI_FAIL_EXHAUSTED);
    assert(next == 0);

    /* Merge: empty SSID clears the slot. */
    app_wifi_cred_t dst;
    app_wifi_cred_t prev = cred("old", "secret");
    assert(app_wifi_merge_slot(&dst, "", "", false, &prev) == APP_WIFI_MERGE_OK);
    assert(dst.ssid[0] == '\0');
    assert(dst.pass[0] == '\0');

    /* Blank password keeps the saved one (password inputs are not prefilled). */
    assert(app_wifi_merge_slot(&dst, "old", "", false, &prev) == APP_WIFI_MERGE_OK);
    assert(strcmp(dst.ssid, "old") == 0);
    assert(strcmp(dst.pass, "secret") == 0);

    /* New SSID with a typed password replaces both. */
    assert(app_wifi_merge_slot(&dst, "new", "fresh", false, &prev) == APP_WIFI_MERGE_OK);
    assert(strcmp(dst.ssid, "new") == 0);
    assert(strcmp(dst.pass, "fresh") == 0);

    /* First-time SSID with no password is rejected. */
    app_wifi_cred_t empty = cred("", "");
    assert(app_wifi_merge_slot(&dst, "new", "", false, &empty) == APP_WIFI_MERGE_NEED_PASS);

    /* A space (or whitespace-only) password is an open network, not "keep old". */
    assert(app_wifi_pass_is_open_marker(" "));
    assert(app_wifi_pass_is_open_marker(" \t"));
    assert(!app_wifi_pass_is_open_marker(""));
    assert(!app_wifi_pass_is_open_marker(" x"));
    assert(app_wifi_merge_slot(&dst, "open", "", true, &prev) == APP_WIFI_MERGE_OK);
    assert(strcmp(dst.ssid, "open") == 0);
    assert(dst.pass[0] == '\0');

    /* Equality is the whole set, so a backup-slot edit is a real change. */
    app_wifi_cred_t a[APP_WIFI_SLOTS];
    app_wifi_cred_t b[APP_WIFI_SLOTS];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    set_slot(a, 0, "home", "h");
    set_slot(b, 0, "home", "h");
    assert(app_wifi_sets_equal(a, b));
    set_slot(b, 2, "office", "o");
    assert(!app_wifi_sets_equal(a, b));

    /*
     * Stay associated only when Wi-Fi 1 matches the driver and backups are
     * unchanged. Editing Wi-Fi 2 must re-apply from slot 1.
     */
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    set_slot(a, 0, "home", "h");
    set_slot(b, 0, "home", "h");
    assert(app_wifi_driver_keeps_link(a, b, "home", "h"));
    set_slot(a, 1, "cafe", "c");
    assert(!app_wifi_driver_keeps_link(a, b, "home", "h"));
    set_slot(b, 1, "cafe", "c");
    assert(app_wifi_driver_keeps_link(a, b, "home", "h"));
    assert(!app_wifi_driver_keeps_link(a, b, "cafe", "c"));
    memset(a, 0, sizeof(a));
    set_slot(a, 1, "cafe", "c");
    assert(!app_wifi_driver_keeps_link(a, b, "cafe", "c"));

    /*
     * Scan-first order: last success first when that SSID is visible,
     * then other visible slots in 1 → 2 → 3. Invisible slots are omitted.
     */
    const char *all_vis[] = {"home", "cafe", "office"};
    int order[APP_WIFI_SLOTS];
    memset(slots, 0, sizeof(slots));
    set_slot(slots, 0, "home", "h");
    set_slot(slots, 1, "cafe", "c");
    set_slot(slots, 2, "office", "o");

    assert(app_wifi_visible_try_order(slots, all_vis, 3, APP_WIFI_PREF_NONE, order) == 3);
    assert(order[0] == 0 && order[1] == 1 && order[2] == 2);

    assert(app_wifi_visible_try_order(slots, all_vis, 3, 2, order) == 3);
    assert(order[0] == 2 && order[1] == 0 && order[2] == 1);

    /* Last success out of range: only the visible filled slots, still 1 then 2. */
    const char *home_cafe[] = {"cafe", "home"};
    assert(app_wifi_visible_try_order(slots, home_cafe, 2, 2, order) == 2);
    assert(order[0] == 0 && order[1] == 1);

    /* Only Wi-Fi 3 is in range. Last success uses it; an older preference does not. */
    const char *office_only[] = {"office", "neighbor"};
    assert(app_wifi_visible_try_order(slots, office_only, 2, 2, order) == 1);
    assert(order[0] == 2);
    assert(app_wifi_visible_try_order(slots, office_only, 2, 0, order) == 1);
    assert(order[0] == 2);

    /* Nothing saved is visible: do not connect. */
    const char *strangers[] = {"other", "guest"};
    assert(app_wifi_visible_try_order(slots, strangers, 2, 2, order) == 0);
    assert(app_wifi_visible_try_order(slots, NULL, 0, 0, order) == 0);

    /* No preference yet: visible slots keep 1 → 2 → 3, holes skipped. */
    memset(slots, 0, sizeof(slots));
    set_slot(slots, 0, "home", "h");
    set_slot(slots, 2, "office", "o");
    const char *home_office[] = {"office", "home"};
    assert(app_wifi_visible_try_order(slots, home_office, 2, APP_WIFI_PREF_NONE, order) == 2);
    assert(order[0] == 0 && order[1] == 2);

    /* Preferred slot empty or out of range is ignored. */
    assert(app_wifi_visible_try_order(slots, home_office, 2, 1, order) == 2);
    assert(order[0] == 0 && order[1] == 2);
    assert(app_wifi_visible_try_order(slots, home_office, 2, 9, order) == 2);
    assert(order[0] == 0 && order[1] == 2);

    /* Names are case-sensitive; a prefix is not a match. */
    const char *almost[] = {"Home", "home-2"};
    assert(!app_wifi_ssid_visible("home", almost, 2));
    assert(app_wifi_visible_try_order(slots, almost, 2, 0, order) == 0);
    const char *dup_home[] = {"home", "home"};
    assert(app_wifi_ssid_visible("home", dup_home, 2));

    /* Fallback walks the visible list once and stops. */
    int vis_order[] = {2, 0, 1};
    assert(app_wifi_order_after(vis_order, 3, 2) == 0);
    assert(app_wifi_order_after(vis_order, 3, 0) == 1);
    assert(app_wifi_order_after(vis_order, 3, 1) == -1);
    assert(app_wifi_order_after(vis_order, 3, 4) == -1);
    assert(app_wifi_order_after(vis_order, 0, 2) == -1);

    puts("test_app_wifi_slots: PASS");
    return 0;
}
