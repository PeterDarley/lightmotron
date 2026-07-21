#include "lighting.h"
#include "patterns.h"
#include "filters.h"
#include "effects.h"
#include "colors.h"
#include "named_ranges.h"
#include "metadata.h"
#include "animation.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "sound_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "lighting";

/* Heap-allocated (lands in external PSRAM via CONFIG_SPIRAM_USE_MALLOC,
 * since MAX_ACTIVE_SCENES * MAX_JOBS_PER_SCENE * MAX_LEDS target_indices
 * arrays are far too large for internal DRAM). */
static active_scene_t *active_scenes = NULL;
static int active_scene_count = 0;
static rgb_t logical_colors[MAX_LEDS];
static SemaphoreHandle_t lighting_mutex = NULL;
static bool initialized = false;

/* Pre-allocated buffers for tick processing (avoids per-frame stack/heap alloc) */
static led_output_t tick_output_buf[MAX_LEDS];
static rgb_t tick_filter_buf[MAX_LEDS];
static rgb_t tick_pattern_current_buf[MAX_LEDS];

esp_err_t lighting_init(void)
{
    if (initialized) return ESP_OK;

    active_scenes = calloc(MAX_ACTIVE_SCENES, sizeof(active_scene_t));
    if (!active_scenes) {
        ESP_LOGE(TAG, "Failed to allocate active_scenes (%u bytes)",
                 (unsigned)(MAX_ACTIVE_SCENES * sizeof(active_scene_t)));
        return ESP_ERR_NO_MEM;
    }

    lighting_mutex = xSemaphoreCreateMutex();
    memset(logical_colors, 0, sizeof(logical_colors));
    active_scene_count = 0;
    initialized = true;

    ESP_LOGI(TAG, "Lighting system initialized");
    return ESP_OK;
}

/**
 * Open the lighting-settings persistent store. Thin wrapper around the
 * shared STORAGE_LIGHTING_SETTINGS_FILE constant so every call site here
 * doesn't repeat the raw persistent_dict_open() call.
 */
static persistent_dict_t *open_lighting_store(void)
{
    return persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
}

/**
 * Open the lighting settings store and return the currently-active model
 * object. Returns NULL if the store, models map, or current model is
 * missing.
 */
static cJSON *get_current_model_object(void)
{
    persistent_dict_t *lighting_store = open_lighting_store();
    if (!lighting_store) return NULL;

    cJSON *models = persistent_dict_get(lighting_store, "models");
    cJSON *current_name = persistent_dict_get(lighting_store, "current_model");
    if (!models || !current_name || !current_name->valuestring) return NULL;

    return cJSON_GetObjectItem(models, current_name->valuestring);
}

/**
 * Return true if every effect in the scene has an explicit cycle limit and
 * all of them have finished (mirrors Lighting._is_scene_finished()). A scene
 * with no cycle-limited effects at all (or none yet resolved) is never
 * considered finished.
 */
static bool scene_is_finished(const active_scene_t *scene)
{
    bool has_cycle_limited = false;

    for (int j = 0; j < scene->job_count; j++) {
        const active_job_t *job = &scene->jobs[j];
        if (job->target_count == 0) continue;

        if (job->cycles <= 0) {
            /* Infinite effect: scene is ongoing by design. */
            return false;
        }

        has_cycle_limited = true;
        if (!job->finished) return false;
    }

    return has_cycle_limited;
}

/**
 * Free a job's per-filter cJSON params/cached_params and null the pointers
 * out. Shared by every place that tears down a job's filter chain (a scene
 * finishing naturally, being explicitly removed, or all scenes being
 * cleared).
 */
static void free_job_filters(active_job_t *job)
{
    for (int f = 0; f < job->filter_count; f++) {
        if (job->filters[f].params) {
            cJSON_Delete(job->filters[f].params);
            job->filters[f].params = NULL;
        }
        if (job->filters[f].state.cached_params) {
            cJSON_Delete(job->filters[f].state.cached_params);
            job->filters[f].state.cached_params = NULL;
        }
    }
}

