/*
 * Scene management views: list/create/delete/rename/copy scenes, edit
 * entries within a scene (effect + target + cycles/after/inherit_target),
 * scene-level settings (kills, trigger_scenes_on_completion, sound,
 * stop_sounds_on_start/end), and the shared color-select fragment endpoint
 * (also used by the effect editor via view_effects_color_select()).
 *
 * Mirrors web/views.py: ScenesView, SceneEditView,
 * SceneEditAddTriggerSceneView, SceneEditRemoveTriggerSceneView,
 * ColorSelectView, plus the module-level helpers _scenes_list(),
 * _scene_name_id(), _scene_edit_context(), _rename_scene_refs(),
 * _rename_scene_entry_after_refs(), _clear_scene_entry_after_refs(),
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

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Mirrors _scene_name_id(): lowercased alnum/-/_ kept as-is, everything
 * else becomes '-'. Used to build unique DOM ids per scene. */
static void scene_name_id(const char *name, char *out, size_t outsz)
{
    size_t oi = 0;
    if (!name || outsz == 0) { if (outsz) out[0] = '\0'; return; }
    for (size_t i = 0; name[i] != '\0' && oi + 1 < outsz; i++) {
        unsigned char c = (unsigned char)name[i];
        out[oi++] = (isalnum(c) || c == '-' || c == '_') ? (char)tolower(c) : '-';
    }
    out[oi] = '\0';
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

/* ---------------------------------------------------------------------
 * Scene list building — mirrors _scenes_list().
 * ------------------------------------------------------------------- */

static cJSON *build_scenes_list(cJSON *scenes_dict)
{
    cJSON *result = cJSON_CreateArray();
    if (!scenes_dict) return result;

    int total = cJSON_GetArraySize(scenes_dict);
    if (total <= 0) return result;

    const char **names = calloc((size_t)total, sizeof(char *));
    if (!names) return result;
    int idx = 0;
    for (cJSON *item = scenes_dict->child; item; item = item->next) names[idx++] = item->string;
    qsort(names, (size_t)total, sizeof(char *), cmp_str);

    for (int i = 0; i < total; i++) {
        cJSON *entries = cJSON_GetObjectItem(scenes_dict, names[i]);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", names[i]);
        char id_buf[128];
        scene_name_id(names[i], id_buf, sizeof(id_buf));
        cJSON_AddStringToObject(entry, "name_id", id_buf);
        cJSON_AddNumberToObject(entry, "effect_count", entries ? cJSON_GetArraySize(entries) : 0);
        cJSON_AddItemToArray(result, entry);
    }

    free(names);
    return result;
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

/* Sound titles (+ file index) for the scene editor's sound dropdown /
 * stop-sounds checkboxes. Sounds live on the current model, same as
 * scenes/effects (see views_sounds.c's get_sounds_dict() for the same
 * storage convention). Only "title"/"file" are needed by scene_edit.html. */
static cJSON *build_sound_titles(cJSON *model)
{
    cJSON *result = cJSON_CreateArray();
    cJSON *sounds = model ? cJSON_GetObjectItem(model, "sounds") : NULL;
    if (!sounds) return result;

    int total = cJSON_GetArraySize(sounds);
    if (total <= 0) return result;
    const char **names = calloc((size_t)total, sizeof(char *));
    if (!names) return result;
    int idx = 0;
    for (cJSON *item = sounds->child; item; item = item->next) names[idx++] = item->string;
    qsort(names, (size_t)total, sizeof(char *), cmp_str);

    for (int i = 0; i < total; i++) {
        cJSON *def = cJSON_GetObjectItem(sounds, names[i]);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "title", names[i]);
        cJSON_AddNumberToObject(entry, "file", json_get_int(def, "file", 0));
        cJSON_AddItemToArray(result, entry);
    }

    free(names);
    return result;
}

/* Comma-separated list -> cJSON string array, keeping only tokens that are
 * keys of valid_keys_obj (mirrors the list-comprehension membership checks
 * in SceneEditView.post()'s "update_scene_settings" branch). */
static void csv_tokens_filtered(const char *raw, cJSON *valid_keys_obj, cJSON *out_array)
{
    if (!raw || !raw[0]) return;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", raw);
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        while (*tok == ' ') tok++;
        size_t l = strlen(tok);
        while (l > 0 && tok[l - 1] == ' ') tok[--l] = '\0';
        if (l > 0 && valid_keys_obj && cJSON_GetObjectItem(valid_keys_obj, tok)) {
            cJSON_AddItemToArray(out_array, cJSON_CreateString(tok));
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
}

static void array_to_csv(cJSON *arr, char *out, size_t outsz)
{
    out[0] = '\0';
    if (!arr) return;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item || !cJSON_IsString(item)) continue;
        if (out[0] != '\0') strncat(out, ",", outsz - strlen(out) - 1);
        strncat(out, item->valuestring, outsz - strlen(out) - 1);
    }
}

