#pragma once
/**
 * Pure helpers for the config-page DeepSeek debug ring.
 *
 * Two independent slots. Display order is newest first. A third push
 * overwrites the oldest physical slot; the kept slot is not rewritten.
 * Empty history formats as 暂无. Two filled slots are separated by one
 * blank line (\\n\\n). No sample text is invented.
 *
 * Truncation marker is "…[截断]" (11 bytes). Head capture keeps the start
 * and puts the marker at the end. Tail capture keeps the end and puts the
 * marker at the start, so a trailing ##题目## survives a size cap.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_DEBUG_SLOTS 2
/** NVS string capacity for one system prompt, including the terminating NUL. */
#define APP_DEBUG_PROMPT_MAX 2048
/**
 * NVS blob capacity for one raw assistant reply, including the terminating NUL.
 * Matches the chat parse buffer. Blobs are required past the ~4000-byte
 * single-page NVS string limit.
 */
#define APP_DEBUG_RAW_MAX 8192
#define APP_DEBUG_TRUNC_MARK "…[截断]"

typedef struct {
    uint8_t count;  /* 0..APP_DEBUG_SLOTS */
    uint8_t newest; /* physical slot 0 or 1 when count > 0 */
} app_debug_ring_t;

/** Pack into one NVS byte: bits 2-3 = count, bits 0-1 = newest slot. */
uint8_t app_debug_ring_pack(const app_debug_ring_t *ring);
void app_debug_ring_unpack(uint8_t packed, app_debug_ring_t *ring);

/** Slot index to overwrite. Advances count and newest. Returns 0 or 1. */
int app_debug_ring_push(app_debug_ring_t *ring);

/** Physical slot for display position (0 = newest). -1 if that position is empty. */
int app_debug_ring_at(const app_debug_ring_t *ring, int position);

/**
 * Copy src into dst (always NUL-terminated when dst_sz > 0).
 * keep_tail: drop the head and prefix the marker when the text does not fit.
 * Otherwise drop the tail and suffix the marker.
 * force_mark: always attach the marker, even when src already fits.
 * Returns bytes written, excluding the NUL.
 */
size_t app_debug_capture(char *dst, size_t dst_sz, const char *src,
                         bool keep_tail, bool force_mark);

typedef void (*app_debug_emit_fn)(void *ctx, const char *bytes, size_t len);

/** Emit 暂无, one entry, or newest + blank line + older. */
void app_debug_emit_pair(const char *newest, const char *older,
                         app_debug_emit_fn emit, void *ctx);