/**
 * Resolve scene_name to an actual scene key, falling back to the model's
 * "default_scene" and then to the first defined scene (mirrors
 * Lighting.set_scene()'s None-name resolution). Returns NULL if no scene
 * could be resolved.
 */
static const char *resolve_scene_name(const cJSON *model, const char *scene_name, cJSON *scenes)
{
    if (scene_name) return scene_name;

    const char *default_scene = json_get_string(model, "default_scene", NULL);
    if (default_scene && cJSON_GetObjectItem(scenes, default_scene)) {
        return default_scene;
    }

    cJSON *first = scenes->child;
    return first ? first->string : NULL;
}

/**
 * Load a scene from storage and activate it. Pass NULL for scene_name to
 * resolve the model's default (or first) scene.
 */
static esp_err_t activate_scene(const char *scene_name)
{
    if (active_scene_count >= MAX_ACTIVE_SCENES) {
        ESP_LOGW(TAG, "Max active scenes reached");
        return ESP_ERR_NO_MEM;
    }

    cJSON *model = get_current_model_object();
    if (!model) return ESP_FAIL;

    cJSON *scenes = cJSON_GetObjectItem(model, "scenes");
    if (!scenes) return ESP_FAIL;

    scene_name = resolve_scene_name(model, scene_name, scenes);
    if (!scene_name) {
        ESP_LOGW(TAG, "No scene available to activate");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *scene_def = cJSON_GetObjectItem(scenes, scene_name);
    if (!scene_def || !cJSON_IsObject(scene_def)) {
        ESP_LOGW(TAG, "Scene not found: %s", scene_name);
        return ESP_ERR_NOT_FOUND;
    }

    /* Handle scene metadata (kills, sounds) */
    scene_metadata_t meta;
    metadata_get_scene(scene_name, &meta);

    /* Kill other scenes */
    for (int k = 0; k < meta.kills_count; k++) {
        lighting_remove_scene(meta.kills[k]);
    }

    /* Stop sounds on start */
    for (int s = 0; s < meta.stop_sounds_on_start_count; s++) {
        sound_manager_stop(meta.stop_sounds_on_start[s]);
    }

    /* Play scene sound */
    if (meta.sound[0] != '\0') {
        sound_manager_play(meta.sound, 0);
    }

    /* Create active scene */
    active_scene_t *scene = &active_scenes[active_scene_count];
    memset(scene, 0, sizeof(active_scene_t));
    strncpy(scene->name, scene_name, sizeof(scene->name) - 1);
    scene->active = true;

    /* Resolve all jobs in the scene */
    cJSON *effects_dict = cJSON_GetObjectItem(model, "effects");
    cJSON *job_entry = scene_def->child;
    int job_idx = 0;
    uint32_t current_tick = animation_get_tick();

    while (job_entry && job_idx < MAX_JOBS_PER_SCENE) {
        active_job_t *job = &scene->jobs[job_idx];
        strncpy(job->name, job_entry->string, sizeof(job->name) - 1);

        /* Check if this references a named effect or is inline */
        const char *effect_ref = json_get_string(job_entry, "effect", NULL);
        if (effect_ref && effects_dict) {
            cJSON *effect_def = cJSON_GetObjectItem(effects_dict, effect_ref);
            if (effect_def) {
                effect_resolve(effect_def, job);
            }
            /* Override with inline params */
            const char *target = json_get_string(job_entry, "target", NULL);
            if (target) {
                job->target_count = target_spec_resolve(target, job->target_indices, MAX_LEDS);
            }
            int cycles = json_get_int(job_entry, "cycles", -1);
            if (cycles >= 0) job->cycles = cycles;

            const char *after = json_get_string(job_entry, "after", NULL);
            if (after) strncpy(job->after, after, sizeof(job->after) - 1);
            job->inherit_target = json_get_bool(job_entry, "inherit_target", false);
        } else {
            effect_resolve_inline(job_entry, job);
        }

        /* Non-"after" jobs start counting from this scene's activation tick,
         * matching Lighting.set_scene()/add_scene() tracking scene start
         * ticks in Python. "after"-dependent jobs get their start_tick
         * reassigned lazily once their dependency finishes (see
         * lighting_process_tick()). */
        job->start_tick = current_tick;
        job->after_pending = (job->after[0] != '\0');
        job->cycles_completed = 0;
        job->finished = false;
        job_idx++;
        job_entry = job_entry->next;
    }

    scene->job_count = job_idx;
    active_scene_count++;

    ESP_LOGI(TAG, "Scene activated: %s (%d jobs)", scene_name, job_idx);
    return ESP_OK;
}

void lighting_process_tick(uint32_t tick)
{
    if (!initialized) return;

    /* Scene names to activate once the mutex is released — completed scenes'
     * trigger_scenes_on_completion lists (mirrors effects.py's process_tick,
     * which calls add_scene() for each trigger before remove_scene()).
     * Deferred because activate_scene() -> lighting_remove_scene() re-takes
     * the non-recursive lighting mutex. Flat capped list keeps animation-task
     * stack usage modest. */
    char pending_triggers[8][64];
    int pending_trigger_count = 0;

    xSemaphoreTake(lighting_mutex, portMAX_DELAY);

    for (int s = 0; s < active_scene_count; s++) {
        active_scene_t *scene = &active_scenes[s];
        if (!scene->active) continue;

        bool all_finished = true;
        bool has_cycle_limited = false;

        for (int j = 0; j < scene->job_count; j++) {
            active_job_t *job = &scene->jobs[j];
            if (job->finished) continue;
            if (job->target_count == 0) continue;

            /* Check 'after' dependency */
            if (job->after[0] != '\0') {
                active_job_t *dep_job = NULL;
                for (int d = 0; d < scene->job_count; d++) {
                    if (strcmp(scene->jobs[d].name, job->after) == 0) {
                        dep_job = &scene->jobs[d];
                        break;
                    }
                }
                if (!dep_job || !dep_job->finished) {
                    all_finished = false;
                    continue;
                }
                /* Start this job now, the first time its dependency finishes */
                if (job->after_pending) {
                    job->start_tick = tick;
                    job->after_pending = false;

                    /* inherit_target: adopt the finished predecessor's
                     * resolved target (mirrors Python's passthrough in
                     * effects.py — the predecessor's target may itself have
                     * been inherited, so chains A->B->C work). */
                    if (job->inherit_target && dep_job->target_count > 0) {
                        memcpy(job->target_indices, dep_job->target_indices,
                               sizeof(job->target_indices));
                        job->target_count = dep_job->target_count;
                    }
                }
            }

            /* Calculate local tick */
            uint32_t local_tick = tick - job->start_tick;

            /* Check cycle completion */
            if (job->cycles > 0) {
                has_cycle_limited = true;
                int duration = job->duration > 0 ? job->duration : 40;
                int ticks_per_cycle = duration;
                if (job->period > 0) ticks_per_cycle = job->period;

                int current_cycle = (int)(local_tick / ticks_per_cycle);
                if (current_cycle >= job->cycles) {
                    job->finished = true;
                    continue;
                }
                all_finished = false;
            } else {
                /* Infinite effect */
                all_finished = false;
            }

            /* Build the current (previous-frame) logical color for each of
             * this job's targets, in target order. Needed by patterns that
             * fade from the prior frame (wave/cylon/phaser_strip), mirroring
             * Lighting.get_logical_color() usage in patterns.py. */
            for (int i = 0; i < job->target_count && i < MAX_LEDS; i++) {
                int idx = job->target_indices[i];
                tick_pattern_current_buf[i] = (idx >= 0 && idx < MAX_LEDS) ? logical_colors[idx] : (rgb_t){0, 0, 0};
            }

            /* Execute pattern */
            pattern_fn_t pattern_fn = pattern_get(job->pattern_name);
            int output_count = pattern_fn(job, local_tick, tick_pattern_current_buf, tick_output_buf, MAX_LEDS);

            /* Apply filters. Filters key their timing off the effect's own
             * local_tick (ticks since this job/effect started), matching
             * Python's effects.py process_tick() which calls filter_func(...,
             * tick_number=local_tick) - not the global animation tick. */
            for (int f = 0; f < job->filter_count; f++) {
                filter_fn_t filter_fn = filter_get(job->filters[f].filter_name);

                /* Build current colors array for the target LEDs */
                for (int o = 0; o < output_count; o++) {
                    int idx = tick_output_buf[o].led_index;
                    if (idx >= 0 && idx < MAX_LEDS) {
                        tick_filter_buf[o] = logical_colors[idx];
                    } else {
                        tick_filter_buf[o] = (rgb_t){0, 0, 0};
                    }
                }

                filter_fn(job->filters[f].params, tick_output_buf, tick_filter_buf,
                          output_count, &job->filters[f].state, local_tick);
            }

            /* Update logical colors */
            for (int o = 0; o < output_count; o++) {
                int idx = tick_output_buf[o].led_index;
                if (idx >= 0 && idx < MAX_LEDS) {
                    logical_colors[idx] = tick_output_buf[o].color;
                }
            }
        }

        /* Auto-remove scene if all cycle-limited effects are done */
        if (has_cycle_limited && all_finished) {
            ESP_LOGI(TAG, "Scene completed: %s", scene->name);

            /* Handle stop_sounds_on_end */
            scene_metadata_t meta;
            metadata_get_scene(scene->name, &meta);
            for (int ss = 0; ss < meta.stop_sounds_on_end_count; ss++) {
                sound_manager_stop(meta.stop_sounds_on_end[ss]);
            }

            /* Queue trigger_scenes_on_completion for activation after the
             * mutex is released. */
            for (int t = 0; t < meta.trigger_scenes_on_completion_count &&
                            pending_trigger_count < 8; t++) {
                strncpy(pending_triggers[pending_trigger_count],
                        meta.trigger_scenes_on_completion[t],
                        sizeof(pending_triggers[0]) - 1);
                pending_triggers[pending_trigger_count][sizeof(pending_triggers[0]) - 1] = '\0';
                pending_trigger_count++;
            }

            /* Free filter resources */
            for (int j = 0; j < scene->job_count; j++) {
                free_job_filters(&scene->jobs[j]);
            }

            scene->active = false;
        }
    }

    /* Write logical colors to LED buffer */
    int total = leds_total_count();
    for (int i = 0; i < total && i < MAX_LEDS; i++) {
        leds_set_pixel_rgb(i, logical_colors[i]);
    }

    /* Compact active scenes (remove inactive) */
    int write_idx = 0;
    for (int i = 0; i < active_scene_count; i++) {
        if (active_scenes[i].active) {
            if (write_idx != i) {
                active_scenes[write_idx] = active_scenes[i];
            }
            write_idx++;
        }
    }
    active_scene_count = write_idx;

    xSemaphoreGive(lighting_mutex);

    /* Fire completion triggers now that the mutex is free. activate_scene()
     * validates the name against the model's scenes itself, matching Python's
     * `if trigger_scene_name in self.settings.get("scenes", {})` guard. */
    for (int t = 0; t < pending_trigger_count; t++) {
        esp_err_t trigger_err = lighting_add_scene(pending_triggers[t]);
        if (trigger_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to trigger scene '%s' on completion (err=%d)",
                     pending_triggers[t], (int)trigger_err);
        }
    }
}

