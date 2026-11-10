#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "named_ranges.h"
#include "range_topology.h"
#include "json_helpers.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

#define MAX_TOPOLOGY_ROWS 32

/* Same parallel-array form-field reader used by views_led_grid.c (now
 * removed) and views_system.c -- duplicated per this project's one-helper-
 * per-file convention rather than shared. */
static const char *form_field_at(http_request_t *req, const char *name, int index)
{
    if (!req || !req->form_data) return NULL;
    cJSON *field = cJSON_GetObjectItem(req->form_data, name);
    if (!field) return NULL;
    if (cJSON_IsArray(field)) {
        cJSON *item = cJSON_GetArrayItem(field, index);
        return (item && cJSON_IsString(item)) ? item->valuestring : NULL;
    }
    return (index == 0 && cJSON_IsString(field)) ? field->valuestring : NULL;
}

static int form_field_count(http_request_t *req, const char *name)
{
    if (!req || !req->form_data) return 0;
    cJSON *field = cJSON_GetObjectItem(req->form_data, name);
    if (!field) return 0;
    if (cJSON_IsArray(field)) return cJSON_GetArraySize(field);
    return cJSON_IsString(field) ? 1 : 0;
}

static cJSON *get_model(void)
{
    return lighting_get_settings();
}

/* Builds the rows the template renders: either the range's saved topology,
 * or -- if it has none yet -- a single default row covering every one of
 * the range's current members (so turning 2D on starts from something
 * valid instead of an empty/undersized grid). */
static void add_topology_context(cJSON *ctx, const char *range_name)
{
    int members[MAX_LEDS];
    int member_count = named_ranges_resolve(range_name, members, MAX_LEDS);

    cJSON *model = get_model();
    cJSON *topology_dict = model ? cJSON_GetObjectItem(model, "range_topology") : NULL;
    cJSON *topology = topology_dict ? cJSON_GetObjectItem(topology_dict, range_name) : NULL;
    cJSON *segments_in = topology ? cJSON_GetObjectItem(topology, "segments") : NULL;
    cJSON *gaps_in = topology ? cJSON_GetObjectItem(topology, "gaps") : NULL;

    cJSON *rows = cJSON_CreateArray();
    int width = 0, height = 0, total_used = 0;

    if (segments_in && cJSON_IsArray(segments_in) && cJSON_GetArraySize(segments_in) > 0) {
        int n_segments = cJSON_GetArraySize(segments_in);
        for (int i = 0; i < n_segments; i++) {
            int count = json_get_int(cJSON_GetArrayItem(segments_in, i), "count", 1);
            if (count < 1) count = 1;
            if (count > width) width = count;
        }
        for (int i = 0; i < n_segments; i++) {
            cJSON *seg = cJSON_GetArrayItem(segments_in, i);
            int count = json_get_int(seg, "count", 1);
            if (count < 1) count = 1;
            const char *direction = json_get_string(seg, "direction", "normal");
            int gap_before = 0;
            if (i > 0 && gaps_in && cJSON_IsArray(gaps_in) && (i - 1) < cJSON_GetArraySize(gaps_in)) {
                cJSON *gap_item = cJSON_GetArrayItem(gaps_in, i - 1);
                if (cJSON_IsNumber(gap_item)) gap_before = gap_item->valueint;
            }
            if (gap_before < 0) gap_before = 0;
            height += gap_before + 1;

            cJSON *row = cJSON_CreateObject();
            cJSON_AddNumberToObject(row, "index", i);
            cJSON_AddBoolToObject(row, "is_first", i == 0);
            cJSON_AddNumberToObject(row, "count", count);
            cJSON_AddStringToObject(row, "direction", direction);
            cJSON_AddNumberToObject(row, "gap_before", gap_before);
            cJSON_AddItemToArray(rows, row);
            total_used += count;
        }
    } else if (member_count > 0) {
        cJSON *row = cJSON_CreateObject();
        cJSON_AddNumberToObject(row, "index", 0);
        cJSON_AddBoolToObject(row, "is_first", true);
        cJSON_AddNumberToObject(row, "count", member_count);
        cJSON_AddStringToObject(row, "direction", "normal");
        cJSON_AddNumberToObject(row, "gap_before", 0);
        cJSON_AddItemToArray(rows, row);
        width = member_count;
        height = 1;
        total_used = member_count;
    }

    cJSON_AddItemToObject(ctx, "topo_rows", rows);
    cJSON_AddStringToObject(ctx, "topo_range_name", range_name);
    cJSON_AddNumberToObject(ctx, "topo_member_count", member_count);
    cJSON_AddNumberToObject(ctx, "topo_width", width);
    cJSON_AddNumberToObject(ctx, "topo_height", height);
    cJSON_AddNumberToObject(ctx, "topo_total_used", total_used);
    cJSON_AddBoolToObject(ctx, "topo_is_2d", range_topology_exists(range_name));
}

