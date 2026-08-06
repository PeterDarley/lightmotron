/*
 * Filter management views: list/create/delete filters, edit an individual
 * filter's type and parameters (including the Standard/Custom color
 * dropdown + picker used by the "spike" filter's Color param), and the
 * shared color-select fragment endpoint used by the filter editor.
 *
 * Mirrors web/views.py: FiltersView, FilterEditView, _filter_edit_context(),
 * ColorSelectView (ported here for the /filters/color_select route), plus
 * the module-level helpers _filters_list(), _rename_filter_refs(),
 * _color_name_to_hex(), and _STANDARD_COLORS.
 */
#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "lighting.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------
 * Filter metadata — mirrors lib/lighting/metadata.py's FILTER_METADATA.
 * ------------------------------------------------------------------- */

typedef struct {
    const char *type_name;
    const char *description;
    const char *optional[8];
} filter_meta_entry_t;

static const filter_meta_entry_t FILTER_METADATA[] = {
    { "null",        "No filter",                    { NULL } },
    { "sizzle",      "Sizzle filter",                { "frequency", "variation_percent", "heat", NULL } },
    { "scintillate", "Scintillate filter",           { "frequency", "variation_percent", "heat", NULL } },
    { "brightness",  "Brightness filter",            { "brightness", NULL } },
    { "spike",       "Spike filter",                 { "color", "duration", "period", "variation", "heat", "scope", NULL } },
    { "dropout",     "Dropout filter (black spike)", { "duration", "period", "variation", "heat", "scope", NULL } },
};
#define FILTER_METADATA_COUNT (sizeof(FILTER_METADATA) / sizeof(FILTER_METADATA[0]))

static const filter_meta_entry_t *find_filter_meta(const char *type_name)
{
    if (!type_name) return NULL;
    for (size_t i = 0; i < FILTER_METADATA_COUNT; i++) {
        if (strcmp(FILTER_METADATA[i].type_name, type_name) == 0) return &FILTER_METADATA[i];
    }
    return NULL;
}

static cJSON *build_filter_metadata_json(void)
{
    cJSON *root = cJSON_CreateObject();
    for (size_t i = 0; i < FILTER_METADATA_COUNT; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "description", FILTER_METADATA[i].description);
        cJSON *optional = cJSON_CreateArray();
        for (int j = 0; FILTER_METADATA[i].optional[j] != NULL; j++) {
            cJSON_AddItemToArray(optional, cJSON_CreateString(FILTER_METADATA[i].optional[j]));
        }
        cJSON_AddItemToObject(entry, "optional", optional);
        cJSON_AddItemToObject(root, FILTER_METADATA[i].type_name, entry);
    }
    return root;
}

/* ---------------------------------------------------------------------
 * Standard color table + hex helpers — mirrors _STANDARD_COLORS and
 * _color_name_to_hex() in web/views.py. Deliberately only the five
 * "Standard" dropdown colors; custom_colors covers everything else.
 * ------------------------------------------------------------------- */

static bool standard_color_rgb(const char *name, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!name) return false;
    if (strcmp(name, "white") == 0) { *r = 255; *g = 255; *b = 255; return true; }
    if (strcmp(name, "black") == 0) { *r = 0;   *g = 0;   *b = 0;   return true; }
    if (strcmp(name, "red") == 0)   { *r = 255; *g = 0;   *b = 0;   return true; }
    if (strcmp(name, "green") == 0) { *r = 0;   *g = 255; *b = 0;   return true; }
    if (strcmp(name, "blue") == 0)  { *r = 0;   *g = 0;   *b = 255; return true; }
    return false;
}

static void hex_from_rgb(uint8_t r, uint8_t g, uint8_t b, char *out /* >= 8 bytes */)
{
    snprintf(out, 8, "#%02X%02X%02X", r, g, b);
}

/* Resolves a color name (standard or custom) to a "#RRGGBB" string,
 * defaulting to "#FF0000" when unresolved — matches _color_name_to_hex(). */