/* Updates all "after" references within a scene when an entry is renamed —
 * mirrors _rename_scene_entry_after_refs(). */
static void rename_scene_entry_after_refs(cJSON *scene_data, const char *old_entry_name, const char *new_entry_name)
{
    if (!scene_data) return;
    for (cJSON *entry = scene_data->child; entry; entry = entry->next) {
        if (!cJSON_IsObject(entry)) continue;
        cJSON *after = cJSON_GetObjectItem(entry, "after");
        if (after && cJSON_IsString(after) && strcmp(after->valuestring, old_entry_name) == 0) {
            cJSON_ReplaceItemInObject(entry, "after", cJSON_CreateString(new_entry_name));
        }
    }
}

/* Removes dangling "after" references to a deleted entry — mirrors
 * _clear_scene_entry_after_refs(). */
static void clear_scene_entry_after_refs(cJSON *scene_data, const char *removed_entry_name)
{
    if (!scene_data) return;
    for (cJSON *entry = scene_data->child; entry; entry = entry->next) {
        if (!cJSON_IsObject(entry)) continue;
        cJSON *after = cJSON_GetObjectItem(entry, "after");
        if (after && cJSON_IsString(after) && strcmp(after->valuestring, removed_entry_name) == 0) {
            cJSON_DeleteItemFromObject(entry, "after");
        }
    }
}

/* Updates scene_settings' key (if the scene was renamed) and the
 * kills/trigger_scenes_on_completion lists that reference a scene by its
 * old name — mirrors _rename_scene_refs(). */
