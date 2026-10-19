#include "app_save_policy.h"
#include <assert.h>
#include <stdio.h>

static app_save_facts_t facts(bool connected, bool unchanged, bool driver_match,
                              bool has_article, bool ssid)
{
    app_save_facts_t in = {
        .sta_connected = connected,
        .wifi_unchanged = unchanged,
        .driver_matches_new = driver_match,
        .has_stored_article = has_article,
        .ssid_nonempty = ssid,
    };
    return in;
}

int main(void)
{
    app_save_action_t a;

    /* First SoftAP provision: no article yet, radio not on a LAN. */
    a = app_save_decide(facts(false, false, false, false, true));
    assert(!a.stay_connected);
    assert(a.auto_fetch);
    assert(a.apply_sta);

    /* First save already online with the same credentials (driver agrees). */
    a = app_save_decide(facts(true, true, true, false, true));
    assert(a.stay_connected);
    assert(a.auto_fetch);
    assert(!a.apply_sta);

    /* Wrong-password retry before any article exists: still the first fetch. */
    a = app_save_decide(facts(false, false, false, false, true));
    assert(a.auto_fetch);
    assert(a.apply_sta);

    /* Outage / new SSID while an article is on screen: reconnect only. */
    a = app_save_decide(facts(false, false, false, true, true));
    assert(!a.stay_connected);
    assert(!a.auto_fetch);
    assert(a.apply_sta);

    /* Re-save of the same SSID while STA is down must still kick a new join. */
    a = app_save_decide(facts(false, true, false, true, true));
    assert(!a.stay_connected);
    assert(!a.auto_fetch);
    assert(a.apply_sta);

    /* LAN edit, same Wi-Fi, article already stored: no radio bounce, no fetch. */
    a = app_save_decide(facts(true, true, false, true, true));
    assert(a.stay_connected);
    assert(!a.auto_fetch);
    assert(!a.apply_sta);

    /* Online, but the form names a different network: leave the old AP. */
    a = app_save_decide(facts(true, false, false, true, true));
    assert(!a.stay_connected);
    assert(!a.auto_fetch);
    assert(a.apply_sta);

    /* Driver already holds the submitted profile and is connected. */
    a = app_save_decide(facts(true, false, true, true, true));
    assert(a.stay_connected);
    assert(!a.apply_sta);

    /* Empty SSID never starts STA. */
    a = app_save_decide(facts(false, false, false, false, false));
    assert(!a.apply_sta);
    assert(a.auto_fetch);

    puts("test_app_save_policy: PASS");
    return 0;
}
