/*
 * Effect management views: list/create/delete effects, edit an individual
 * effect's pattern/colors/optional params and its ordered filter chain.
 *
 * Mirrors web/views.py: EffectsView, EffectEditView, _effect_edit_context(),
 * _pattern_params_context(), _parse_effect_from_form(), plus the
 * module-level helpers _effects_list(), _filters_list() (as used here with
 * no filter_order), and _rename_effect_refs(). ColorSelectView is shared
 * with scenes — view_effects_color_select() just delegates to
 * view_scenes_color_select() in views_scenes.c.
 */
#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "lighting.h"
#include "metadata.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ---------------------------------------------------------------------
 * Standard color table + hex helpers — mirrors _STANDARD_COLORS and
 * _color_name_to_hex() in web/views.py.
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

/* True if the first `len` chars of s are all hex digits. */
static bool is_hex_digits(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

/* Sorted keys of a cJSON object as a cJSON string array. */
static cJSON *sorted_keys_array(cJSON *obj)
{
    cJSON *result = cJSON_CreateArray();
    if (!obj) return result;
    int total = cJSON_GetArraySize(obj);
    if (total <= 0) return result;
    const char **names = calloc((size_t)total, sizeof(char *));
    if (!names) return result;
    int idx = 0;
    for (cJSON *item = obj->child; item; item = item->next) names[idx++] = item->string;
    qsort(names, (size_t)total, sizeof(char *), cmp_str);
    for (int i = 0; i < total; i++) cJSON_AddItemToArray(result, cJSON_CreateString(names[i]));
    free(names);
    return result;
}

static bool form_field_exists(http_request_t *req, const char *field_name)
{
    return req && req->form_data && cJSON_GetObjectItem(req->form_data, field_name) != NULL;
}

/* ---------------------------------------------------------------------
 * List building — mirrors _effects_list() and _filters_list() (no
 * filter_order — that ordering is only used by the filter editor itself).
 * ------------------------------------------------------------------- */

static cJSON *build_effects_list(cJSON *effects_dict)
{
    cJSON *result = cJSON_CreateArray();
    if (!effects_dict) return result;
    int total = cJSON_GetArraySize(effects_dict);
    if (total <= 0) return result;
    const char **names = calloc((size_t)total, sizeof(char *));
    if (!names) return result;
    int idx = 0;
    for (cJSON *item = effects_dict->child; item; item = item->next) names[idx++] = item->string;
    qsort(names, (size_t)total, sizeof(char *), cmp_str);
    for (int i = 0; i < total; i++) {
        cJSON *def = cJSON_GetObjectItem(effects_dict, names[i]);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", names[i]);
        cJSON_AddStringToObject(entry, "pattern", json_get_string(def, "pattern", ""));
        cJSON_AddItemToArray(result, entry);
    }
    free(names);
    return result;
}

static cJSON *build_filters_list_plain(cJSON *filters_dict)
{
    cJSON *result = cJSON_CreateArray();
    if (!filters_dict) return result;
    int total = cJSON_GetArraySize(filters_dict);
    if (total <= 0) return result;
    const char **names = calloc((size_t)total, sizeof(char *));
    if (!names) return result;
    int idx = 0;
    for (cJSON *item = filters_dict->child; item; item = item->next) names[idx++] = item->string;
    qsort(names, (size_t)total, sizeof(char *), cmp_str);
    for (int i = 0; i < total; i++) {
        cJSON *def = cJSON_GetObjectItem(filters_dict, names[i]);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", names[i]);
        cJSON_AddStringToObject(entry, "filter_type", json_get_string(def, "filter", ""));
        cJSON_AddItemToArray(result, entry);
    }
    free(names);
    return result;
}

static cJSON *build_pattern_metadata_json(void)
{
    cJSON *root = cJSON_CreateObject();
    for (int i = 0; pattern_metadata_table[i].name != NULL; i++) {
        const pattern_metadata_t *p = &pattern_metadata_table[i];
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "description", p->description);
        cJSON_AddNumberToObject(entry, "color_count", p->color_count);
        cJSON *optional = cJSON_CreateArray();
        for (int j = 0; j < p->optional_count; j++) {
            cJSON_AddItemToArray(optional, cJSON_CreateString(p->optional[j]));
        }
        cJSON_AddItemToObject(entry, "optional", optional);
        cJSON_AddItemToObject(root, p->name, entry);
    }
    return root;
}