static void rename_scene_refs(cJSON *model, const char *old_name, const char *new_name)
{
    cJSON *scene_settings = model ? cJSON_GetObjectItem(model, "scene_settings") : NULL;
    if (!scene_settings) return;

    cJSON *old_meta = cJSON_GetObjectItem(scene_settings, old_name);
    if (old_meta) {
        cJSON *detached = cJSON_DetachItemFromObject(scene_settings, old_name);
        cJSON_DeleteItemFromObject(scene_settings, new_name);
        cJSON_AddItemToObject(scene_settings, new_name, detached);
    }

    for (cJSON *meta = scene_settings->child; meta; meta = meta->next) {
        cJSON *kills = cJSON_GetObjectItem(meta, "kills");
        if (kills && cJSON_IsArray(kills)) {
            int n = cJSON_GetArraySize(kills);
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(kills, i);
                if (item && cJSON_IsString(item) && strcmp(item->valuestring, old_name) == 0) {
                    cJSON_ReplaceItemInArray(kills, i, cJSON_CreateString(new_name));
                }
            }
        }
        cJSON *triggers = cJSON_GetObjectItem(meta, "trigger_scenes_on_completion");
        if (triggers && cJSON_IsArray(triggers)) {
            int n = cJSON_GetArraySize(triggers);
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(triggers, i);
                if (item && cJSON_IsString(item) && strcmp(item->valuestring, old_name) == 0) {
                    cJSON_ReplaceItemInArray(triggers, i, cJSON_CreateString(new_name));
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------
 * Edit-context building — mirrors _scene_edit_context().
 * ------------------------------------------------------------------- */

static void fill_scene_edit_context(cJSON *ctx, cJSON *model, const char *scene_name, const char *edit_entry_name)
{
    cJSON *scenes_dict = model ? cJSON_GetObjectItem(model, "scenes") : NULL;
    cJSON *effects_dict = model ? cJSON_GetObjectItem(model, "effects") : NULL;
    cJSON *scene_data = (scenes_dict && scene_name) ? cJSON_GetObjectItem(scenes_dict, scene_name) : NULL;

    int total = scene_data ? cJSON_GetArraySize(scene_data) : 0;
    const char **entry_names = total > 0 ? calloc((size_t)total, sizeof(char *)) : NULL;
    if (entry_names) {
        int i = 0;
        for (cJSON *item = scene_data->child; item; item = item->next) entry_names[i++] = item->string;
        qsort(entry_names, (size_t)total, sizeof(char *), cmp_str);
    }

    cJSON *scene_entries_arr = cJSON_CreateArray();
    cJSON *chainable_arr = cJSON_CreateArray();

    for (int i = 0; i < total; i++) {
        cJSON *entry = cJSON_GetObjectItem(scene_data, entry_names[i]);
        const char *effect_name = json_get_string(entry, "effect", "");
        cJSON *effect_def = effects_dict ? cJSON_GetObjectItem(effects_dict, effect_name) : NULL;
        const char *pattern = json_get_string(effect_def, "pattern", "?");

        cJSON *details = cJSON_CreateObject();
        cJSON_AddStringToObject(details, "effect", effect_name);
        cJSON_AddStringToObject(details, "target", json_get_string(entry, "target", "all"));
        cJSON_AddStringToObject(details, "pattern", pattern);
        cJSON_AddStringToObject(details, "after", json_get_string(entry, "after", ""));
        cJSON_AddBoolToObject(details, "inherit_target", json_get_bool(entry, "inherit_target", false));

        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateString(entry_names[i]));
        cJSON_AddItemToArray(pair, details);
        cJSON_AddItemToArray(scene_entries_arr, pair);

        if (!edit_entry_name || strcmp(entry_names[i], edit_entry_name) != 0) {
            cJSON_AddItemToArray(chainable_arr, cJSON_CreateString(entry_names[i]));
        }
    }
    free(entry_names);

    cJSON *scene_settings = model ? cJSON_GetObjectItem(model, "scene_settings") : NULL;
    cJSON *scene_meta = (scene_settings && scene_name) ? cJSON_GetObjectItem(scene_settings, scene_name) : NULL;
    cJSON *kills = scene_meta ? cJSON_GetObjectItem(scene_meta, "kills") : NULL;
    cJSON *trigger_scenes = scene_meta ? cJSON_GetObjectItem(scene_meta, "trigger_scenes_on_completion") : NULL;
    cJSON *stop_start = scene_meta ? cJSON_GetObjectItem(scene_meta, "stop_sounds_on_start") : NULL;
    cJSON *stop_end = scene_meta ? cJSON_GetObjectItem(scene_meta, "stop_sounds_on_end") : NULL;

    int all_total = scenes_dict ? cJSON_GetArraySize(scenes_dict) : 0;
    const char **all_names = all_total > 0 ? calloc((size_t)all_total, sizeof(char *)) : NULL;
    if (all_names) {
        int i = 0;
        for (cJSON *item = scenes_dict->child; item; item = item->next) all_names[i++] = item->string;
        qsort(all_names, (size_t)all_total, sizeof(char *), cmp_str);
    }
    cJSON *killable_arr = cJSON_CreateArray();
    cJSON *triggerable_arr = cJSON_CreateArray();
    for (int i = 0; i < all_total; i++) {
        if (scene_name && strcmp(all_names[i], scene_name) == 0) continue;
        cJSON_AddItemToArray(killable_arr, cJSON_CreateString(all_names[i]));
        cJSON_AddItemToArray(triggerable_arr, cJSON_CreateString(all_names[i]));
    }
    free(all_names);

    char scene_id_buf[128];
    scene_name_id(scene_name ? scene_name : "", scene_id_buf, sizeof(scene_id_buf));

    cJSON_AddStringToObject(ctx, "scene_name", scene_name ? scene_name : "");
    cJSON_AddStringToObject(ctx, "scene_name_id", scene_id_buf);
    cJSON_AddItemToObject(ctx, "scene_entries", scene_entries_arr);
    cJSON_AddItemToObject(ctx, "available_effects", sorted_keys_array(effects_dict));

    cJSON *named_ranges_obj = model ? cJSON_GetObjectItem(model, "named_ranges") : NULL;
    if (named_ranges_obj) cJSON_AddItemReferenceToObject(ctx, "named_ranges", named_ranges_obj);
    cJSON_AddItemToObject(ctx, "named_range_names", sorted_keys_array(named_ranges_obj));

    cJSON_AddItemToObject(ctx, "chainable_entries", chainable_arr);
    cJSON_AddItemToObject(ctx, "killable_scenes", killable_arr);

    cJSON *scene_kills_arr = kills ? cJSON_Duplicate(kills, 1) : cJSON_CreateArray();
    char kills_csv[512];
    array_to_csv(scene_kills_arr, kills_csv, sizeof(kills_csv));
    cJSON_AddItemToObject(ctx, "scene_kills", scene_kills_arr);
    cJSON_AddStringToObject(ctx, "scene_kills_csv", kills_csv);

    cJSON *trig_arr = trigger_scenes ? cJSON_Duplicate(trigger_scenes, 1) : cJSON_CreateArray();
    cJSON_AddItemToObject(ctx, "trigger_scenes_on_completion", trig_arr);
    cJSON_AddItemToObject(ctx, "triggerable_scenes", triggerable_arr);

    cJSON_AddStringToObject(ctx, "scene_sound", json_get_string(scene_meta, "sound", ""));

    cJSON *stop_start_arr = stop_start ? cJSON_Duplicate(stop_start, 1) : cJSON_CreateArray();
    char stop_start_csv[512];
    array_to_csv(stop_start_arr, stop_start_csv, sizeof(stop_start_csv));
    cJSON_AddItemToObject(ctx, "scene_stop_sounds_on_start", stop_start_arr);
    cJSON_AddStringToObject(ctx, "scene_stop_sounds_on_start_csv", stop_start_csv);

    cJSON *stop_end_arr = stop_end ? cJSON_Duplicate(stop_end, 1) : cJSON_CreateArray();
    char stop_end_csv[512];
    array_to_csv(stop_end_arr, stop_end_csv, sizeof(stop_end_csv));
    cJSON_AddItemToObject(ctx, "scene_stop_sounds_on_end", stop_end_arr);
    cJSON_AddStringToObject(ctx, "scene_stop_sounds_on_end_csv", stop_end_csv);

    cJSON_AddBoolToObject(ctx, "scene_active_on_boot", json_get_bool(scene_meta, "active_on_boot", false));

    cJSON_AddStringToObject(ctx, "page_title", "Edit Scene");

    /* _sounds_context() merge - only "sounds" (title/file) is needed here. */
    cJSON_AddItemToObject(ctx, "sounds", build_sound_titles(model));

    if (edit_entry_name && edit_entry_name[0] != '\0') {
        cJSON *entry_dict = scene_data ? cJSON_GetObjectItem(scene_data, edit_entry_name) : NULL;
        cJSON_AddStringToObject(ctx, "edit_entry_name", edit_entry_name);
        cJSON_AddStringToObject(ctx, "edit_entry_effect", json_get_string(entry_dict, "effect", ""));
        cJSON_AddStringToObject(ctx, "edit_entry_target", json_get_string(entry_dict, "target", ""));

        cJSON *cycles_item = entry_dict ? cJSON_GetObjectItem(entry_dict, "cycles") : NULL;
        char cycles_buf[32] = "";
        if (cycles_item && cJSON_IsNumber(cycles_item)) {
            snprintf(cycles_buf, sizeof(cycles_buf), "%d", (int)cycles_item->valuedouble);
        }
        cJSON_AddStringToObject(ctx, "edit_entry_cycles", cycles_buf);
        cJSON_AddStringToObject(ctx, "edit_entry_after", json_get_string(entry_dict, "after", ""));
        cJSON_AddBoolToObject(ctx, "edit_entry_inherit_target", json_get_bool(entry_dict, "inherit_target", false));
        cJSON_AddStringToObject(ctx, "old_entry_name", edit_entry_name);
    }
}

static void update_scene_settings_from_form(http_request_t *req, cJSON *model, const char *scene_name)
{
    cJSON *scenes_dict = cJSON_GetObjectItem(model, "scenes");
    cJSON *sounds_dict = cJSON_GetObjectItem(model, "sounds");

    cJSON *kills_list = cJSON_CreateArray();
    csv_tokens_filtered(request_get_form_field(req, "kills"), scenes_dict, kills_list);

    cJSON *trig_list = cJSON_CreateArray();
    csv_tokens_filtered(request_get_form_field(req, "trigger_scenes_on_completion"), scenes_dict, trig_list);

    const char *scene_sound = request_get_form_field(req, "scene_sound");

    cJSON *stop_start_list = cJSON_CreateArray();
    csv_tokens_filtered(request_get_form_field(req, "stop_sounds_on_start"), sounds_dict, stop_start_list);

    cJSON *stop_end_list = cJSON_CreateArray();
    csv_tokens_filtered(request_get_form_field(req, "stop_sounds_on_end"), sounds_dict, stop_end_list);

    cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
    if (!scene_settings) { scene_settings = cJSON_CreateObject(); cJSON_AddItemToObject(model, "scene_settings", scene_settings); }
    cJSON *meta = cJSON_GetObjectItem(scene_settings, scene_name);
    if (!meta) { meta = cJSON_CreateObject(); cJSON_AddItemToObject(scene_settings, scene_name, meta); }

    cJSON_DeleteItemFromObject(meta, "kills");
    if (cJSON_GetArraySize(kills_list) > 0) cJSON_AddItemToObject(meta, "kills", kills_list);
    else cJSON_Delete(kills_list);

    cJSON_DeleteItemFromObject(meta, "trigger_scenes_on_completion");
    if (cJSON_GetArraySize(trig_list) > 0) cJSON_AddItemToObject(meta, "trigger_scenes_on_completion", trig_list);
    else cJSON_Delete(trig_list);

    cJSON_DeleteItemFromObject(meta, "sound");
    if (scene_sound && scene_sound[0]) cJSON_AddStringToObject(meta, "sound", scene_sound);

    cJSON_DeleteItemFromObject(meta, "stop_sounds_on_start");
    if (cJSON_GetArraySize(stop_start_list) > 0) cJSON_AddItemToObject(meta, "stop_sounds_on_start", stop_start_list);
    else cJSON_Delete(stop_start_list);

    cJSON_DeleteItemFromObject(meta, "stop_sounds_on_end");
    if (cJSON_GetArraySize(stop_end_list) > 0) cJSON_AddItemToObject(meta, "stop_sounds_on_end", stop_end_list);
    else cJSON_Delete(stop_end_list);
}

/* Handles the "active on boot" toggle as its own isolated action (rather
 * than folding it into update_scene_settings_from_form's batch), so saving
 * it can't be affected by, or clobber, the other scene-settings forms -
 * mirrors SceneEditView.post()'s separate "set_active_on_boot" branch in
 * web/views_scenes.py. */
static void set_active_on_boot_from_form(http_request_t *req, cJSON *model, const char *scene_name)
{
    const char *value = request_get_form_field(req, "active_on_boot");

    cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
    if (!scene_settings) { scene_settings = cJSON_CreateObject(); cJSON_AddItemToObject(model, "scene_settings", scene_settings); }
    cJSON *meta = cJSON_GetObjectItem(scene_settings, scene_name);
    if (!meta) { meta = cJSON_CreateObject(); cJSON_AddItemToObject(scene_settings, scene_name, meta); }

    cJSON_DeleteItemFromObject(meta, "active_on_boot");
    if (value && strcmp(value, "1") == 0) cJSON_AddBoolToObject(meta, "active_on_boot", true);
}

/* ---------------------------------------------------------------------
 * Views
 * ------------------------------------------------------------------- */

/* GET/POST /scenes — mirrors ScenesView.get()/post() (list, create_scene,
 * delete_scene, rename_scene, copy_scene). */
http_response_t *view_scenes(http_request_t *req)
{
    cJSON *model = lighting_get_settings();
    cJSON *scenes_dict = model ? cJSON_GetObjectItem(model, "scenes") : NULL;

    if (req->method == HTTP_METHOD_POST && model) {
        if (!scenes_dict) { scenes_dict = cJSON_CreateObject(); cJSON_AddItemToObject(model, "scenes", scenes_dict); }
        persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);

        const char *action = request_get_form_field(req, "action");
        const char *scene_name = request_get_form_field(req, "scene_name");

        if (action && strcmp(action, "create_scene") == 0 && scene_name && scene_name[0]) {
            if (!cJSON_GetObjectItem(scenes_dict, scene_name)) {
                cJSON_AddItemToObject(scenes_dict, scene_name, cJSON_CreateObject());
            }
            persistent_dict_save(store);

        } else if (action && strcmp(action, "delete_scene") == 0 && scene_name && scene_name[0] &&
                   cJSON_GetObjectItem(scenes_dict, scene_name)) {
            cJSON_DeleteItemFromObject(scenes_dict, scene_name);
            persistent_dict_save(store);

        } else if (action && strcmp(action, "rename_scene") == 0 && scene_name && scene_name[0]) {
            const char *new_name = request_get_form_field(req, "new_scene_name");
            if (new_name && new_name[0] && strcmp(new_name, scene_name) != 0 &&
                cJSON_GetObjectItem(scenes_dict, scene_name) && !cJSON_GetObjectItem(scenes_dict, new_name)) {
                cJSON *detached = cJSON_DetachItemFromObject(scenes_dict, scene_name);
                cJSON_AddItemToObject(scenes_dict, new_name, detached);

                cJSON *default_scene = cJSON_GetObjectItem(model, "default_scene");
                if (default_scene && cJSON_IsString(default_scene) &&
                    strcmp(default_scene->valuestring, scene_name) == 0) {
                    cJSON_ReplaceItemInObject(model, "default_scene", cJSON_CreateString(new_name));
                }
                rename_scene_refs(model, scene_name, new_name);
                persistent_dict_save(store);
            }

        } else if (action && strcmp(action, "copy_scene") == 0 && scene_name && scene_name[0] &&
                   cJSON_GetObjectItem(scenes_dict, scene_name)) {
            const char *new_name = request_get_form_field(req, "new_scene_name");
            if (new_name && new_name[0] && !cJSON_GetObjectItem(scenes_dict, new_name)) {
                cJSON *src = cJSON_GetObjectItem(scenes_dict, scene_name);
                cJSON *copy = json_deep_clone(src);
                cJSON_AddItemToObject(scenes_dict, new_name, copy);
                persistent_dict_save(store);
            }
        }
    }

    cJSON *ctx = build_global_context();
    cJSON_AddItemToObject(ctx, "scenes", build_scenes_list(scenes_dict));
    cJSON_AddStringToObject(ctx, "page_title", "Scenes");
    return webserver_render_response("setup/scenes.html", ctx);
}

/* GET/POST /scenes/edit — mirrors SceneEditView.get()/post(). */
http_response_t *view_scene_edit(http_request_t *req)
{
    cJSON *model = lighting_get_settings();
    cJSON *scenes_dict = model ? cJSON_GetObjectItem(model, "scenes") : NULL;

    if (req->method == HTTP_METHOD_GET) {
        const char *scene_name = request_get_query_param(req, "scene");
        if (!scene_name || !scene_name[0] || !scenes_dict || !cJSON_GetObjectItem(scenes_dict, scene_name)) {
            return response_create(200, "text/html", "<p class=\"text-danger small\">Scene not found.</p>");
        }
        const char *edit_entry = request_get_query_param(req, "edit_entry");
        cJSON *ctx = build_global_context();
        fill_scene_edit_context(ctx, model, scene_name, (edit_entry && edit_entry[0]) ? edit_entry : NULL);
        return webserver_render_response("setup/scene_edit.html", ctx);
    }

    /* POST */
    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    const char *action = request_get_form_field(req, "action");
    const char *scene_name = request_get_form_field(req, "scene_name");
    const char *entry_name = request_get_form_field(req, "entry_name");
    const char *effect_name = request_get_form_field(req, "effect_name");
    const char *target = request_get_form_field(req, "target");

    cJSON *scene_data = (scene_name && scenes_dict) ? cJSON_GetObjectItem(scenes_dict, scene_name) : NULL;

    if (scene_name && scene_data) {
        if (action && (strcmp(action, "add_entry") == 0 || strcmp(action, "update_entry") == 0) &&
            entry_name && entry_name[0] && effect_name && effect_name[0]) {
            const char *old_entry_name = request_get_form_field(req, "old_entry_name");

            if (old_entry_name && old_entry_name[0] && strcmp(old_entry_name, entry_name) != 0 &&
                cJSON_GetObjectItem(scene_data, old_entry_name)) {
                rename_scene_entry_after_refs(scene_data, old_entry_name, entry_name);
                cJSON_DeleteItemFromObject(scene_data, old_entry_name);
            }

            cJSON *entry_dict = cJSON_CreateObject();
            cJSON_AddStringToObject(entry_dict, "effect", effect_name);
            cJSON_AddStringToObject(entry_dict, "target", (target && target[0]) ? target : "all");

            const char *cycles_value = request_get_form_field(req, "cycles");
            if (cycles_value && cycles_value[0]) {
                char *endptr = NULL;
                long cycles = strtol(cycles_value, &endptr, 10);
                if (endptr != cycles_value && *endptr == '\0') {
                    cJSON_AddNumberToObject(entry_dict, "cycles", (double)cycles);
                }
            }

            const char *after_value = request_get_form_field(req, "after");
            if (after_value && after_value[0]) cJSON_AddStringToObject(entry_dict, "after", after_value);

            const char *inherit_target_value = request_get_form_field(req, "inherit_target");
            if (inherit_target_value && strcmp(inherit_target_value, "1") == 0) {
                cJSON_AddBoolToObject(entry_dict, "inherit_target", true);
            }

            cJSON_DeleteItemFromObject(scene_data, entry_name);
            cJSON_AddItemToObject(scene_data, entry_name, entry_dict);
            persistent_dict_save(store);

        } else if (action && strcmp(action, "delete_entry") == 0 && entry_name && entry_name[0]) {
            if (cJSON_GetObjectItem(scene_data, entry_name)) {
                clear_scene_entry_after_refs(scene_data, entry_name);
                cJSON_DeleteItemFromObject(scene_data, entry_name);
                persistent_dict_save(store);
            }

        } else if (action && strcmp(action, "update_scene_settings") == 0) {
            update_scene_settings_from_form(req, model, scene_name);
            persistent_dict_save(store);

        } else if (action && strcmp(action, "set_active_on_boot") == 0) {
            set_active_on_boot_from_form(req, model, scene_name);
            persistent_dict_save(store);
        }
    }

    cJSON *ctx = build_global_context();
    fill_scene_edit_context(ctx, model, scene_name, NULL);
    return webserver_render_response("setup/scene_edit.html", ctx);
}

/* POST /scenes/edit/add_trigger_scene — mirrors SceneEditAddTriggerSceneView.post(). */
http_response_t *view_scene_edit_add_trigger(http_request_t *req)
{
    const char *scene_name = request_get_form_field(req, "scene_name");
    const char *trigger_scene_to_add = request_get_form_field(req, "trigger_scene_to_add");

    cJSON *model = lighting_get_settings();
    cJSON *scenes_dict = model ? cJSON_GetObjectItem(model, "scenes") : NULL;

    if (!scene_name || !scene_name[0] || !scenes_dict || !cJSON_GetObjectItem(scenes_dict, scene_name)) {
        return response_create(200, "text/html", "<div class=\"alert alert-danger small\">Scene not found.</div>");
    }
    if (!trigger_scene_to_add || !trigger_scene_to_add[0] || !cJSON_GetObjectItem(scenes_dict, trigger_scene_to_add)) {
        return response_create(200, "text/html", "<div class=\"alert alert-danger small\">Invalid trigger scene.</div>");
    }

    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
    if (!scene_settings) { scene_settings = cJSON_CreateObject(); cJSON_AddItemToObject(model, "scene_settings", scene_settings); }
    cJSON *meta = cJSON_GetObjectItem(scene_settings, scene_name);
    if (!meta) { meta = cJSON_CreateObject(); cJSON_AddItemToObject(scene_settings, scene_name, meta); }

    cJSON *triggers = cJSON_GetObjectItem(meta, "trigger_scenes_on_completion");
    if (!triggers || !cJSON_IsArray(triggers)) {
        triggers = cJSON_CreateArray();
        cJSON_DeleteItemFromObject(meta, "trigger_scenes_on_completion");
        cJSON_AddItemToObject(meta, "trigger_scenes_on_completion", triggers);
    }

    bool already_present = false;
    int n = cJSON_GetArraySize(triggers);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(triggers, i);
        if (item && cJSON_IsString(item) && strcmp(item->valuestring, trigger_scene_to_add) == 0) {
            already_present = true;
            break;
        }
    }
    if (!already_present) {
        cJSON_AddItemToArray(triggers, cJSON_CreateString(trigger_scene_to_add));
        persistent_dict_save(store);
    }

    char scene_id[128];
    scene_name_id(scene_name, scene_id, sizeof(scene_id));

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "scene_name", scene_name);
    cJSON_AddStringToObject(ctx, "scene_name_id", scene_id);
    cJSON_AddItemToObject(ctx, "trigger_scenes_on_completion", cJSON_Duplicate(triggers, 1));

    cJSON *triggerable = cJSON_CreateArray();
    cJSON *all_names = sorted_keys_array(scenes_dict);
    int total = cJSON_GetArraySize(all_names);
    for (int i = 0; i < total; i++) {
        cJSON *item = cJSON_GetArrayItem(all_names, i);
        if (item && cJSON_IsString(item) && strcmp(item->valuestring, scene_name) != 0) {
            cJSON_AddItemToArray(triggerable, cJSON_CreateString(item->valuestring));
        }
    }
    cJSON_Delete(all_names);
    cJSON_AddItemToObject(ctx, "triggerable_scenes", triggerable);
    cJSON_AddStringToObject(ctx, "trigger_scene_to_add", "");

    return webserver_render_response("setup/scene_edit_trigger_control.html", ctx);
}