static void color_name_to_hex(const char *name, cJSON *custom_colors, char *out /* >= 8 bytes */)
{
    uint8_t r, g, b;
    if (standard_color_rgb(name, &r, &g, &b)) {
        hex_from_rgb(r, g, b, out);
        return;
    }
    if (custom_colors && name) {
        cJSON *rgb = cJSON_GetObjectItem(custom_colors, name);
        if (rgb && cJSON_IsArray(rgb) && cJSON_GetArraySize(rgb) >= 3) {
            cJSON *ri = cJSON_GetArrayItem(rgb, 0);
            cJSON *gi = cJSON_GetArrayItem(rgb, 1);
            cJSON *bi = cJSON_GetArrayItem(rgb, 2);
            hex_from_rgb((uint8_t)ri->valueint, (uint8_t)gi->valueint, (uint8_t)bi->valueint, out);
            return;
        }
    }
    snprintf(out, 8, "#FF0000");
}

static bool is_hex6(const char *s)
{
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return s[6] == '\0';
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Renders a cJSON scalar the way Python's str() would for the values we
 * store in filter params (ints, floats, or plain strings). */
static void json_value_to_str(const cJSON *item, char *out, size_t outsz)
{
    if (!item) { out[0] = '\0'; return; }
    if (cJSON_IsString(item)) { snprintf(out, outsz, "%s", item->valuestring); return; }
    if (cJSON_IsNumber(item)) {
        double v = item->valuedouble;
        if (v == (double)(long long)v) {
            snprintf(out, outsz, "%lld", (long long)v);
        } else {
            snprintf(out, outsz, "%g", v);
        }
        return;
    }
    out[0] = '\0';
}

/* ---------------------------------------------------------------------
 * Filter list building — mirrors _filters_list().
 * ------------------------------------------------------------------- */

static cJSON *build_filters_list(cJSON *filters_dict, cJSON *filter_order)
{
    cJSON *result = cJSON_CreateArray();
    if (!filters_dict) return result;

    int total = cJSON_GetArraySize(filters_dict);
    if (total <= 0) return result;

    const char **names = calloc(total, sizeof(char *));
    bool *emitted = calloc(total, sizeof(bool));
    if (!names || !emitted) { free(names); free(emitted); return result; }

    int idx = 0;
    for (cJSON *item = filters_dict->child; item; item = item->next) {
        names[idx++] = item->string;
    }

    if (filter_order && cJSON_IsArray(filter_order)) {
        int order_count = cJSON_GetArraySize(filter_order);
        for (int oi = 0; oi < order_count; oi++) {
            cJSON *on = cJSON_GetArrayItem(filter_order, oi);
            if (!on || !cJSON_IsString(on)) continue;
            for (int i = 0; i < total; i++) {
                if (emitted[i] || strcmp(names[i], on->valuestring) != 0) continue;
                cJSON *def = cJSON_GetObjectItem(filters_dict, names[i]);
                cJSON *entry = cJSON_CreateObject();
                cJSON_AddStringToObject(entry, "name", names[i]);
                cJSON_AddStringToObject(entry, "filter_type", json_get_string(def, "filter", ""));
                cJSON_AddItemToArray(result, entry);
                emitted[i] = true;
                break;
            }
        }
    }

    const char **remaining = calloc(total, sizeof(char *));
    int remaining_count = 0;
    if (remaining) {
        for (int i = 0; i < total; i++) {
            if (!emitted[i]) remaining[remaining_count++] = names[i];
        }
        qsort(remaining, remaining_count, sizeof(char *), cmp_str);
        for (int i = 0; i < remaining_count; i++) {
            cJSON *def = cJSON_GetObjectItem(filters_dict, remaining[i]);
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "name", remaining[i]);
            cJSON_AddStringToObject(entry, "filter_type", json_get_string(def, "filter", ""));
            cJSON_AddItemToArray(result, entry);
        }
    }

    free(names);
    free(emitted);
    free(remaining);
    return result;
}

/* Updates any effect's "filters" list entries that reference old_name,
 * replacing them with new_name (or JSON null when new_name is NULL, on
 * delete) — mirrors _rename_filter_refs(). */
static void rename_filter_refs(cJSON *model, const char *old_name, const char *new_name)
{
    if (!model || !old_name) return;
    cJSON *effects = cJSON_GetObjectItem(model, "effects");
    if (!effects) return;

    for (cJSON *effect = effects->child; effect; effect = effect->next) {
        cJSON *filters = cJSON_GetObjectItem(effect, "filters");
        if (!filters || !cJSON_IsArray(filters)) continue;
        int n = cJSON_GetArraySize(filters);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(filters, i);
            if (item && cJSON_IsString(item) && strcmp(item->valuestring, old_name) == 0) {
                cJSON *replacement = new_name ? cJSON_CreateString(new_name) : cJSON_CreateNull();
                cJSON_ReplaceItemInArray(filters, i, replacement);
            }
        }
    }
}