/* Updates all scene entries that reference an effect by its old name —
 * mirrors _rename_effect_refs(). */
static void rename_effect_refs(cJSON *model, const char *old_name, const char *new_name)
{
    cJSON *scenes_dict = model ? cJSON_GetObjectItem(model, "scenes") : NULL;
    if (!scenes_dict) return;
    for (cJSON *scene = scenes_dict->child; scene; scene = scene->next) {
        if (!cJSON_IsObject(scene)) continue;
        for (cJSON *entry = scene->child; entry; entry = entry->next) {
            if (!cJSON_IsObject(entry)) continue;
            cJSON *effect_item = cJSON_GetObjectItem(entry, "effect");
            if (effect_item && cJSON_IsString(effect_item) && strcmp(effect_item->valuestring, old_name) == 0) {
                cJSON_ReplaceItemInObject(entry, "effect", cJSON_CreateString(new_name));
            }
        }
    }
}

/* ---------------------------------------------------------------------
 * Pattern params context — mirrors _pattern_params_context(). Every
 * current call site passes show_target=false (scene editing uses a
 * dedicated ordered filter manager instead of the checkbox list this
 * function would otherwise emit for show_target=true), so the
 * named-filter-checkbox branch is intentionally not implemented here —
 * it is dead code in the Python source too.
 * ------------------------------------------------------------------- */

