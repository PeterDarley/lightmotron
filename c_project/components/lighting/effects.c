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

static void resolve_filters(const cJSON *filters_array, active_job_t *job)
{
    job->filter_count = 0;
    if (!filters_array || !cJSON_IsArray(filters_array)) return;

    int size = cJSON_GetArraySize(filters_array);
    if (size > 8) size = 8;

    for (int i = 0; i < size; i++) {
        cJSON *filter_def = cJSON_GetArrayItem(filters_array, i);
        const char *filter_name = json_get_string(filter_def, "filter", "null");
        strncpy(job->filters[i].filter_name, filter_name,
                sizeof(job->filters[i].filter_name) - 1);
        job->filters[i].params = cJSON_Duplicate(filter_def, 1);
        memset(&job->filters[i].state, 0, sizeof(filter_state_t));
        job->filter_count++;
    }
}

void effect_resolve(const cJSON *effect_def, active_job_t *job)
{
    if (!effect_def || !job) return;

    memset(job, 0, sizeof(active_job_t));

    strncpy(job->pattern_name,
            json_get_string(effect_def, "pattern", "solid"),
            sizeof(job->pattern_name) - 1);

    job->duration = json_get_int(effect_def, "duration", 40);
    job->cycles = json_get_int(effect_def, "cycles", 0);
    job->period = json_get_int(effect_def, "period", 0);
    job->width = json_get_int(effect_def, "width", 5);
    job->number = json_get_int(effect_def, "number", 1);
    job->reverse = json_get_bool(effect_def, "reverse", false);

    const char *target = json_get_string(effect_def, "target", NULL);
    resolve_target(target, job);

    cJSON *colors = json_get_array(effect_def, "colors");
    resolve_colors(colors, job);

    cJSON *filters = json_get_array(effect_def, "filters");
    resolve_filters(filters, job);

    const char *after = json_get_string(effect_def, "after", NULL);
    if (after) {
        strncpy(job->after, after, sizeof(job->after) - 1);
    }
}

void effect_resolve_inline(const cJSON *job_def, active_job_t *job)
{
    /* Inline jobs use the same format as effect definitions */
    effect_resolve(job_def, job);
}
