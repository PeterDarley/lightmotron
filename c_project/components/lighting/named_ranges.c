#include "named_ranges.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "named_ranges";

#define MAX_RECURSION_DEPTH 8

static int resolve_recursive(const char *range_name, int *indices, int max_indices, int depth)
{
    if (depth > MAX_RECURSION_DEPTH) {
        ESP_LOGW(TAG, "Max recursion depth reached for range: %s", range_name);
        return 0;
    }

    persistent_dict_t *lighting = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (!lighting) return 0;

    cJSON *models = persistent_dict_get(lighting, "models");
    cJSON *current_name = persistent_dict_get(lighting, "current_model");
    if (!models || !current_name || !current_name->valuestring) return 0;

    cJSON *model = cJSON_GetObjectItem(models, current_name->valuestring);
    if (!model) return 0;

    cJSON *named_ranges = cJSON_GetObjectItem(model, "named_ranges");
    if (!named_ranges) return 0;

    cJSON *range_def = cJSON_GetObjectItem(named_ranges, range_name);
    if (!range_def || !cJSON_IsArray(range_def)) return 0;

    int count = 0;
    int size = cJSON_GetArraySize(range_def);

    for (int i = 0; i < size && count < max_indices; i++) {
        cJSON *item = cJSON_GetArrayItem(range_def, i);

        if (cJSON_IsNumber(item)) {
            /* Direct index */
            indices[count++] = item->valueint;

        } else if (cJSON_IsString(item) && item->valuestring) {
            /* Check for "named:X" reference */
            if (strncmp(item->valuestring, "named:", 6) == 0) {
                int sub_count = resolve_recursive(
                    item->valuestring + 6, indices + count,
                    max_indices - count, depth + 1);
                count += sub_count;
            }

        } else if (cJSON_IsObject(item)) {
            /* Range object: {"from": x, "to": y} */
            int from = json_get_int(item, "from", 0);
            int to = json_get_int(item, "to", 0);
            for (int idx = from; idx <= to && count < max_indices; idx++) {
                indices[count++] = idx;
            }
        }
    }

    return count;
}

int named_ranges_resolve(const char *range_name, int *indices, int max_indices)
{
    if (!range_name || !indices || max_indices <= 0) return 0;
    return resolve_recursive(range_name, indices, max_indices, 0);
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
            /* Named range reference */
            int sub_count = named_ranges_resolve(
                token + 6, indices + count, max_indices - count);
            count += sub_count;

        } else if (strchr(token, '-')) {
            /* Range: "from-to" */
            int from = 0, to = 0;
            if (sscanf(token, "%d-%d", &from, &to) == 2) {
                for (int i = from; i <= to && count < max_indices; i++) {
                    indices[count++] = i;
                }
            }

        } else {
            /* Single index */
            char *endptr;
            int idx = strtol(token, &endptr, 10);
            if (*endptr == '\0') {
                indices[count++] = idx;
            }
        }

        token = strtok(NULL, ",");
    }

    free(copy);
    return count;
}