static void fill_pattern_params_context(cJSON *ctx, cJSON *model, const char *pattern,
                                         cJSON *existing_effect /* nullable */, bool show_target)
{
    const pattern_metadata_t *pattern_info = metadata_get_pattern(pattern);
    if (!pattern_info) return;

    cJSON *custom_colors = model ? cJSON_GetObjectItem(model, "custom_colors") : NULL;
    cJSON *existing_colors = existing_effect ? cJSON_GetObjectItem(existing_effect, "colors") : NULL;
    int existing_colors_count =
        (existing_colors && cJSON_IsArray(existing_colors)) ? cJSON_GetArraySize(existing_colors) : 0;

    int color_count = pattern_info->color_count;
    cJSON *color_hex = cJSON_CreateArray();
    cJSON *color_names = cJSON_CreateArray();
    cJSON *color_display_names = cJSON_CreateArray();
    cJSON *color_selected = cJSON_CreateArray();
    cJSON *color_is_named = cJSON_CreateArray();
    cJSON *color_is_picker = cJSON_CreateArray();

    for (int i = 0; i < color_count; i++) {
        cJSON *existing = (i < existing_colors_count) ? cJSON_GetArrayItem(existing_colors, i) : NULL;

        if (existing && cJSON_IsString(existing)) {
            const char *ev = existing->valuestring;
            bool is_custom = strncmp(ev, "custom:", 7) == 0;
            const char *raw_name = is_custom ? ev + 7 : ev;
            char hex_val[8];
            color_name_to_hex(raw_name, custom_colors, hex_val);

            cJSON_AddItemToArray(color_hex, cJSON_CreateString(hex_val));
            cJSON_AddItemToArray(color_names, cJSON_CreateString(ev));
            cJSON_AddItemToArray(color_display_names, cJSON_CreateString(raw_name));
            cJSON_AddItemToArray(color_selected, cJSON_CreateString(raw_name));
            cJSON_AddItemToArray(color_is_named, cJSON_CreateBool(true));
            cJSON_AddItemToArray(color_is_picker, cJSON_CreateBool(false));
        } else if (existing && cJSON_IsArray(existing) && cJSON_GetArraySize(existing) == 3) {
            cJSON *ri = cJSON_GetArrayItem(existing, 0);
            cJSON *gi = cJSON_GetArrayItem(existing, 1);
            cJSON *bi = cJSON_GetArrayItem(existing, 2);
            char hex_val[8];
            hex_from_rgb((uint8_t)ri->valueint, (uint8_t)gi->valueint, (uint8_t)bi->valueint, hex_val);

            cJSON_AddItemToArray(color_hex, cJSON_CreateString(hex_val));
            cJSON_AddItemToArray(color_names, cJSON_CreateString(""));
            cJSON_AddItemToArray(color_display_names, cJSON_CreateString(""));
            cJSON_AddItemToArray(color_selected, cJSON_CreateString("__picker__"));
            cJSON_AddItemToArray(color_is_named, cJSON_CreateBool(false));
            cJSON_AddItemToArray(color_is_picker, cJSON_CreateBool(true));
        } else {
            cJSON_AddItemToArray(color_hex, cJSON_CreateString("#FF0000"));
            cJSON_AddItemToArray(color_names, cJSON_CreateString(""));
            cJSON_AddItemToArray(color_display_names, cJSON_CreateString(""));
            cJSON_AddItemToArray(color_selected, cJSON_CreateString(""));
            cJSON_AddItemToArray(color_is_named, cJSON_CreateBool(false));
            cJSON_AddItemToArray(color_is_picker, cJSON_CreateBool(false));
        }
    }

    cJSON_AddStringToObject(ctx, "pattern", pattern);

    cJSON *pattern_info_json = cJSON_CreateObject();
    cJSON_AddStringToObject(pattern_info_json, "description", pattern_info->description);
    cJSON_AddNumberToObject(pattern_info_json, "color_count", pattern_info->color_count);
    cJSON *optional_arr = cJSON_CreateArray();
    for (int i = 0; i < pattern_info->optional_count; i++) {
        cJSON_AddItemToArray(optional_arr, cJSON_CreateString(pattern_info->optional[i]));
    }
    cJSON_AddItemToObject(pattern_info_json, "optional", optional_arr);
    cJSON_AddItemToObject(ctx, "pattern_info", pattern_info_json);

    cJSON_AddNumberToObject(ctx, "color_count", color_count);
    cJSON_AddItemToObject(ctx, "color_hex", color_hex);
    cJSON_AddItemToObject(ctx, "color_names", color_names);
    cJSON_AddItemToObject(ctx, "color_display_names", color_display_names);
    cJSON_AddItemToObject(ctx, "color_selected", color_selected);
    cJSON_AddItemToObject(ctx, "color_is_named", color_is_named);
    cJSON_AddItemToObject(ctx, "color_is_picker", color_is_picker);

    if (model) {
        cJSON *named_ranges = cJSON_GetObjectItem(model, "named_ranges");
        if (named_ranges) cJSON_AddItemReferenceToObject(ctx, "named_ranges", named_ranges);
    }
    if (custom_colors) cJSON_AddItemReferenceToObject(ctx, "custom_colors", custom_colors);

    cJSON_AddBoolToObject(ctx, "has_optional", pattern_info->optional_count > 0);
    cJSON_AddBoolToObject(ctx, "show_target", show_target);
    cJSON_AddStringToObject(ctx, "color_select_url", show_target ? "/scenes/color_select" : "/effects/color_select");

    if (existing_effect) {
        for (int i = 0; i < pattern_info->optional_count; i++) {
            const char *param_name = pattern_info->optional[i];
            cJSON *val = cJSON_GetObjectItem(existing_effect, param_name);
            if (!val) continue;
            char key[80];
            snprintf(key, sizeof(key), "param_val_%s", param_name);
            if (cJSON_IsBool(val)) {
                cJSON_AddStringToObject(ctx, key, cJSON_IsTrue(val) ? "true" : "False");
            } else if (cJSON_IsNumber(val)) {
                char buf[32];
                double v = val->valuedouble;
                if (v == (double)(long long)v) snprintf(buf, sizeof(buf), "%lld", (long long)v);
                else snprintf(buf, sizeof(buf), "%g", v);
                cJSON_AddStringToObject(ctx, key, buf);
            } else if (cJSON_IsString(val)) {
                cJSON_AddStringToObject(ctx, key, val->valuestring);
            }
        }
        cJSON *target_val = cJSON_GetObjectItem(existing_effect, "target");
        if (target_val && cJSON_IsString(target_val)) {
            cJSON_AddStringToObject(ctx, "param_val_target", target_val->valuestring);
        }
    }
}

/* Appends one parsed color (hex "#RRGGBB" -> [r,g,b] triple, otherwise a
 * name string, matching _parse_effect_from_form()'s per-color handling). */
static void parse_color_value(const char *color_value, cJSON *colors_list)
{
    size_t len = strlen(color_value);
    if (len == 0) return;

    bool looks_hex = (color_value[0] == '#') || (len == 6 && is_hex_digits(color_value, 6));
    if (looks_hex) {
        const char *hexpart = (color_value[0] == '#') ? color_value + 1 : color_value;
        if (strlen(hexpart) >= 6 && is_hex_digits(hexpart, 6)) {
            unsigned int r, g, b;
            sscanf(hexpart, "%2x%2x%2x", &r, &g, &b);
            cJSON *tuple = cJSON_CreateArray();
            cJSON_AddItemToArray(tuple, cJSON_CreateNumber((double)r));
            cJSON_AddItemToArray(tuple, cJSON_CreateNumber((double)g));
            cJSON_AddItemToArray(tuple, cJSON_CreateNumber((double)b));
            cJSON_AddItemToArray(colors_list, tuple);
            return;
        }
    }
    cJSON_AddItemToArray(colors_list, cJSON_CreateString(color_value));
}

