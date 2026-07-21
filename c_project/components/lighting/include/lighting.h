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

/* Max number of independently-timed LED groups a spike/dropout filter can
 * track (scope "all" = 1 group, "subranges" = contiguous LED runs, anything
 * else = one group per LED). Groups beyond this cap share the last slot. */
#define MAX_SPIKE_GROUPS 8

/**
 * Per-group periodic spike timing state (mirrors the Python "_state" dict
 * used by lighting.filters._apply_spike_filter).
 */
typedef struct {
    int next_spike;
    int spike_end;
    bool initialized;
} spike_group_state_t;

/**
 * Filter runtime state (per-effect instance).
 */
typedef struct {
    spike_group_state_t spike_groups[MAX_SPIKE_GROUPS];
    void *cached_params; /* Reserved for filters that need modified params */
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
    bool after_pending;      /* True until the "after" dependency has been satisfied once */
    bool inherit_target;     /* With "after": adopt the finished predecessor's resolved
                                target instead of this job's own (mirrors Python's
                                inherit_target/passthrough in effects.py) */
    /* Retained per-cycle random state used by patterns that need a value to
     * persist across ticks within the same cycle (mirrors Python's
     * Lighting.retained_values dict). Currently only pattern_phaser_strip's
     * random meeting point uses this. */
    int retained_meet_index;
    bool retained_meet_set;
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

/**
 * Create a new model. If copy_from_current is true, the current model's
 * scenes/effects/filters/named_ranges/custom_colors/scene_settings are
 * deep-copied into the new model; otherwise it starts empty.
 */
esp_err_t lighting_create_model(const char *model_name, bool copy_from_current);

/**
 * Delete a model. Fails if it is the currently active model.
 */
esp_err_t lighting_delete_model(const char *model_name);

/**
 * Rename a model, updating current_model if it was the active one.
 */
esp_err_t lighting_rename_model(const char *old_name, const char *new_name);

#endif /* LIGHTING_H */
