#pragma once
#include <stdbool.h>

/**
 * What a real config-page save should do.
 *
 * auto_fetch is only the first successful article: once a short text is stored,
 * later saves (new Wi-Fi, corrected password, LAN edits) must not replace it.
 * The reader may still be on that page. Long-press OK remains the manual pull.
 *
 * apply_sta means the radio must drop any in-progress STA attempt and install
 * this SSID/password before connect. NVS alone is not enough: a SoftAP keepalive
 * that is already associating keeps the previous profile until disconnect.
 */
typedef struct {
    bool sta_connected;
    bool wifi_unchanged;
    /** Driver STA profile already equals the submitted SSID and password. */
    bool driver_matches_new;
    /** A fetched article is already in NVS (not the on-device sample). */
    bool has_stored_article;
    bool ssid_nonempty;
} app_save_facts_t;

typedef struct {
    /** STA is already on this network; do not bounce the radio. */
    bool stay_connected;
    /** Arm the one-shot post-save DeepSeek fetch. */
    bool auto_fetch;
    /** Disconnect, set the new STA profile, and connect. */
    bool apply_sta;
} app_save_action_t;

static inline app_save_action_t app_save_decide(app_save_facts_t in)
{
    app_save_action_t out = {0};
    out.stay_connected = in.sta_connected
        && (in.wifi_unchanged || in.driver_matches_new);
    out.auto_fetch = !in.has_stored_article;
    out.apply_sta = !out.stay_connected && in.ssid_nonempty;
    return out;
}
