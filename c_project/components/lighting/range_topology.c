#include "range_topology.h"
#include "named_ranges.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "range_topology";

static cJSON *get_model(void)
{
    persistent_dict_t *lighting = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    if (!lighting) return NULL;
    cJSON *models = persistent_dict_get(lighting, "models");
    cJSON *current_name = persistent_dict_get(lighting, "current_model");
    if (!models || !current_name || !current_name->valuestring) return NULL;
    return cJSON_GetObjectItem(models, current_name->valuestring);
}

static cJSON *get_topology(const char *range_name)
{
    cJSON *model = get_model();
    cJSON *topology_dict = model ? cJSON_GetObjectItem(model, "range_topology") : NULL;
    return topology_dict ? cJSON_GetObjectItem(topology_dict, range_name) : NULL;
}

bool range_topology_exists(const char *range_name)
{
    if (!range_name) return false;
    return get_topology(range_name) != NULL;
}

static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

bool range_topology_resolve(const char *range_name, active_job_t *job)
{
    job->target_count = 0;
    job->grid_width = 0;
    job->grid_height = 0;
    if (!range_name) return false;

    cJSON *topology = get_topology(range_name);
    cJSON *segments = topology ? cJSON_GetObjectItem(topology, "segments") : NULL;
    if (!segments || !cJSON_IsArray(segments)) return false;

    int members[MAX_LEDS];
    int member_count = named_ranges_resolve(range_name, members, MAX_LEDS);
    if (member_count <= 0) return false;
    qsort(members, member_count, sizeof(int), cmp_int);

    cJSON *gaps = cJSON_GetObjectItem(topology, "gaps");
    int n_segments = cJSON_GetArraySize(segments);

    int width = 0;
    for (int i = 0; i < n_segments; i++) {
        int count = json_get_int(cJSON_GetArrayItem(segments, i), "count", 1);
        if (count < 1) count = 1;
        if (count > width) width = count;
    }
    if (width <= 0) return false;

    /* job was memset(0) by effect_resolve() -- 0 is a valid LED index, so
     * every cell this loop doesn't explicitly touch (gap rows, short rows,
     * anything beyond the grid) must be set to -1 first, not left at 0. */
    for (int i = 0; i < MAX_LEDS; i++) job->target_indices[i] = -1;

    int row = 0;
    int member_cursor = 0;
    for (int i = 0; i < n_segments && row < MAX_LEDS; i++) {
        cJSON *seg = cJSON_GetArrayItem(segments, i);
        int count = json_get_int(seg, "count", 1);
        if (count < 1) count = 1;
        bool reversed = strcmp(json_get_string(seg, "direction", "normal"), "reversed") == 0;

        int gap_before = 0;
        if (i > 0 && gaps && cJSON_IsArray(gaps) && (i - 1) < cJSON_GetArraySize(gaps)) {
            cJSON *gap_item = cJSON_GetArrayItem(gaps, i - 1);
            if (cJSON_IsNumber(gap_item)) gap_before = gap_item->valueint;
        }
        if (gap_before < 0) gap_before = 0;
        row += gap_before;

        if (row < MAX_LEDS) {
            for (int col = 0; col < count && col < width; col++) {
                int cell_index = row * width + col;
                if (cell_index < 0 || cell_index >= MAX_LEDS) continue;
                int member_pos = member_cursor + (reversed ? (count - 1 - col) : col);
                job->target_indices[cell_index] = (member_pos < member_count) ? members[member_pos] : -1;
            }
        }
        member_cursor += count;
        row += 1;
    }

    int height = row;
    if ((long)width * height > MAX_LEDS) {
        ESP_LOGW(TAG, "Range '%s' topology %dx%d (%d cells) exceeds the %d-cell job target capacity",
                 range_name, width, height, width * height, MAX_LEDS);
        height = MAX_LEDS / width;
    }

    job->grid_width = width;
    job->grid_height = height;
    job->target_count = width * height;
    return true;
}