/* ---------------------------------------------------------------------
 * Edit-context building — mirrors _filter_edit_context().
 * ------------------------------------------------------------------- */

static void fill_filter_edit_context(cJSON *ctx, cJSON *model, const char *filter_name)
{
    cJSON *filters_dict = model ? cJSON_GetObjectItem(model, "filters") : NULL;
    cJSON *filter_order = model ? cJSON_GetObjectItem(model, "filter_order") : NULL;
    cJSON *custom_colors = model ? cJSON_GetObjectItem(model, "custom_colors") : NULL;

    cJSON_AddItemToObject(ctx, "filters", build_filters_list(filters_dict, filter_order));
    cJSON_AddItemToObject(ctx, "filter_metadata", build_filter_metadata_json());
    if (custom_colors) cJSON_AddItemReferenceToObject(ctx, "custom_colors", custom_colors);
    cJSON_AddStringToObject(ctx, "page_title", "Filters");

    cJSON *filter_def = (filters_dict && filter_name) ? cJSON_GetObjectItem(filters_dict, filter_name) : NULL;
    if (!filter_def) return;

    const char *filter_type = json_get_string(filter_def, "filter", "");
    cJSON_AddStringToObject(ctx, "edit_filter_name", filter_name);
    cJSON_AddStringToObject(ctx, "edit_filter_type", filter_type);
    cJSON_AddStringToObject(ctx, "old_filter_name", filter_name);

    if (strcmp(filter_type, "sizzle") == 0 || strcmp(filter_type, "scintillate") == 0) {
        double variation_percent = 20.0;
        cJSON *vp = cJSON_GetObjectItem(filter_def, "variation_percent");
        cJSON *legacy_v = cJSON_GetObjectItem(filter_def, "variation");
        if (vp && cJSON_IsNumber(vp)) {
            variation_percent = vp->valuedouble;
        } else if (legacy_v && cJSON_IsNumber(legacy_v)) {
            variation_percent = (legacy_v->valuedouble / 255.0) * 100.0;
        }
        if (variation_percent < 0.0) variation_percent = 0.0;
        if (variation_percent > 100.0) variation_percent = 100.0;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", variation_percent);
        cJSON_AddStringToObject(ctx, "filter_val_variation_percent", buf);
    }

    const filter_meta_entry_t *meta = find_filter_meta(filter_type);
    if (meta) {
        for (int i = 0; meta->optional[i] != NULL; i++) {
            cJSON *val = cJSON_GetObjectItem(filter_def, meta->optional[i]);
            if (!val) continue;
            char key[64];
            snprintf(key, sizeof(key), "filter_val_%s", meta->optional[i]);
            char strval[64];
            json_value_to_str(val, strval, sizeof(strval));
            cJSON_AddStringToObject(ctx, key, strval);
        }
    }

    /* Resolve the "color" param into dropdown/swatch context. */
    cJSON *color_item = cJSON_GetObjectItem(filter_def, "color");
    if (color_item && cJSON_IsArray(color_item) && cJSON_GetArraySize(color_item) == 3) {
        cJSON *ri = cJSON_GetArrayItem(color_item, 0);
        cJSON *gi = cJSON_GetArrayItem(color_item, 1);
        cJSON *bi = cJSON_GetArrayItem(color_item, 2);
        char hex[8];
        hex_from_rgb((uint8_t)ri->valueint, (uint8_t)gi->valueint, (uint8_t)bi->valueint, hex);
        cJSON_AddStringToObject(ctx, "filter_color_hex", hex);
        cJSON_AddStringToObject(ctx, "filter_color_selected", "__picker__");
        cJSON_AddStringToObject(ctx, "filter_color_display_name", "");
        cJSON_AddBoolToObject(ctx, "filter_color_is_named", false);
        cJSON_AddBoolToObject(ctx, "filter_color_is_picker", true);
    } else if (color_item && cJSON_IsString(color_item) && color_item->valuestring[0] == '#') {
        cJSON_AddStringToObject(ctx, "filter_color_hex", color_item->valuestring);
        cJSON_AddStringToObject(ctx, "filter_color_selected", "__picker__");
        cJSON_AddStringToObject(ctx, "filter_color_display_name", "");
        cJSON_AddBoolToObject(ctx, "filter_color_is_named", false);
        cJSON_AddBoolToObject(ctx, "filter_color_is_picker", true);
    } else if (color_item && cJSON_IsString(color_item)) {
        const char *existing = color_item->valuestring;
        const char *raw_name = (strncmp(existing, "custom:", 7) == 0) ? existing + 7 : existing;
        char hex[8];
        color_name_to_hex(raw_name, custom_colors, hex);
        cJSON_AddStringToObject(ctx, "filter_color_selected", raw_name);
        cJSON_AddStringToObject(ctx, "filter_color_display_name", raw_name);
        cJSON_AddStringToObject(ctx, "filter_color_hex", hex);
        cJSON_AddBoolToObject(ctx, "filter_color_is_named", true);
        cJSON_AddBoolToObject(ctx, "filter_color_is_picker", false);
    } else {
        cJSON_AddStringToObject(ctx, "filter_color_hex", "#FF0000");
        cJSON_AddStringToObject(ctx, "filter_color_selected", "");
        cJSON_AddStringToObject(ctx, "filter_color_display_name", "");
        cJSON_AddBoolToObject(ctx, "filter_color_is_named", false);
        cJSON_AddBoolToObject(ctx, "filter_color_is_picker", false);
    }
}

