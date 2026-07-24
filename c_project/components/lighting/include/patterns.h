#ifndef PATTERNS_H
#define PATTERNS_H

#include "lighting.h"

/**
 * Pattern function type.
 *
 * Generates LED output for a single tick of an effect. `current_colors` is
 * an array parallel to job->target_indices (job->target_count entries)
 * giving each target's current logical color - needed by patterns (wave,
 * cylon, phaser_strip) that fade the previous frame's color toward the
 * background rather than recomputing a static shape each tick, mirroring
 * Lighting.get_logical_color() usage in lib/lighting/patterns.py.
 *
 * `job` is non-const because some patterns (phaser_strip) retain small bits
 * of per-cycle state (e.g. a random meeting point) directly on the job,
 * mirroring Python's Lighting.retained_values dict.
 *
 * Returns number of entries written to output array.
 */
typedef int (*pattern_fn_t)(active_job_t *job, uint32_t local_tick,
                            const rgb_t *current_colors,
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
int pattern_solid(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_blink(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_pulse(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_fade_in(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_breathe(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_wave(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_cylon(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_phaser_strip(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_rainbow(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_color_wipe(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_fire(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_gradient(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_warp_pulse(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_theater_chase(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);
int pattern_heartbeat(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors, led_output_t *output, int max_output);

#endif /* PATTERNS_H */
