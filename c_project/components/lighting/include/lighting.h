#ifndef LIGHTING_H
#define LIGHTING_H

#include "esp_err.h"
#include "leds.h"
#include "cJSON.h"

#include <stdbool.h>
#include <stdint.h>

/* Maximum concurrent active scenes */
#define MAX_ACTIVE_SCENES 8
#define MAX_JOBS_PER_SCENE 16

#ifndef MAX_LEDS
#define MAX_LEDS 300
#endif

/**
 * LED output entry from a pattern function.
 */
typedef struct {
    int led_index;
    rgb_t color;
} led_output_t;

/**
 * Filter runtime state (per-effect instance).
 */
typedef struct {
    uint32_t last_spike_tick;
    int spike_subrange_index;
    int spike_led_index;
    bool spike_active;
    void *cached_params; /* For filters that need modified params (e.g. dropout) */
} filter_state_t;

/**
 * Active job state within a scene.
 */
typedef struct {
    char name[64];
    char pattern_name[32];
    rgb_t colors[8];
    int color_count;
    int target_indices[MAX_LEDS];
    int target_count;
    int duration;
    int cycles;
    int cycles_completed;
    int period;
    int width;
    int number;
    bool reverse;
    uint32_t start_tick;
    bool finished;
    char after[64];          /* Job name this depends on */
    /* Filter chain */
    struct {
        char filter_name[32];
        cJSON *params;
        filter_state_t state;
    } filters[8];
    int filter_count;
} active_job_t;

/**
 * Active scene state.
 */
typedef struct {
    char name[64];
    active_job_t jobs[MAX_JOBS_PER_SCENE];
    int job_count;
    bool active;
} active_scene_t;

/**
 * Initialize the lighting system.
 */
esp_err_t lighting_init(void);

/**
 * Process a single animation tick.
 *
 * Called by the animation task each frame.
 */
void lighting_process_tick(uint32_t tick);

/**
 * Set (activate) a scene by name. Clears existing scenes first.
 */
esp_err_t lighting_set_scene(const char *scene_name);

/**
 * Add a scene to the active set (without clearing others).
 */
esp_err_t lighting_add_scene(const char *scene_name);

/**
 * Remove a scene from the active set.
 */
esp_err_t lighting_remove_scene(const char *scene_name);

/**
 * Get list of currently active scene names.
 */
int lighting_get_active_scenes(char names[][64], int max_count);

/**
 * Clear all active scenes.
 */
void lighting_clear_scenes(void);

/**
 * Get the logical color buffer (current LED state).
 */
rgb_t *lighting_get_logical_colors(void);

/**
 * Resolve a target specification to LED indices.
 *
 * Supports: "0-5", "10-20", "named:RangeName", comma-separated combos.
 * Returns number of indices resolved.
 */
int lighting_get_targets(const char *spec, int *indices, int max_indices);

/**
 * Resolve a color specification to an RGB value.
 *
 * Supports: named colors, "#RRGGBB" hex, [r, g, b] arrays.
 */
rgb_t lighting_get_color(const char *spec);

/**
 * Resolve a color from a cJSON value.
 */
rgb_t lighting_get_color_json(const cJSON *color_spec);

/**
 * Get model names list.
 */
cJSON *lighting_get_model_names(void);

/**
 * Get current model name.
 */
const char *lighting_get_current_model(void);

/**
 * Set the current model.
 */
esp_err_t lighting_set_current_model(const char *model_name);

/**
 * Get the current model's settings object.
 */
cJSON *lighting_get_settings(void);

#endif /* LIGHTING_H */
