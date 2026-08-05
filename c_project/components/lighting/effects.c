#include "effects.h"
#include "colors.h"
#include "named_ranges.h"
#include "json_helpers.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "effects";

static void resolve_colors(const cJSON *colors_array, active_job_t *job)
{
    job->color_count = 0;
    if (!colors_array || !cJSON_IsArray(colors_array)) return;

    int size = cJSON_GetArraySize(colors_array);
    if (size > 8) size = 8;

    for (int i = 0; i < size; i++) {
        cJSON *item = cJSON_GetArrayItem(colors_array, i);
        job->colors[i] = color_resolve_json(item);
        job->color_count++;
    }
}

static void resolve_target(const char *target_spec, active_job_t *job)
{
    job->target_count = 0;
    if (!target_spec) return;

    job->target_count = target_spec_resolve(target_spec, job->target_indices, MAX_LEDS);
}

static void resolve_filters(const cJSON *filters_array, const cJSON *filters_dict, active_job_t *job)
{
    job->filter_count = 0;
    if (!filters_array || !cJSON_IsArray(filters_array)) return;

    int size = cJSON_GetArraySize(filters_array);
    if (size > 8) size = 8;

    for (int i = 0; i < size; i++) {
        cJSON *entry = cJSON_GetArrayItem(filters_array, i);

        /* Each entry is normally a *name* referencing model.filters[name]
         * (e.g. "Thrusters Flicker" -> {"filter":"scintillate", ...}) --
         * matches how the effect editor (views_effects.c) reads/writes this
         * array. Still accept an inline object directly too, for anyone
         * hand-authoring a filter without registering it under a name. */
        cJSON *filter_def = entry;
        if (cJSON_IsString(entry) && entry->valuestring) {
            filter_def = filters_dict ? cJSON_GetObjectItem(filters_dict, entry->valuestring) : NULL;
            if (!filter_def) continue; /* Named filter not found -- skip it. */
        }

        const char *filter_name = json_get_string(filter_def, "filter", "null");
        strncpy(job->filters[job->filter_count].filter_name, filter_name,
                sizeof(job->filters[job->filter_count].filter_name) - 1);
        job->filters[job->filter_count].params = cJSON_Duplicate(filter_def, 1);
        memset(&job->filters[job->filter_count].state, 0, sizeof(filter_state_t));
        job->filter_count++;
    }
}

void effect_resolve(const cJSON *effect_def, const cJSON *filters_dict, active_job_t *job)
{
    if (!effect_def || !job) return;

    memset(job, 0, sizeof(active_job_t));

    strncpy(job->pattern_name,
            json_get_string(effect_def, "pattern", "solid"),
            sizeof(job->pattern_name) - 1);

    /* Leave duration/period at the "unset" sentinel of 0 rather than a
     * blanket default: each pattern_*() function in patterns.c applies its
     * own pattern-specific default (e.g. pulse's on-duration defaults to 10,
     * not 40), mirroring the per-call effect.get("duration", X) defaults
     * used throughout lib/lighting/patterns.py. Forcing a single default
     * here would shadow those per-pattern defaults. */
    job->duration = json_get_int(effect_def, "duration", 0);
    job->cycles = json_get_int(effect_def, "cycles", 0);
    job->period = json_get_int(effect_def, "period", 0);
    job->width = json_get_int(effect_def, "width", 5);
    job->number = json_get_int(effect_def, "number", 1);
    job->reverse = json_get_bool(effect_def, "reverse", false);
    /* Additional per-pattern params (see active_job_t). Defaults match the
     * effect.get(name, default) fallbacks in the Python patterns. */
    job->saturation = (float)json_get_double(effect_def, "saturation", 1.0);
    job->cooling = json_get_int(effect_def, "cooling", 55);
    job->sparking = json_get_int(effect_def, "sparking", 120);
    job->spacing = json_get_int(effect_def, "spacing", 3);

    const char *target = json_get_string(effect_def, "target", NULL);
    resolve_target(target, job);

    cJSON *colors = json_get_array(effect_def, "colors");
    resolve_colors(colors, job);

    cJSON *filters = json_get_array(effect_def, "filters");
    resolve_filters(filters, filters_dict, job);

    const char *after = json_get_string(effect_def, "after", NULL);
    if (after) {
        strncpy(job->after, after, sizeof(job->after) - 1);
    }

    /* Legacy "frequency" support for the blink pattern (mirrors
     * pattern_blink()'s runtime fallback in patterns.py, kept for configs
     * that predate Lighting.convert_frequencies_to_durations()). Resolved
     * once here since frequency is static for the life of the job. */
    cJSON *freq_item = cJSON_GetObjectItem(effect_def, "frequency");
    if (freq_item && cJSON_IsNumber(freq_item) && freq_item->valuedouble > 0 &&
        strcmp(job->pattern_name, "blink") == 0) {
        int half_period = (int)(20.0 / freq_item->valuedouble + 0.5);
        if (half_period < 1) half_period = 1;
        job->duration = half_period;
    }
}

void effect_resolve_inline(const cJSON *job_def, const cJSON *filters_dict, active_job_t *job)
{
    /* Inline jobs use the same format as effect definitions */
    effect_resolve(job_def, filters_dict, job);
}
