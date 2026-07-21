#include "named_ranges.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "leds.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "named_ranges";

#define MAX_RECURSION_DEPTH 8

/*
 * Append `value` to `indices` if it is not already present, matching the
 * dedup behavior of Lighting.get_targets()'s `_append_unique` /
 * `seen` set: the same LED index can be reached via multiple targets
 * (direct + a named range containing it, or several overlapping ranges),
 * but should only be counted once.
 */
static void append_unique(int *indices, int *count, int max_indices, int value)
{
    if (*count >= max_indices) return;

    for (int i = 0; i < *count; i++) {
        if (indices[i] == value) return;
    }

    indices[(*count)++] = value;
}

/*
 * Resolves a named range into `indices`, writing new entries starting at
 * *count and updating *count in place. `depth` guards against runaway
 * recursion from a cyclic "named:X" reference; Python's get_targets()
 * detects true cycles precisely via a per-path ancestor set, but a depth
 * cap achieves the same practical goal (no infinite loop) without needing
 * to track visited names through the recursion.
 */
static void resolve_recursive(const char *range_name, int *indices, int *count, int max_indices, int depth)
{
    if (depth > MAX_RECURSION_DEPTH) {
        ESP_LOGW(TAG, "Max recursion depth reached for range: %s", range_name);
        return;
    }

    persistent_dict_t *lighting = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    if (!lighting) return;

    cJSON *models = persistent_dict_get(lighting, "models");
    cJSON *current_name = persistent_dict_get(lighting, "current_model");
    if (!models || !current_name || !current_name->valuestring) return;

    cJSON *model = cJSON_GetObjectItem(models, current_name->valuestring);
    if (!model) return;

    cJSON *named_ranges = cJSON_GetObjectItem(model, "named_ranges");
    if (!named_ranges) return;

    cJSON *range_def = cJSON_GetObjectItem(named_ranges, range_name);
    if (!range_def || !cJSON_IsArray(range_def)) return;

    int size = cJSON_GetArraySize(range_def);

    for (int i = 0; i < size && *count < max_indices; i++) {
        cJSON *item = cJSON_GetArrayItem(range_def, i);

        if (cJSON_IsNumber(item)) {
            /* Direct index */
            append_unique(indices, count, max_indices, item->valueint);

        } else if (cJSON_IsString(item) && item->valuestring) {
            /* Check for "named:X" reference */
            if (strncmp(item->valuestring, "named:", 6) == 0) {
                resolve_recursive(item->valuestring + 6, indices, count, max_indices, depth + 1);
            }

        } else if (cJSON_IsObject(item)) {
            /* Range object: {"from": x, "to": y} */
            int from = json_get_int(item, "from", 0);
            int to = json_get_int(item, "to", 0);
            for (int idx = from; idx <= to && *count < max_indices; idx++) {
                append_unique(indices, count, max_indices, idx);
            }
        }
    }
}

int named_ranges_resolve(const char *range_name, int *indices, int max_indices)
{
    if (!range_name || !indices || max_indices <= 0) return 0;

    int count = 0;
    resolve_recursive(range_name, indices, &count, max_indices, 0);
    return count;
}

int target_spec_resolve(const char *spec, int *indices, int max_indices)
{
    if (!spec || !indices || max_indices <= 0) return 0;

    int count = 0;
    char *copy = strdup(spec);
    if (!copy) return 0;

    /* Split by comma */
    char *token = strtok(copy, ",");
    while (token && count < max_indices) {
        /* Trim whitespace */
        while (*token == ' ') token++;
        size_t tlen = strlen(token);
        while (tlen > 0 && token[tlen - 1] == ' ') token[--tlen] = '\0';

        if (strncmp(token, "named:", 6) == 0) {
            /* Named range reference - resolve directly into the shared
             * indices/count so dedup applies across the whole spec, same
             * as get_targets() merging nested results via _append_unique. */
            resolve_recursive(token + 6, indices, &count, max_indices, 0);

        } else if (strcmp(token, "all") == 0) {
            /* Matches get_targets()'s `target == "all"` case: every LED. */
            int total = leds_total_count();
            for (int i = 0; i < total && count < max_indices; i++) {
                append_unique(indices, &count, max_indices, i);
            }

        } else if (strchr(token, '-')) {
            /* Range: "from-to" */
            int from = 0, to = 0;
            if (sscanf(token, "%d-%d", &from, &to) == 2) {
                for (int i = from; i <= to && count < max_indices; i++) {
                    append_unique(indices, &count, max_indices, i);
                }
            }

        } else {
            /* Single index */
            char *endptr;
            int idx = strtol(token, &endptr, 10);
            if (*endptr == '\0') {
                append_unique(indices, &count, max_indices, idx);
            }
        }

        token = strtok(NULL, ",");
    }

    free(copy);
    return count;
}
