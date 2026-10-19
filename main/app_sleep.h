#pragma once
#include <stdbool.h>
/**
 * Enter deep sleep; wakes on any function key (GPIO0 low).
 * Returns false if sleep was aborted (key still held / pad unsafe) — caller
 * must re-init buttons and tip the user. Does not return on success.
 */
bool app_sleep_enter_deep(void);
