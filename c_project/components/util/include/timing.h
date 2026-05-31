#ifndef TIMING_H
#define TIMING_H

#include <stdint.h>

/**
 * Get monotonic time in milliseconds (wraps at ~49 days).
 */
uint32_t timing_millis(void);

/**
 * Get monotonic time in microseconds.
 */
uint64_t timing_micros(void);

/**
 * Non-blocking delay check. Returns true if elapsed_ms have passed since start_ms.
 */
bool timing_elapsed(uint32_t start_ms, uint32_t elapsed_ms);

/**
 * Get system uptime in seconds.
 */
uint32_t timing_uptime_seconds(void);

#endif /* TIMING_H */
