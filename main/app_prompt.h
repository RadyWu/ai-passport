#pragma once
/**
 * Build the role:system text the model sees.
 * Not the JSON envelope. Preference, avoid-topics, and salt are already
 * included. Control bytes (0x00-0x1F) are dropped, matching the JSON
 * string encoder, so this text matches the decoded request content.
 */
#include <stddef.h>
#include <stdint.h>

#define APP_PROMPT_TOPIC_SLOTS 5
#define APP_PROMPT_TOPIC_MAX 64
/** Same budget the debug ring stores. A normal prompt is well under this. */
#define APP_PROMPT_SYS_MAX 2048

/**
 * topics may be NULL. Empty preference uses the built-in default.
 * Returns the byte length excluding NUL, or -1 when out is missing.
 * If out_sz cannot hold the full text, the head is kept and the result
 * ends with "…[截断]".
 */
int app_prompt_build_system(char *out, size_t out_sz,
                            const char *preference,
                            const char (*topics)[APP_PROMPT_TOPIC_MAX],
                            uint32_t salt);