/* Builds an effect dict from posted form data — mirrors
 * _parse_effect_from_form(). Handles indexed param_color_N fields, typed
 * param_* fields (bool/int/float/string), and effect_filter_<name>
 * checkboxes. */
static cJSON *parse_effect_from_form(http_request_t *req, cJSON *model, const char *pattern)
{
    cJSON *job = cJSON_CreateObject();
    cJSON_AddStringToObject(job, "pattern", pattern);

    cJSON *colors_list = cJSON_CreateArray();
    int color_index = 0;
    while (1) {
        char color_key[32];
        snprintf(color_key, sizeof(color_key), "param_color_%d", color_index);
        if (!form_field_exists(req, color_key)) break;

        const char *raw = request_get_form_field_last(req, color_key);
        char trimmed[64] = "";
        if (raw) snprintf(trimmed, sizeof(trimmed), "%s", raw);
        char *start = trimmed;
        while (*start == ' ') start++;
        size_t tlen = strlen(start);
        while (tlen > 0 && start[tlen - 1] == ' ') start[--tlen] = '\0';

        parse_color_value(start, colors_list);
        color_index++;
    }
    if (cJSON_GetArraySize(colors_list) > 0) {
        cJSON_AddItemToObject(job, "colors", colors_list);
    } else {
        cJSON_Delete(colors_list);
    }

    if (req->form_data) {
        for (cJSON *field = req->form_data->child; field; field = field->next) {
            const char *key = field->string;
            if (!key || strncmp(key, "param_", 6) != 0) continue;
            if (strncmp(key, "param_color", 11) == 0) continue;
            const char *param_name = key + 6;

            const char *raw = request_get_form_field_last(req, key);
            if (!raw) continue;
            char trimmed[128];
            snprintf(trimmed, sizeof(trimmed), "%s", raw);
            char *start = trimmed;
            while (*start == ' ') start++;
            size_t tlen = strlen(start);
            while (tlen > 0 && start[tlen - 1] == ' ') start[--tlen] = '\0';
            if (tlen == 0) continue;

            char lower[16];
            size_t n = tlen < sizeof(lower) - 1 ? tlen : sizeof(lower) - 1;
            for (size_t i = 0; i < n; i++) lower[i] = (char)tolower((unsigned char)start[i]);
            lower[n] = '\0';

            if (n == 4 && strcmp(lower, "true") == 0) {
                cJSON_AddBoolToObject(job, param_name, true);
            } else {
                bool has_dot = strchr(start, '.') != NULL;
                char *endptr = NULL;
                if (has_dot) {
                    double d = strtod(start, &endptr);
                    if (endptr != start && *endptr == '\0') cJSON_AddNumberToObject(job, param_name, d);
                    else cJSON_AddStringToObject(job, param_name, start);
                } else {
                    long v = strtol(start, &endptr, 10);
                    if (endptr != start && *endptr == '\0') cJSON_AddNumberToObject(job, param_name, (double)v);
                    else cJSON_AddStringToObject(job, param_name, start);
                }
            }
        }
    }

    cJSON *filters_dict = model ? cJSON_GetObjectItem(model, "filters") : NULL;
    cJSON *sorted_filter_names = sorted_keys_array(filters_dict);
    cJSON *filters_out = cJSON_CreateArray();
    int total = cJSON_GetArraySize(sorted_filter_names);
    for (int i = 0; i < total; i++) {
        cJSON *name_item = cJSON_GetArrayItem(sorted_filter_names, i);
        if (!name_item || !cJSON_IsString(name_item)) continue;
        char field[80];
        snprintf(field, sizeof(field), "effect_filter_%s", name_item->valuestring);
        const char *val = request_get_form_field(req, field);
        if (val && strcmp(val, "1") == 0) {
            cJSON_AddItemToArray(filters_out, cJSON_CreateString(name_item->valuestring));
        }
    }
    cJSON_Delete(sorted_filter_names);
    if (cJSON_GetArraySize(filters_out) > 0) {
        cJSON_AddItemToObject(job, "filters", filters_out);
    } else {
        cJSON_Delete(filters_out);
    }

    return job;
}

