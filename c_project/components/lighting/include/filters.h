#ifndef FILTERS_H
#define FILTERS_H

#include "lighting.h"
#include "cJSON.h"

/**
 * Filter function type.
 *
 * Applies a filter to pattern output. Receives both target colors (from pattern)
 * and current colors (from logical_colors). Differences are calculated against
 * target but applied to current, making filters order-independent.
 */
typedef void (*filter_fn_t)(const cJSON *params, led_output_t *target_colors,
                            const rgb_t *current_colors, int count,
                            filter_state_t *state, uint32_t tick);

/**
 * Get a filter function by name.
 */
filter_fn_t filter_get(const char *name);

/**
 * Get list of available filter names.
 */
const char **filter_get_names(int *count);

/* Individual filter functions */
void filter_null(const cJSON *params, led_output_t *target, const rgb_t *current, int count, filter_state_t *state, uint32_t tick);
void filter_brightness(const cJSON *params, led_output_t *target, const rgb_t *current, int count, filter_state_t *state, uint32_t tick);
void filter_sizzle(const cJSON *params, led_output_t *target, const rgb_t *current, int count, filter_state_t *state, uint32_t tick);
void filter_scintillate(const cJSON *params, led_output_t *target, const rgb_t *current, int count, filter_state_t *state, uint32_t tick);
void filter_spike(const cJSON *params, led_output_t *target, const rgb_t *current, int count, filter_state_t *state, uint32_t tick);
void filter_dropout(const cJSON *params, led_output_t *target, const rgb_t *current, int count, filter_state_t *state, uint32_t tick);

#endif /* FILTERS_H */