esp_err_t lighting_set_scene(const char *scene_name)
{
    lighting_clear_scenes();
    return activate_scene(scene_name);
}

esp_err_t lighting_add_scene(const char *scene_name)
{
    return activate_scene(scene_name);
}

esp_err_t lighting_remove_scene(const char *scene_name)
{
    if (!scene_name) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(lighting_mutex, portMAX_DELAY);

    for (int i = 0; i < active_scene_count; i++) {
        if (strcmp(active_scenes[i].name, scene_name) == 0) {
            /* Free filter params and cached state */
            for (int j = 0; j < active_scenes[i].job_count; j++) {
                free_job_filters(&active_scenes[i].jobs[j]);
            }
            active_scenes[i].active = false;
            break;
        }
    }

    xSemaphoreGive(lighting_mutex);
    return ESP_OK;
}

int lighting_get_active_scenes(char names[][64], int max_count)
{
    xSemaphoreTake(lighting_mutex, portMAX_DELAY);
    int count = 0;
    for (int i = 0; i < active_scene_count && count < max_count; i++) {
        if (active_scenes[i].active) {
            strncpy(names[count], active_scenes[i].name, 63);
            count++;
        }
    }
    xSemaphoreGive(lighting_mutex);
    return count;
}

void lighting_clear_scenes(void)
{
    xSemaphoreTake(lighting_mutex, portMAX_DELAY);
    for (int i = 0; i < active_scene_count; i++) {
        for (int j = 0; j < active_scenes[i].job_count; j++) {
            free_job_filters(&active_scenes[i].jobs[j]);
        }
    }
    active_scene_count = 0;
    memset(active_scenes, 0, sizeof(active_scenes));
    xSemaphoreGive(lighting_mutex);
}