/* ---------------------------------------------------------------------
 * Edit-context building — mirrors _effect_edit_context().
 * ------------------------------------------------------------------- */

static void fill_effect_edit_context(cJSON *ctx, cJSON *model, const char *effect_name)
{
    cJSON *effects_dict = model ? cJSON_GetObjectItem(model, "effects") : NULL;
    cJSON *filters_dict = model ? cJSON_GetObjectItem(model, "filters") : NULL;

    cJSON_AddItemToObject(ctx, "effects", build_effects_list(effects_dict));
    cJSON_AddItemToObject(ctx, "pattern_metadata", build_pattern_metadata_json());
    cJSON_AddItemToObject(ctx, "all_filters", build_filters_list_plain(filters_dict));
    cJSON_AddStringToObject(ctx, "page_title", "Effects");

    cJSON *effect_dict = (effects_dict && effect_name) ? cJSON_GetObjectItem(effects_dict, effect_name) : NULL;
    if (!effect_dict) return;

    const char *pattern = json_get_string(effect_dict, "pattern", "");
    cJSON_AddStringToObject(ctx, "edit_effect_name", effect_name);
    cJSON_AddStringToObject(ctx, "edit_effect_pattern", pattern);

    cJSON *cycles_item = cJSON_GetObjectItem(effect_dict, "cycles");
    char cycles_buf[32] = "";
    if (cycles_item && cJSON_IsNumber(cycles_item)) {
        snprintf(cycles_buf, sizeof(cycles_buf), "%d", (int)cycles_item->valuedouble);
    }
    cJSON_AddStringToObject(ctx, "edit_effect_cycles", cycles_buf);
    cJSON_AddStringToObject(ctx, "old_effect_name", effect_name);

    cJSON *effect_filters = cJSON_GetObjectItem(effect_dict, "filters");
    cJSON *effect_filter_names_arr = cJSON_CreateArray();
    cJSON *effect_filters_display_arr = cJSON_CreateArray();
    if (effect_filters && cJSON_IsArray(effect_filters)) {
        int n = cJSON_GetArraySize(effect_filters);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(effect_filters, i);
            if (item && cJSON_IsString(item)) {
                cJSON_AddItemToArray(effect_filter_names_arr, cJSON_CreateString(item->valuestring));
            }
        }
        /* Top-to-bottom display is reverse execution order (top runs last). */
        for (int i = n - 1; i >= 0; i--) {
            cJSON *item = cJSON_GetArrayItem(effect_filters, i);
            if (!item || !cJSON_IsString(item)) continue;
            cJSON *fdef = filters_dict ? cJSON_GetObjectItem(filters_dict, item->valuestring) : NULL;
            if (!fdef) continue;
            cJSON *disp = cJSON_CreateObject();
            cJSON_AddStringToObject(disp, "name", item->valuestring);
            cJSON_AddStringToObject(disp, "filter_type", json_get_string(fdef, "filter", ""));
            cJSON_AddItemToArray(effect_filters_display_arr, disp);
        }
    }
    cJSON_AddItemToObject(ctx, "effect_filters", effect_filters_display_arr);
    cJSON_AddItemToObject(ctx, "effect_filter_names", effect_filter_names_arr);

    if (pattern[0] != '\0' && metadata_get_pattern(pattern)) {
        fill_pattern_params_context(ctx, model, pattern, effect_dict, false);
    }
}

/* Swaps two adjacent string entries within a cJSON array in place. */
static void swap_array_strings(cJSON *arr, int a, int b)
{
    cJSON *item_a = cJSON_GetArrayItem(arr, a);
    cJSON *item_b = cJSON_GetArrayItem(arr, b);
    if (!item_a || !item_b) return;
    cJSON *dup_a = cJSON_CreateString(item_a->valuestring);
    cJSON *dup_b = cJSON_CreateString(item_b->valuestring);
    cJSON_ReplaceItemInArray(arr, a, dup_b);
    cJSON_ReplaceItemInArray(arr, b, dup_a);
}

/* ---------------------------------------------------------------------
 * Views
 * ------------------------------------------------------------------- */

/* GET/POST /effects — mirrors EffectsView.get()/post() (list, create_effect,
 * delete_effect). */