/* POST /scenes/edit/remove_trigger_scene — mirrors SceneEditRemoveTriggerSceneView.post(). */
http_response_t *view_scene_edit_remove_trigger(http_request_t *req)
{
    const char *scene_name = request_get_form_field(req, "scene_name");
    const char *trigger_scene_to_remove = request_get_form_field(req, "trigger_scene_to_remove");

    cJSON *model = lighting_get_settings();
    cJSON *scenes_dict = model ? cJSON_GetObjectItem(model, "scenes") : NULL;

    if (!scene_name || !scene_name[0] || !scenes_dict || !cJSON_GetObjectItem(scenes_dict, scene_name)) {
        return response_create(200, "text/html", "<div class=\"alert alert-danger small\">Scene not found.</div>");
    }

    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
    if (!scene_settings) { scene_settings = cJSON_CreateObject(); cJSON_AddItemToObject(model, "scene_settings", scene_settings); }
    cJSON *meta = cJSON_GetObjectItem(scene_settings, scene_name);
    if (!meta) { meta = cJSON_CreateObject(); cJSON_AddItemToObject(scene_settings, scene_name, meta); }

    cJSON *triggers = cJSON_GetObjectItem(meta, "trigger_scenes_on_completion");

    cJSON *remaining = cJSON_CreateArray();
    bool removed = false;
    if (triggers && cJSON_IsArray(triggers)) {
        int n = cJSON_GetArraySize(triggers);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(triggers, i);
            if (!item || !cJSON_IsString(item)) continue;
            if (!removed && trigger_scene_to_remove && strcmp(item->valuestring, trigger_scene_to_remove) == 0) {
                removed = true;
                continue;
            }
            cJSON_AddItemToArray(remaining, cJSON_CreateString(item->valuestring));
        }
    }

    if (removed) {
        cJSON_DeleteItemFromObject(meta, "trigger_scenes_on_completion");
        if (cJSON_GetArraySize(remaining) > 0) {
            cJSON_AddItemToObject(meta, "trigger_scenes_on_completion", cJSON_Duplicate(remaining, 1));
        }
        persistent_dict_save(store);
    }

    char scene_id[128];
    scene_name_id(scene_name, scene_id, sizeof(scene_id));

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "scene_name", scene_name);
    cJSON_AddStringToObject(ctx, "scene_name_id", scene_id);
    cJSON_AddItemToObject(ctx, "trigger_scenes_on_completion", remaining);

    cJSON *triggerable = cJSON_CreateArray();
    cJSON *all_names = sorted_keys_array(scenes_dict);
    int total = cJSON_GetArraySize(all_names);
    for (int i = 0; i < total; i++) {
        cJSON *item = cJSON_GetArrayItem(all_names, i);
        if (item && cJSON_IsString(item) && strcmp(item->valuestring, scene_name) != 0) {
            cJSON_AddItemToArray(triggerable, cJSON_CreateString(item->valuestring));
        }
    }
    cJSON_Delete(all_names);
    cJSON_AddItemToObject(ctx, "triggerable_scenes", triggerable);
    cJSON_AddStringToObject(ctx, "trigger_scene_to_add", "");

    return webserver_render_response("setup/scene_edit_trigger_control.html", ctx);
}

/* POST /scenes/color_select and /effects/color_select (via
 * view_effects_color_select() delegating here) — mirrors ColorSelectView.post(). */
http_response_t *view_scenes_color_select(http_request_t *req)
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

void add_scenes_summary_context(cJSON *ctx)
{
    cJSON *model = lighting_get_settings();
    cJSON *scenes_dict = model ? cJSON_GetObjectItem(model, "scenes") : NULL;
    cJSON_AddItemToObject(ctx, "scenes", build_scenes_list(scenes_dict));
}

/* GET /scenes/summary — mirrors ScenesSummaryView.get(). */
http_response_t *view_scenes_summary(http_request_t *req)
{
    (void)req;
    cJSON *ctx = build_global_context();
    add_scenes_summary_context(ctx);
    return webserver_render_response("setup/scenes_summary.html", ctx);
}