rgb_t *lighting_get_logical_colors(void)
{
    return logical_colors;
}

int lighting_get_targets(const char *spec, int *indices, int max_indices)
{
    return target_spec_resolve(spec, indices, max_indices);
}

rgb_t lighting_get_color(const char *spec)
{
    return color_resolve(spec);
}

rgb_t lighting_get_color_json(const cJSON *color_spec)
{
    return color_resolve_json(color_spec);
}

cJSON *lighting_get_model_names(void)
{
    persistent_dict_t *lighting_store = open_lighting_store();
    if (!lighting_store) return cJSON_CreateArray();

    cJSON *models = persistent_dict_get(lighting_store, "models");
    if (!models || !cJSON_IsObject(models)) return cJSON_CreateArray();

    cJSON *names = cJSON_CreateArray();
    cJSON *child = models->child;
    while (child) {
        cJSON_AddItemToArray(names, cJSON_CreateString(child->string));
        child = child->next;
    }
    return names;
}

const char *lighting_get_current_model(void)
{
    persistent_dict_t *lighting_store = open_lighting_store();
    if (!lighting_store) return "Model";

    cJSON *current = persistent_dict_get(lighting_store, "current_model");
    if (current && current->valuestring) return current->valuestring;
    return "Model";
}

esp_err_t lighting_set_current_model(const char *model_name)
{
    persistent_dict_t *lighting_store = open_lighting_store();
    if (!lighting_store) return ESP_FAIL;

    cJSON *val = cJSON_CreateString(model_name);
    persistent_dict_set(lighting_store, "current_model", val);
    persistent_dict_save(lighting_store);
    return ESP_OK;
}