static cJSON *render_filters_list_ctx(cJSON *filters_dict)
{
    cJSON *ctx = build_global_context();
    cJSON_AddItemToObject(ctx, "filters", build_filters_list(filters_dict, NULL));
    cJSON_AddStringToObject(ctx, "page_title", "Filters");
    return ctx;
}

/* ---------------------------------------------------------------------
 * Views
 * ------------------------------------------------------------------- */

http_response_t *view_filters(http_request_t *req)
{
    (void)req;
    cJSON *model = lighting_get_settings();
    cJSON *filters_dict = model ? cJSON_GetObjectItem(model, "filters") : NULL;
    return webserver_render_response("setup/filters.html", render_filters_list_ctx(filters_dict));
}

/* POST /filters — create_filter / delete_filter, mirrors FiltersView.post(). */
http_response_t *view_filters_post(http_request_t *req)
{
    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    cJSON *model = lighting_get_settings();
    if (!model) return response_create(500, "text/plain", "Error");

    cJSON *filters_dict = cJSON_GetObjectItem(model, "filters");
    if (!filters_dict) { filters_dict = cJSON_CreateObject(); cJSON_AddItemToObject(model, "filters", filters_dict); }

    const char *action = request_get_form_field(req, "action");
    const char *filter_name = request_get_form_field(req, "filter_name");

    if (action && strcmp(action, "create_filter") == 0 && filter_name && filter_name[0]) {
        if (!cJSON_GetObjectItem(filters_dict, filter_name)) {
            cJSON *def = cJSON_CreateObject();
            cJSON_AddStringToObject(def, "filter", "sizzle");
            cJSON_AddItemToObject(filters_dict, filter_name, def);
        }
        persistent_dict_mark_dirty(store); persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        fill_filter_edit_context(ctx, model, filter_name);
        return webserver_render_response("setup/filter_edit.html", ctx);
    }

    if (action && strcmp(action, "delete_filter") == 0 && filter_name && filter_name[0] &&
        cJSON_GetObjectItem(filters_dict, filter_name)) {
        cJSON_DeleteItemFromObject(filters_dict, filter_name);
        rename_filter_refs(model, filter_name, NULL);
        persistent_dict_mark_dirty(store); persistent_dict_save(store);
    }

    return webserver_render_response("setup/filters.html", render_filters_list_ctx(filters_dict));
}

