#pragma once
/**
 * Ordered Wi-Fi credential slots (exactly 3).
 *
 * Host-testable: no ESP-IDF or NVS. Slot 0 is Wi-Fi 1 and is tried first.
 * Empty SSIDs are skipped. A join budget is per slot; the all-failed result
 * happens only after every filled slot has used its budget, walking 1 → 2 → 3.
 */
#include <stdbool.h>
#include <string.h>

#define APP_WIFI_SLOTS 3
#define APP_WIFI_SSID_MAX 33
#define APP_WIFI_PASS_MAX 65

typedef struct {
    char ssid[APP_WIFI_SSID_MAX];
    char pass[APP_WIFI_PASS_MAX];
} app_wifi_cred_t;

static inline bool app_wifi_slot_filled(const app_wifi_cred_t *slot)
{
    return slot && slot->ssid[0] != '\0';
}

/** Index of the first non-empty SSID, or -1 when every slot is empty. */
static inline int app_wifi_first_slot(const app_wifi_cred_t slots[APP_WIFI_SLOTS])
{
    if (!slots) return -1;
    for (int i = 0; i < APP_WIFI_SLOTS; i++) {
        if (app_wifi_slot_filled(&slots[i])) return i;
    }
    return -1;
}

/** Next non-empty slot strictly after `after`, or -1. Does not wrap. */
static inline int app_wifi_next_slot(const app_wifi_cred_t slots[APP_WIFI_SLOTS], int after)
{
    if (!slots) return -1;
    for (int i = after + 1; i < APP_WIFI_SLOTS; i++) {
        if (app_wifi_slot_filled(&slots[i])) return i;
    }
    return -1;
}

static inline bool app_wifi_creds_equal(const app_wifi_cred_t *a, const app_wifi_cred_t *b)
{
    if (!a || !b) return false;
    return strncmp(a->ssid, b->ssid, APP_WIFI_SSID_MAX) == 0
        && strncmp(a->pass, b->pass, APP_WIFI_PASS_MAX) == 0;
}

static inline bool app_wifi_sets_equal(const app_wifi_cred_t a[APP_WIFI_SLOTS],
                                       const app_wifi_cred_t b[APP_WIFI_SLOTS])
{
    if (!a || !b) return false;
    for (int i = 0; i < APP_WIFI_SLOTS; i++) {
        if (!app_wifi_creds_equal(&a[i], &b[i])) return false;
    }
    return true;
}

/**
 * Whitespace-only password means "open network" (the config page tells the
 * user to type a space). Completely empty is different: keep the saved
 * password, because password inputs are not prefilled.
 */
static inline bool app_wifi_pass_is_open_marker(const char *pass)
{
    if (!pass || pass[0] == '\0') return false;
    for (const unsigned char *p = (const unsigned char *)pass; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return false;
    }
    return true;
}

typedef enum {
    APP_WIFI_MERGE_OK = 0,
    /** Non-empty SSID, no submitted password, and nothing saved to keep. */
    APP_WIFI_MERGE_NEED_PASS,
} app_wifi_merge_status_t;

/**
 * Merge one submitted slot onto `dst`.
 * - Empty SSID clears the slot (not tried).
 * - Open-network marker stores an empty password and does not keep the old one.
 * - Empty password keeps `prev` password when that slot already had one.
 */
static inline app_wifi_merge_status_t app_wifi_merge_slot(
    app_wifi_cred_t *dst,
    const char *ssid_in,
    const char *pass_in,
    bool open_network,
    const app_wifi_cred_t *prev)
{
    if (!dst) return APP_WIFI_MERGE_NEED_PASS;
    memset(dst, 0, sizeof(*dst));
    if (!ssid_in || ssid_in[0] == '\0') return APP_WIFI_MERGE_OK;

    strncpy(dst->ssid, ssid_in, sizeof(dst->ssid) - 1);
    if (open_network) return APP_WIFI_MERGE_OK;

    if (pass_in && pass_in[0] != '\0') {
        strncpy(dst->pass, pass_in, sizeof(dst->pass) - 1);
        return APP_WIFI_MERGE_OK;
    }
    if (prev && prev->pass[0] != '\0') {
        strncpy(dst->pass, prev->pass, sizeof(dst->pass) - 1);
        return APP_WIFI_MERGE_OK;
    }
    return APP_WIFI_MERGE_NEED_PASS;
}

typedef enum {
    APP_WIFI_FAIL_KEEP = 0,     /* same slot, budget remains */
    APP_WIFI_FAIL_ADVANCE,      /* this slot is exhausted; try next_slot */
    APP_WIFI_FAIL_EXHAUSTED,    /* every filled slot exhausted; next_slot wraps to the first */
} app_wifi_fail_action_t;

/**
 * One join failure on `current_slot`.
 * `fails_on_slot` counts this failure (1 on the first failure).
 * `per_slot_max` is the budget before moving on.
 * `*next_slot` is the slot the caller should install next (may equal current).
 */
static inline app_wifi_fail_action_t app_wifi_on_fail(
    const app_wifi_cred_t slots[APP_WIFI_SLOTS],
    int current_slot,
    int fails_on_slot,
    int per_slot_max,
    int *next_slot)
{
    if (next_slot) *next_slot = current_slot;
    if (per_slot_max < 1) per_slot_max = 1;
    if (fails_on_slot < per_slot_max) return APP_WIFI_FAIL_KEEP;

    int nxt = app_wifi_next_slot(slots, current_slot);
    if (nxt >= 0) {
        if (next_slot) *next_slot = nxt;
        return APP_WIFI_FAIL_ADVANCE;
    }
    int first = app_wifi_first_slot(slots);
    if (next_slot && first >= 0) *next_slot = first;
    return APP_WIFI_FAIL_EXHAUSTED;
}

/**
 * Stay on the current association without reinstalling when Wi-Fi 1 matches
 * the driver and every later slot is unchanged. Any backup-slot edit must
 * re-apply so the saved set is what the radio will search.
 */
static inline bool app_wifi_driver_keeps_link(
    const app_wifi_cred_t submitted[APP_WIFI_SLOTS],
    const app_wifi_cred_t previous[APP_WIFI_SLOTS],
    const char *driver_ssid,
    const char *driver_pass)
{
    if (!submitted || !previous || !app_wifi_slot_filled(&submitted[0])) return false;
    if (!driver_ssid) driver_ssid = "";
    if (!driver_pass) driver_pass = "";
    if (strncmp(submitted[0].ssid, driver_ssid, APP_WIFI_SSID_MAX) != 0) return false;
    if (strncmp(submitted[0].pass, driver_pass, APP_WIFI_PASS_MAX) != 0) return false;
    for (int i = 1; i < APP_WIFI_SLOTS; i++) {
        if (!app_wifi_creds_equal(&submitted[i], &previous[i])) return false;
    }
    return true;
}
