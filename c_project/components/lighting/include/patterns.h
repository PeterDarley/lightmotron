#ifndef PATTERNS_H
#define PATTERNS_H

#include "lighting.h"

/**
 * Pattern function type.
 *
 * Generates LED output for a single tick of an effect.
 * Returns number of entries written to output array.
 */
typedef int (*pattern_fn_t)(const active_job_t *job, uint32_t local_tick,
                            led_output_t *output, int max_output);

/**
 * Get a pattern function by name.
 */
pattern_fn_t pattern_get(const char *name);

/**
 * Get list of available pattern names.
 */
const char **pattern_get_names(int *count);

/* Individual pattern functions */
int pattern_solid(const active_job_t *job, uint32_t local_tick, led_output_t *output, int max_output);
int pattern_blink(const active_job_t *job, uint32_t local_tick, led_output_t *output, int max_output);
int pattern_pulse(const active_job_t *job, uint32_t local_tick, led_output_t *output, int max_output);
int pattern_fade_in(const active_job_t *job, uint32_t local_tick, led_output_t *output, int max_output);
int pattern_breathe(const active_job_t *job, uint32_t local_tick, led_output_t *output, int max_output);
int pattern_wave(const active_job_t *job, uint32_t local_tick, led_output_t *output, int max_output);
int pattern_cylon(const active_job_t *job, uint32_t local_tick, led_output_t *output, int max_output);
int pattern_phaser_strip(const active_job_t *job, uint32_t local_tick, led_output_t *output, int max_output);

#endif /* PATTERNS_H */
