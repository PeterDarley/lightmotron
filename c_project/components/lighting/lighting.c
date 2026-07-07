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

static active_scene_t active_scenes[MAX_ACTIVE_SCENES];
static int active_scene_count = 0;
static rgb_t logical_colors[MAX_LEDS];
static SemaphoreHandle_t lighting_mutex = NULL;
static bool initialized = false;

/* Pre-allocated buffers for tick processing (avoids per-frame stack/heap alloc) */
static led_output_t tick_output_buf[MAX_LEDS];
static rgb_t tick_filter_buf[MAX_LEDS];

esp_err_t lighting_init(void)
{
    if (initialized) return ESP_OK;

    lighting_mutex = xSemaphoreCreateMutex();
    memset(logical_colors, 0, sizeof(logical_colors));
    memset(active_scenes, 0, sizeof(active_scenes));
    active_scene_count = 0;
    initialized = true;

    ESP_LOGI(TAG, "Lighting system initialized");
    return ESP_OK;
}

/**
 * Open the lighting settings store and return the currently-active model
 * object. Returns NULL if the store, models map, or current model is
 * missing.
 */
static cJSON *get_current_model_object(void)
{
    persistent_dict_t *lighting_store = persistent_dict_open("/spiffs/data/lighting_settings.json");
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
                bool dep_finished = false;
                for (int d = 0; d < scene->job_count; d++) {
                    if (strcmp(scene->jobs[d].name, job->after) == 0) {
                        dep_finished = scene->jobs[d].finished;
                        break;
                    }
                }
                if (!dep_finished) {
                    all_finished = false;
                    continue;
                }
                /* Start this job now, the first time its dependency finishes */
                if (job->after_pending) {
                    job->start_tick = tick;
                    job->after_pending = false;
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

            /* Execute pattern */
            pattern_fn_t pattern_fn = pattern_get(job->pattern_name);
            int output_count = pattern_fn(job, local_tick, tick_output_buf, MAX_LEDS);

            /* Apply filters */
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
                          output_count, &job->filters[f].state, tick);
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

            /* Free filter resources */
            for (int j = 0; j < scene->job_count; j++) {
                for (int f = 0; f < scene->jobs[j].filter_count; f++) {
                    if (scene->jobs[j].filters[f].params) {
                        cJSON_Delete(scene->jobs[j].filters[f].params);
                        scene->jobs[j].filters[f].params = NULL;
                    }
                    if (scene->jobs[j].filters[f].state.cached_params) {
                        cJSON_Delete(scene->jobs[j].filters[f].state.cached_params);
                        scene->jobs[j].filters[f].state.cached_params = NULL;
                    }
                }
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
                for (int f = 0; f < active_scenes[i].jobs[j].filter_count; f++) {
                    if (active_scenes[i].jobs[j].filters[f].params) {
                        cJSON_Delete(active_scenes[i].jobs[j].filters[f].params);
                    }
                    if (active_scenes[i].jobs[j].filters[f].state.cached_params) {
                        cJSON_Delete(active_scenes[i].jobs[j].filters[f].state.cached_params);
                    }
                }
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
            for (int f = 0; f < active_scenes[i].jobs[j].filter_count; f++) {
                if (active_scenes[i].jobs[j].filters[f].params) {
                    cJSON_Delete(active_scenes[i].jobs[j].filters[f].params);
                }
                if (active_scenes[i].jobs[j].filters[f].state.cached_params) {
                    cJSON_Delete(active_scenes[i].jobs[j].filters[f].state.cached_params);
                }
            }
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
    persistent_dict_t *lighting_store = persistent_dict_open("/spiffs/data/lighting_settings.json");
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
    persistent_dict_t *lighting_store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (!lighting_store) return "Model";

    cJSON *current = persistent_dict_get(lighting_store, "current_model");
    if (current && current->valuestring) return current->valuestring;
    return "Model";
}

esp_err_t lighting_set_current_model(const char *model_name)
{
    persistent_dict_t *lighting_store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (!lighting_store) return ESP_FAIL;

    cJSON *val = cJSON_CreateString(model_name);
    persistent_dict_set(lighting_store, "current_model", val);
    persistent_dict_save(lighting_store);
    return ESP_OK;
}

cJSON *lighting_get_settings(void)
{
    persistent_dict_t *lighting_store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (!lighting_store) return NULL;

    cJSON *models = persistent_dict_get(lighting_store, "models");
    const char *current = lighting_get_current_model();
    if (!models || !current) return NULL;

    return cJSON_GetObjectItem(models, current);
}