/* GET/POST /filters/edit — mirrors FilterEditView.get()/post(). */
http_response_t *view_filter_edit(http_request_t *req)
{
    cJSON *model = lighting_get_settings();
    if (!model) return response_create(500, "text/plain", "Error");

    cJSON *filters_dict = cJSON_GetObjectItem(model, "filters");

    if (req->method == HTTP_METHOD_GET) {
        const char *filter_name = request_get_query_param(req, "filter");
        if (!filter_name || !filter_name[0] || !filters_dict || !cJSON_GetObjectItem(filters_dict, filter_name)) {
            return response_create(200, "text/html", "<p class=\"text-danger small\">Filter not found.</p>");
        }
        cJSON *ctx = build_global_context();
        fill_filter_edit_context(ctx, model, filter_name);
        return webserver_render_response("setup/filter_edit.html", ctx);
    }

    /* POST */
    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    if (!filters_dict) { filters_dict = cJSON_CreateObject(); cJSON_AddItemToObject(model, "filters", filters_dict); }

    const char *action = request_get_form_field(req, "action");
    const char *filter_name = request_get_form_field(req, "filter_name");
    const char *filter_type = request_get_form_field(req, "filter_type");

    if (action && strcmp(action, "update_filter") == 0 && filter_name && filter_name[0] && filter_type && filter_type[0]) {
        const char *old_filter_name = request_get_form_field(req, "old_filter_name");

        cJSON *def = cJSON_CreateObject();
        cJSON_AddStringToObject(def, "filter", filter_type);

        const filter_meta_entry_t *meta = find_filter_meta(filter_type);
        if (meta) {
            for (int i = 0; meta->optional[i] != NULL; i++) {
                const char *param_name = meta->optional[i];
                char field[64];
                snprintf(field, sizeof(field), "filter_param_%s", param_name);
                const char *raw_value = request_get_form_field_last(req, field);
                if (!raw_value) continue;

                char trimmed[128];
                snprintf(trimmed, sizeof(trimmed), "%s", raw_value);
                char *start = trimmed;
                while (*start == ' ') start++;
                size_t tlen = strlen(start);
                while (tlen > 0 && start[tlen - 1] == ' ') start[--tlen] = '\0';
                if (tlen == 0) continue;

                if (strcmp(param_name, "color") == 0 && start[0] == '#' && tlen == 7 && is_hex6(start + 1)) {
                    unsigned int r, g, b;
                    sscanf(start + 1, "%2x%2x%2x", &r, &g, &b);
                    cJSON *arr = cJSON_CreateArray();
                    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)r));
                    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)g));
                    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)b));
                    cJSON_AddItemToObject(def, param_name, arr);
                } else if (strcmp(start, "true") == 0 || strcmp(start, "True") == 0) {
                    cJSON_AddBoolToObject(def, param_name, true);
                } else if (strcmp(start, "false") == 0 || strcmp(start, "False") == 0) {
                    cJSON_AddBoolToObject(def, param_name, false);
                } else {
                    bool has_dot = strchr(start, '.') != NULL;
                    char *endptr = NULL;
                    if (has_dot) {
                        double d = strtod(start, &endptr);
                        if (endptr != start && *endptr == '\0') cJSON_AddNumberToObject(def, param_name, d);
                        else cJSON_AddStringToObject(def, param_name, start);
                    } else {
                        long v = strtol(start, &endptr, 10);
                        if (endptr != start && *endptr == '\0') cJSON_AddNumberToObject(def, param_name, (double)v);
                        else cJSON_AddStringToObject(def, param_name, start);
                    }
                }
            }
        }

        if (strcmp(filter_type, "sizzle") == 0 || strcmp(filter_type, "scintillate") == 0) {
            const char *vp_raw = request_get_form_field_last(req, "filter_param_variation_percent");
            double variation_percent = 20.0;
            if (vp_raw && vp_raw[0]) {
                char *endptr = NULL;
                double d = strtod(vp_raw, &endptr);
                if (endptr != vp_raw) variation_percent = d;
            }
            if (variation_percent < 0.0) variation_percent = 0.0;
            if (variation_percent > 100.0) variation_percent = 100.0;
            variation_percent = ((double)(long long)(variation_percent * 100.0 + 0.5)) / 100.0;
            cJSON_AddNumberToObject(def, "variation_percent", variation_percent);
        }

        if (old_filter_name && old_filter_name[0] && strcmp(old_filter_name, filter_name) != 0 &&
            cJSON_GetObjectItem(filters_dict, old_filter_name)) {
            cJSON_DeleteItemFromObject(filters_dict, old_filter_name);
            rename_filter_refs(model, old_filter_name, filter_name);
        }

        cJSON_DeleteItemFromObject(filters_dict, filter_name);
        cJSON_AddItemToObject(filters_dict, filter_name, def);
        persistent_dict_mark_dirty(store); persistent_dict_save(store);

    } else if (action && strcmp(action, "delete_filter") == 0 && filter_name && filter_name[0]) {
        if (cJSON_GetObjectItem(filters_dict, filter_name)) {
            cJSON_DeleteItemFromObject(filters_dict, filter_name);
            persistent_dict_mark_dirty(store); persistent_dict_save(store);
        }
        return webserver_render_response("setup/filters.html", render_filters_list_ctx(filters_dict));
    }

    return webserver_render_response("setup/filters.html", render_filters_list_ctx(filters_dict));
}