cJSON *lighting_get_settings(void)
{
    persistent_dict_t *lighting_store = open_lighting_store();
    if (!lighting_store) return NULL;

    cJSON *models = persistent_dict_get(lighting_store, "models");
    const char *current = lighting_get_current_model();
    if (!models || !current) return NULL;

    return cJSON_GetObjectItem(models, current);
}

esp_err_t lighting_create_model(const char *model_name, bool copy_from_current)
{
    /* Mirrors Lighting.create_model(): a new model either starts as a
     * minimal empty scaffold, or deep-copies the allowed keys from the
     * currently active model. */
    if (!model_name || model_name[0] == '\0') return ESP_ERR_INVALID_ARG;

    persistent_dict_t *lighting_store = open_lighting_store();
    if (!lighting_store) return ESP_FAIL;

    cJSON *models = persistent_dict_get(lighting_store, "models");
    if (!models || !cJSON_IsObject(models)) return ESP_FAIL;

    if (cJSON_GetObjectItem(models, model_name)) {
        ESP_LOGW(TAG, "Model already exists: %s", model_name);
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *new_model = cJSON_CreateObject();

    if (copy_from_current) {
        cJSON *current_name = persistent_dict_get(lighting_store, "current_model");
        cJSON *current_model = (current_name && current_name->valuestring)
            ? cJSON_GetObjectItem(models, current_name->valuestring) : NULL;

        static const char *allowed_keys[] = {
            "default_scene", "scenes", "named_ranges", "effects",
            "filters", "custom_colors", "scene_settings",
        };
        for (size_t i = 0; i < sizeof(allowed_keys) / sizeof(allowed_keys[0]); i++) {
            cJSON *src = current_model ? cJSON_GetObjectItem(current_model, allowed_keys[i]) : NULL;
            if (src) {
                cJSON_AddItemToObject(new_model, allowed_keys[i], cJSON_Duplicate(src, 1));
            } else if (strcmp(allowed_keys[i], "default_scene") == 0) {
                cJSON_AddItemToObject(new_model, allowed_keys[i], cJSON_CreateNull());
            } else {
                cJSON_AddItemToObject(new_model, allowed_keys[i], cJSON_CreateObject());
            }
        }
    } else {
        cJSON_AddItemToObject(new_model, "default_scene", cJSON_CreateNull());
        cJSON_AddItemToObject(new_model, "scenes", cJSON_CreateObject());
        cJSON_AddItemToObject(new_model, "named_ranges", cJSON_CreateObject());
        cJSON_AddItemToObject(new_model, "effects", cJSON_CreateObject());
        cJSON_AddItemToObject(new_model, "filters", cJSON_CreateObject());
        cJSON_AddItemToObject(new_model, "custom_colors", cJSON_CreateObject());
        cJSON_AddItemToObject(new_model, "scene_settings", cJSON_CreateObject());
    }

    cJSON_AddItemToObject(models, model_name, new_model);
    return persistent_dict_save(lighting_store);
}

esp_err_t lighting_delete_model(const char *model_name)
{
    /* Mirrors Lighting.delete_model(): refuses to delete the active model. */
    if (!model_name) return ESP_ERR_INVALID_ARG;

    persistent_dict_t *lighting_store = open_lighting_store();
    if (!lighting_store) return ESP_FAIL;

    cJSON *current_name = persistent_dict_get(lighting_store, "current_model");
    if (current_name && current_name->valuestring && strcmp(current_name->valuestring, model_name) == 0) {
        ESP_LOGW(TAG, "Cannot delete the currently active model: %s", model_name);
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *models = persistent_dict_get(lighting_store, "models");
    if (!models || !cJSON_GetObjectItem(models, model_name)) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON_DeleteItemFromObject(models, model_name);
    return persistent_dict_save(lighting_store);
}

esp_err_t lighting_rename_model(const char *old_name, const char *new_name)
{
    /* Mirrors Lighting.rename_model(): updates current_model too if the
     * renamed model was the active one. */
    if (!old_name || !new_name || new_name[0] == '\0') return ESP_ERR_INVALID_ARG;

    persistent_dict_t *lighting_store = open_lighting_store();
    if (!lighting_store) return ESP_FAIL;

    cJSON *models = persistent_dict_get(lighting_store, "models");
    if (!models) return ESP_FAIL;

    if (!cJSON_GetObjectItem(models, old_name)) return ESP_ERR_NOT_FOUND;
    if (cJSON_GetObjectItem(models, new_name)) return ESP_ERR_INVALID_STATE;

    cJSON *detached = cJSON_DetachItemFromObject(models, old_name);
    cJSON_AddItemToObject(models, new_name, detached);

    cJSON *current_name = persistent_dict_get(lighting_store, "current_model");
    if (current_name && current_name->valuestring && strcmp(current_name->valuestring, old_name) == 0) {
        persistent_dict_set(lighting_store, "current_model", cJSON_CreateString(new_name));
    }

    return persistent_dict_save(lighting_store);
}