http_response_t *view_effects(http_request_t *req)
{
    cJSON *model = lighting_get_settings();
    cJSON *effects_dict = model ? cJSON_GetObjectItem(model, "effects") : NULL;

    if (req->method == HTTP_METHOD_POST && model) {
        if (!effects_dict) { effects_dict = cJSON_CreateObject(); cJSON_AddItemToObject(model, "effects", effects_dict); }
        persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);

        const char *action = request_get_form_field(req, "action");
        const char *effect_name = request_get_form_field(req, "effect_name");

        if (action && strcmp(action, "create_effect") == 0 && effect_name && effect_name[0]) {
            if (!cJSON_GetObjectItem(effects_dict, effect_name)) {
                cJSON *def = cJSON_CreateObject();
                cJSON_AddStringToObject(def, "pattern", "solid");
                cJSON_AddItemToObject(effects_dict, effect_name, def);
            }
            persistent_dict_save(store);
            /* Go straight to editing the new effect, matching Python. */
            cJSON *ctx = build_global_context();
            fill_effect_edit_context(ctx, model, effect_name);
            return webserver_render_response("setup/effect_edit.html", ctx);

        } else if (action && strcmp(action, "delete_effect") == 0 && effect_name && effect_name[0] &&
                   cJSON_GetObjectItem(effects_dict, effect_name)) {
            cJSON_DeleteItemFromObject(effects_dict, effect_name);
            persistent_dict_save(store);
        }
    }

    cJSON *ctx = build_global_context();
    cJSON_AddItemToObject(ctx, "effects", build_effects_list(effects_dict));
    cJSON_AddStringToObject(ctx, "page_title", "Effects");
    return webserver_render_response("setup/effects.html", ctx);
}