/* POST /filters/color_select — mirrors ColorSelectView.post(). Generic
 * over field_name/color_index, ported here for the filter editor's use. */
http_response_t *view_filter_color_select(http_request_t *req)
{
    char field_name_buf[64];
    const char *field_name = request_get_form_field(req, "field_name");
    const char *color_index = request_get_form_field(req, "color_index");
    if (!color_index || !color_index[0]) color_index = "0";
    if (!field_name || !field_name[0]) {
        snprintf(field_name_buf, sizeof(field_name_buf), "param_color_%s", color_index);
        field_name = field_name_buf;
    }

    const char *color_value = request_get_form_field_last(req, field_name);
    if (!color_value) color_value = "";

    cJSON *model = lighting_get_settings();
    cJSON *custom_colors = model ? cJSON_GetObjectItem(model, "custom_colors") : NULL;

    bool is_named = color_value[0] != '\0' && strcmp(color_value, "__picker__") != 0 && color_value[0] != '#';
    bool is_picker = strcmp(color_value, "__picker__") == 0 || color_value[0] == '#';

    char stored_name[80] = "";
    char display_name[80] = "";
    char hex_val[8];
    snprintf(hex_val, sizeof(hex_val), "#FF0000");

    if (is_named) {
        bool is_custom = strncmp(color_value, "custom:", 7) == 0;
        const char *raw_name = is_custom ? color_value + 7 : color_value;
        snprintf(stored_name, sizeof(stored_name), "%s", color_value);
        snprintf(display_name, sizeof(display_name), "%s", raw_name);
        color_name_to_hex(raw_name, custom_colors, hex_val);
    } else if (color_value[0] == '#') {
        snprintf(hex_val, sizeof(hex_val), "%s", color_value);
    }

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "color_index", color_index);
    cJSON_AddStringToObject(ctx, "field_name", field_name);
    cJSON_AddStringToObject(ctx, "color_name", stored_name);
    cJSON_AddStringToObject(ctx, "color_display_name", display_name);
    cJSON_AddStringToObject(ctx, "color_hex_val", hex_val);
    cJSON_AddBoolToObject(ctx, "is_named", is_named);
    cJSON_AddBoolToObject(ctx, "is_picker", is_picker);

    return webserver_render_response("setup/color_select.html", ctx);
}

void add_filters_summary_context(cJSON *ctx)
{
    cJSON *model = lighting_get_settings();
    cJSON *filters_dict = model ? cJSON_GetObjectItem(model, "filters") : NULL;

    cJSON *names = cJSON_CreateArray();
    int total = filters_dict ? cJSON_GetArraySize(filters_dict) : 0;
    if (total > 0) {
        const char **name_arr = calloc(total, sizeof(char *));
        if (name_arr) {
            int i = 0;
            for (cJSON *item = filters_dict->child; item; item = item->next) name_arr[i++] = item->string;
            qsort(name_arr, total, sizeof(char *), cmp_str);
            for (int j = 0; j < total; j++) cJSON_AddItemToArray(names, cJSON_CreateString(name_arr[j]));
            free(name_arr);
        }
    }
    cJSON_AddItemToObject(ctx, "filter_names", names);
}

/* GET /filters/summary — mirrors FiltersSummaryView.get(). */
http_response_t *view_filters_summary(http_request_t *req)
{
    (void)req;
    cJSON *ctx = build_global_context();
    add_filters_summary_context(ctx);
    return webserver_render_response("setup/filters_summary.html", ctx);
}
