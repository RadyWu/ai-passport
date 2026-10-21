#include "app_debug_log.h"
#include <string.h>

static int utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static size_t utf8_prefix_bytes(const char *s, size_t n, size_t budget)
{
    size_t i = 0;
    while (i < n && i < budget) {
        int need = utf8_len((unsigned char)s[i]);
        if (need < 1) need = 1;
        if (i + (size_t)need > n || i + (size_t)need > budget) break;
        i += (size_t)need;
    }
    return i;
}

static size_t utf8_tail_start(const char *s, size_t n, size_t budget)
{
    size_t i = 0;
    if (n <= budget) return 0;
    while (i < n) {
        int need = utf8_len((unsigned char)s[i]);
        if (need < 1) need = 1;
        if (i + (size_t)need > n) break;
        if (n - i <= budget) return i;
        i += (size_t)need;
    }
    return n;
}

uint8_t app_debug_ring_pack(const app_debug_ring_t *ring)
{
    uint8_t count;
    uint8_t newest;
    if (!ring) return 0;
    count = ring->count;
    newest = ring->newest;
    if (count > APP_DEBUG_SLOTS) count = 0;
    if (newest > 1) newest = 0;
    if (count == 0) newest = 0;
    return (uint8_t)((count << 2) | newest);
}

void app_debug_ring_unpack(uint8_t packed, app_debug_ring_t *ring)
{
    if (!ring) return;
    ring->newest = (uint8_t)(packed & 0x3);
    ring->count = (uint8_t)((packed >> 2) & 0x3);
    if (ring->count > APP_DEBUG_SLOTS || ring->newest > 1) {
        ring->count = 0;
        ring->newest = 0;
    }
    if (ring->count == 0) ring->newest = 0;
}

int app_debug_ring_push(app_debug_ring_t *ring)
{
    int slot;
    if (!ring) return 0;
    if (ring->count > APP_DEBUG_SLOTS || ring->newest > 1) {
        ring->count = 0;
        ring->newest = 0;
    }
    if (ring->count == 0) slot = 0;
    else slot = (int)(ring->newest ^ 1u);
    ring->newest = (uint8_t)slot;
    if (ring->count < APP_DEBUG_SLOTS) ring->count++;
    return slot;
}

int app_debug_ring_at(const app_debug_ring_t *ring, int position)
{
    if (!ring || position < 0 || ring->count == 0 || ring->count > APP_DEBUG_SLOTS) return -1;
    if (position >= (int)ring->count) return -1;
    if (ring->newest > 1) return -1;
    if (position == 0) return (int)ring->newest;
    return (int)(ring->newest ^ 1u);
}

size_t app_debug_capture(char *dst, size_t dst_sz, const char *src,
                         bool keep_tail, bool force_mark)
{
    size_t n;
    size_t mlen;
    bool mark;
    if (!dst || dst_sz == 0) return 0;
    dst[0] = '\0';
    if (!src) src = "";
    n = strlen(src);
    mlen = sizeof(APP_DEBUG_TRUNC_MARK) - 1;
    mark = force_mark || (n + 1 > dst_sz);
    if (!mark) {
        memcpy(dst, src, n + 1);
        return n;
    }
    if (dst_sz <= mlen + 1) {
        size_t fit = utf8_prefix_bytes(src, n, dst_sz - 1);
        memcpy(dst, src, fit);
        dst[fit] = '\0';
        return fit;
    }
    {
        size_t budget = dst_sz - 1 - mlen;
        if (keep_tail) {
            size_t start = utf8_tail_start(src, n, budget);
            size_t tail = (start < n) ? (n - start) : 0;
            if (tail > budget) tail = utf8_prefix_bytes(src + start, tail, budget);
            memcpy(dst, APP_DEBUG_TRUNC_MARK, mlen);
            memcpy(dst + mlen, src + start, tail);
            dst[mlen + tail] = '\0';
            return mlen + tail;
        }
        {
            size_t head = utf8_prefix_bytes(src, n, budget);
            memcpy(dst, src, head);
            memcpy(dst + head, APP_DEBUG_TRUNC_MARK, mlen + 1);
            return head + mlen;
        }
    }
}

void app_debug_emit_pair(const char *newest, const char *older,
                         app_debug_emit_fn emit, void *ctx)
{
    bool has_new;
    bool has_old;
    if (!emit) return;
    has_new = newest && newest[0] != '\0';
    has_old = older && older[0] != '\0';
    if (!has_new && !has_old) {
        static const char empty[] = "暂无";
        emit(ctx, empty, sizeof(empty) - 1);
        return;
    }
    if (has_new) emit(ctx, newest, strlen(newest));
    if (has_new && has_old) emit(ctx, "\n\n", 2);
    if (has_old) emit(ctx, older, strlen(older));
}