/* GET/POST /effects/edit — mirrors EffectEditView.get()/post(). */
http_response_t *view_effect_edit(http_request_t *req)
{
    cJSON *model = lighting_get_settings();
    cJSON *effects_dict = model ? cJSON_GetObjectItem(model, "effects") : NULL;

    if (req->method == HTTP_METHOD_GET) {
        const char *effect_name = request_get_query_param(req, "effect");
        if (!effect_name || !effect_name[0] || !effects_dict || !cJSON_GetObjectItem(effects_dict, effect_name)) {
            return response_create(200, "text/html", "<p class=\"text-danger small\">Effect not found.</p>");
        }
        cJSON *ctx = build_global_context();
        fill_effect_edit_context(ctx, model, effect_name);
        return webserver_render_response("setup/effect_edit.html", ctx);
    }

    /* POST */
    const char *action = request_get_form_field(req, "action");
    const char *effect_name = request_get_form_field(req, "effect_name");
    const char *pattern = request_get_form_field(req, "pattern");

    /* Pattern selected — return the params fragment only (no target, no persistence). */
    if ((!action || !action[0]) && pattern && pattern[0]) {
        if (metadata_get_pattern(pattern)) {
            cJSON *ctx = build_global_context();
            fill_pattern_params_context(ctx, model, pattern, NULL, false);
            return webserver_render_response("setup/pattern_params.html", ctx);
        }
        return response_create(200, "text/html", "<p class=\"text-danger\">Invalid pattern.</p>");
    }

    if (model && !effects_dict) {
        effects_dict = cJSON_CreateObject();
        cJSON_AddItemToObject(model, "effects", effects_dict);
    }

    /* Effect filter manager actions. */
    if (action && (strcmp(action, "add_effect_filter") == 0 || strcmp(action, "remove_effect_filter") == 0 ||
                   strcmp(action, "move_effect_filter_up") == 0 || strcmp(action, "move_effect_filter_down") == 0)) {
        if (!effect_name || !effect_name[0] || !effects_dict || !cJSON_GetObjectItem(effects_dict, effect_name)) {
            return response_create(200, "text/html", "<p class=\"text-danger small\">Effect not found.</p>");
        }
        cJSON *effect_dict = cJSON_GetObjectItem(effects_dict, effect_name);
        cJSON *filters_list = cJSON_GetObjectItem(effect_dict, "filters");
        if (!filters_list || !cJSON_IsArray(filters_list)) {
            filters_list = cJSON_CreateArray();
            cJSON_DeleteItemFromObject(effect_dict, "filters");
            cJSON_AddItemToObject(effect_dict, "filters", filters_list);
        }

        const char *filter_name = request_get_form_field(req, "filter_name");
        persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
        int n = cJSON_GetArraySize(filters_list);
        int found_idx = -1;
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(filters_list, i);
            if (item && cJSON_IsString(item) && filter_name && strcmp(item->valuestring, filter_name) == 0) {
                found_idx = i;
                break;
            }
        }

        if (strcmp(action, "add_effect_filter") == 0 && filter_name && filter_name[0] && found_idx < 0) {
            cJSON_AddItemToArray(filters_list, cJSON_CreateString(filter_name));
            persistent_dict_save(store);
        } else if (strcmp(action, "remove_effect_filter") == 0 && found_idx >= 0) {
            cJSON_DeleteItemFromArray(filters_list, found_idx);
            persistent_dict_save(store);
        } else if (strcmp(action, "move_effect_filter_up") == 0 && found_idx > 0) {
            swap_array_strings(filters_list, found_idx - 1, found_idx);
            persistent_dict_save(store);
        } else if (strcmp(action, "move_effect_filter_down") == 0 && found_idx >= 0 && found_idx < n - 1) {
            swap_array_strings(filters_list, found_idx, found_idx + 1);
            persistent_dict_save(store);
        }

        cJSON *ctx = build_global_context();
        fill_effect_edit_context(ctx, model, effect_name);
        return webserver_render_response("setup/effect_filters_manager.html", ctx);
    }

    if (action && strcmp(action, "update_effect") == 0 && effect_name && effect_name[0] && pattern && pattern[0] &&
        effects_dict) {
        const char *old_effect_name = request_get_form_field(req, "old_effect_name");

        cJSON *effect_dict = parse_effect_from_form(req, model, pattern);
        cJSON_DeleteItemFromObject(effect_dict, "target"); /* target lives on the scene entry, not the effect */

        const char *source_name = (old_effect_name && old_effect_name[0]) ? old_effect_name : effect_name;
        cJSON *existing_effect = cJSON_GetObjectItem(effects_dict, source_name);
        cJSON *existing_filters = existing_effect ? cJSON_GetObjectItem(existing_effect, "filters") : NULL;
        if (existing_filters) {
            cJSON_DeleteItemFromObject(effect_dict, "filters");
            cJSON_AddItemToObject(effect_dict, "filters", cJSON_Duplicate(existing_filters, 1));
        }

        persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
        if (old_effect_name && old_effect_name[0] && strcmp(old_effect_name, effect_name) != 0 &&
            cJSON_GetObjectItem(effects_dict, old_effect_name)) {
            cJSON_DeleteItemFromObject(effects_dict, old_effect_name);
            rename_effect_refs(model, old_effect_name, effect_name);
        }

        cJSON_DeleteItemFromObject(effects_dict, effect_name);
        cJSON_AddItemToObject(effects_dict, effect_name, effect_dict);
        persistent_dict_save(store);

    } else if (action && strcmp(action, "delete_effect") == 0 && effect_name && effect_name[0]) {
        if (effects_dict && cJSON_GetObjectItem(effects_dict, effect_name)) {
            persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
            cJSON_DeleteItemFromObject(effects_dict, effect_name);
            persistent_dict_save(store);
        }
        cJSON *ctx = build_global_context();
        cJSON_AddItemToObject(ctx, "effects", build_effects_list(effects_dict));
        cJSON_AddStringToObject(ctx, "page_title", "Effects");
        return webserver_render_response("setup/effects.html", ctx);
    }

    cJSON *ctx = build_global_context();
    cJSON_AddItemToObject(ctx, "effects", build_effects_list(effects_dict));
    cJSON_AddStringToObject(ctx, "page_title", "Effects");
    return webserver_render_response("setup/effects.html", ctx);
}

/* POST /effects/color_select — ColorSelectView is shared with scenes;
 * delegate to the single implementation in views_scenes.c. */
http_response_t *view_effects_color_select(http_request_t *req)
{
    return view_scenes_color_select(req);
}

/* GET /effects/summary — mirrors EffectsSummaryView.get() (plain sorted
 * effect NAME strings, not {name,pattern} objects). */
http_response_t *view_effects_summary(http_request_t *req)
{
    (void)req;
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    cJSON *effects_dict = model ? cJSON_GetObjectItem(model, "effects") : NULL;
    cJSON_AddItemToObject(ctx, "effect_names", sorted_keys_array(effects_dict));
    return webserver_render_response("setup/effects_summary.html", ctx);
}
