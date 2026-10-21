#pragma once
/**
 * Ordered Wi-Fi credential slots (exactly 3).
 *
 * Host-testable: no ESP-IDF or NVS. Empty SSIDs are skipped.
 *
 * Connect order is scan-first (`app_wifi_visible_try_order`):
 *   1. Keep filled slots whose SSID was in the latest scan.
 *   2. If `preferred` (last slot that got an IP, or APP_WIFI_PREF_NONE)
 *      is one of those, try it first.
 *   3. Then the other visible slots in index order (Wi-Fi 1 → 2 → 3).
 * Filled slots absent from the scan are not tried. A zero-length order means
 * do not connect. With no preferred slot, visible slots stay in order 1 → 2 → 3.
 *
 * `app_wifi_on_fail` is the per-slot join budget while walking a candidate
 * list. `app_wifi_order_after` steps a visible order and does not wrap.
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

/** No stored last-success slot. */
#define APP_WIFI_PREF_NONE (-1)

/** ASCII A–Z folded to a–z. Other bytes, including non-ASCII, stay as they are. */
static inline char app_wifi_ascii_fold(char c)
{
    unsigned char u = (unsigned char)c;
    if (u >= 'A' && u <= 'Z') return (char)(u - 'A' + 'a');
    return (char)u;
}

/**
 * SSID equality for scan matching.
 * ASCII letters are case-insensitive. Comparison stops at the first NUL and
 * does not treat a shorter name as a prefix of a longer one.
 */
static inline bool app_wifi_ssid_equal_ci(const char *a, const char *b)
{
    if (!a || !b) return false;
    for (int i = 0; i < APP_WIFI_SSID_MAX; i++) {
        unsigned char ca = (unsigned char)app_wifi_ascii_fold(a[i]);
        unsigned char cb = (unsigned char)app_wifi_ascii_fold(b[i]);
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
    return true;
}

/**
 * Scan entry that matches `saved`, or NULL.
 * The returned pointer is the scan buffer (exact bytes and letter case).
 * The first match wins. Empty names never match.
 */
static inline const char *app_wifi_scan_ssid(const char *saved,
                                             const char *const *visible,
                                             int visible_count)
{
    if (!saved || saved[0] == '\0' || !visible || visible_count <= 0) return NULL;
    for (int i = 0; i < visible_count; i++) {
        const char *got = visible[i];
        if (!got || got[0] == '\0') continue;
        if (app_wifi_ssid_equal_ci(saved, got)) return got;
    }
    return NULL;
}

/**
 * True when `ssid` matches one scan entry, ignoring ASCII letter case.
 * Limited to the 32-character SSID field. Empty names never match.
 */
static inline bool app_wifi_ssid_visible(const char *ssid,
                                         const char *const *visible,
                                         int visible_count)
{
    return app_wifi_scan_ssid(ssid, visible, visible_count) != NULL;
}

/**
 * Visible-slot connect order.
 * Writes slot indexes into `order` and returns how many to try (0 = none).
 * `preferred` is tried first when that slot is filled and its SSID is visible;
 * remaining visible slots follow in ascending index. Invisible slots are omitted.
 */
static inline int app_wifi_visible_try_order(
    const app_wifi_cred_t slots[APP_WIFI_SLOTS],
    const char *const *visible,
    int visible_count,
    int preferred,
    int order[APP_WIFI_SLOTS])
{
    if (order) {
        for (int i = 0; i < APP_WIFI_SLOTS; i++) order[i] = -1;
    }
    if (!slots || !order) return 0;

    int n = 0;
    if (preferred >= 0 && preferred < APP_WIFI_SLOTS
        && app_wifi_slot_filled(&slots[preferred])
        && app_wifi_ssid_visible(slots[preferred].ssid, visible, visible_count)) {
        order[n++] = preferred;
    }
    for (int i = 0; i < APP_WIFI_SLOTS; i++) {
        if (n > 0 && i == order[0]) continue;
        if (!app_wifi_slot_filled(&slots[i])) continue;
        if (!app_wifi_ssid_visible(slots[i].ssid, visible, visible_count)) continue;
        order[n++] = i;
    }
    return n;
}

/**
 * Next slot in a visible order after `current`, or -1 when `current` is last
 * or not in the list. Does not wrap; the caller rescans or takes all-failed.
 */
static inline int app_wifi_order_after(const int order[], int count, int current)
{
    if (!order || count <= 0 || current < 0) return -1;
    for (int i = 0; i < count && i < APP_WIFI_SLOTS; i++) {
        if (order[i] == current) {
            return (i + 1 < count) ? order[i + 1] : -1;
        }
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