http_response_t *view_named_range_topology(http_request_t *req)
{
    if (req->method == HTTP_METHOD_GET) {
        const char *range_name = request_get_query_param(req, "name");
        if (!range_name || !range_name[0]) {
            return response_create(200, "text/html", "<p class=\"text-danger small\">No range specified.</p>");
        }
        cJSON *ctx = build_global_context();
        add_topology_context(ctx, range_name);
        return webserver_render_response("setup/named_range_topology.html", ctx);
    }

    /* POST: save this range's row layout. */
    const char *range_name = request_get_form_field(req, "name");
    if (!range_name || !range_name[0]) {
        return response_create(200, "text/html", "<p class=\"text-danger small\">No range specified.</p>");
    }

    int members[MAX_LEDS];
    int member_count = named_ranges_resolve(range_name, members, MAX_LEDS);

    int n_rows = form_field_count(req, "segment_count");
    if (n_rows > MAX_TOPOLOGY_ROWS) n_rows = MAX_TOPOLOGY_ROWS;

    cJSON *segments = cJSON_CreateArray();
    cJSON *gaps = cJSON_CreateArray();
    int total_used = 0;
    const char *error = NULL;

    for (int i = 0; i < n_rows; i++) {
        const char *count_s = form_field_at(req, "segment_count", i);
        const char *dir_s = form_field_at(req, "segment_direction", i);
        const char *gap_s = form_field_at(req, "segment_gap_before", i);

        int count_val = count_s ? atoi(count_s) : 0;
        if (count_val < 1) { error = "Every row needs at least 1 LED."; break; }

        bool reversed = dir_s && strcmp(dir_s, "reversed") == 0;
        int gap_val = (i > 0 && gap_s) ? atoi(gap_s) : 0;
        if (gap_val < 0) gap_val = 0;

        cJSON *seg = cJSON_CreateObject();
        cJSON_AddNumberToObject(seg, "count", count_val);
        cJSON_AddStringToObject(seg, "direction", reversed ? "reversed" : "normal");
        cJSON_AddItemToArray(segments, seg);
        if (i > 0) cJSON_AddItemToArray(gaps, cJSON_CreateNumber(gap_val));

        total_used += count_val;
    }

    if (!error && total_used > member_count) {
        error = "These rows need more LEDs than this range has. Add more LEDs to the range, or reduce the row counts.";
    }

    if (error) {
        cJSON_Delete(segments);
        cJSON_Delete(gaps);
        cJSON *ctx = build_global_context();
        add_topology_context(ctx, range_name);
        cJSON_AddStringToObject(ctx, "error", error);
        return webserver_render_response("setup/named_range_topology.html", ctx);
    }

    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    cJSON *model = get_model();
    cJSON *topology_dict = model ? cJSON_GetObjectItem(model, "range_topology") : NULL;
    if (model && !topology_dict) {
        topology_dict = cJSON_CreateObject();
        cJSON_AddItemToObject(model, "range_topology", topology_dict);
    }
    if (topology_dict) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddItemToObject(entry, "segments", segments);
        cJSON_AddItemToObject(entry, "gaps", gaps);
        cJSON_DeleteItemFromObject(topology_dict, range_name);
        cJSON_AddItemToObject(topology_dict, range_name, entry);
        persistent_dict_mark_dirty(store);
        persistent_dict_save(store);
    } else {
        cJSON_Delete(segments);
        cJSON_Delete(gaps);
    }

    cJSON *ctx = build_global_context();
    add_topology_context(ctx, range_name);
    cJSON_AddStringToObject(ctx, "message", "2D layout saved.");
    return webserver_render_response("setup/named_range_topology.html", ctx);
}

http_response_t *view_named_range_topology_disable(http_request_t *req)
{
    const char *range_name = request_get_form_field(req, "name");
    if (range_name && range_name[0]) {
        cJSON *model = get_model();
        cJSON *topology_dict = model ? cJSON_GetObjectItem(model, "range_topology") : NULL;
        if (topology_dict && cJSON_GetObjectItem(topology_dict, range_name)) {
            cJSON_DeleteItemFromObject(topology_dict, range_name);
            persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
            persistent_dict_mark_dirty(store);
            persistent_dict_save(store);
        }
    }

    cJSON *ctx = build_global_context();
    if (range_name && range_name[0]) add_topology_context(ctx, range_name);
    cJSON_AddStringToObject(ctx, "message", "2D layout disabled -- this range is 1D again.");
    return webserver_render_response("setup/named_range_topology.html", ctx);
}
