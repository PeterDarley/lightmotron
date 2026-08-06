#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "sound_manager.h"
#include "lighting.h"
#include "json_helpers.h"
#include "cJSON.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Mirrors _scene_name_id(): lowercase alnum/-/_ pass through, anything else
 * becomes '-'. Used for soundscape button DOM ids. */
static void slugify_id(const char *name, char *out, size_t outsz)
{
    size_t j = 0;
    for (size_t i = 0; name[i] != '\0' && j + 1 < outsz; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c) || c == '-' || c == '_') out[j++] = (char)tolower(c);
        else out[j++] = '-';
    }
    out[j] = '\0';
}

/* Non-negative integer parse tolerant of blank/garbage input, mirrors
 * _parse_non_negative_int(). */
static int parse_non_negative_int(const char *value)
{
    if (!value || !value[0]) return 0;
    int v = atoi(value);
    return v < 0 ? 0 : v;
}

/* Mirrors _is_enabled_setting() for the "1"/"true"/"yes"/"on" checkbox
 * convention used by entry_repeat_enabled form fields. */
static bool is_enabled_setting(const char *value)
{
    if (!value) return false;
    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) return true;
    return false;
}

static cJSON *get_soundscapes_dict(bool create_if_missing)
{
    cJSON *model = lighting_get_settings();
    if (!model) return NULL;

    cJSON *soundscapes = cJSON_GetObjectItem(model, "soundscapes");
    if (!soundscapes && create_if_missing) {
        soundscapes = cJSON_CreateObject();
        cJSON_AddItemToObject(model, "soundscapes", soundscapes);
    }
    return soundscapes;
}

static int get_current_volume(void)
{
    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    int volume = 20;
    if (store) {
        cJSON *v = persistent_dict_get_dup(store, "master_volume");
        if (v && cJSON_IsNumber(v)) volume = v->valueint;
        cJSON_Delete(v);
    }
    if (volume < 0) volume = 0;
    if (volume > 30) volume = 30;
    return volume;
}

/* Builds {soundscapes, soundscape_rows, soundscape_count,
 * soundscape_plural_suffix, soundscape_items, active_soundscape,
 * current_volume}. Mirrors _soundscapes_context(). Exposed (non-static, see
 * views.h) so view_home() in views_home.c can merge it in, matching
 * HomeView.get()'s `context.update(_soundscapes_context(include_active=True))`
 * for the soundscapes panel included by home.html. */
void add_soundscapes_context(cJSON *ctx, bool include_active)
{
    cJSON *soundscapes_dict = get_soundscapes_dict(false);

    int total = soundscapes_dict ? cJSON_GetArraySize(soundscapes_dict) : 0;
    const char **names = total > 0 ? calloc(total, sizeof(char *)) : NULL;
    int count = 0;
    if (names) {
        for (cJSON *item = soundscapes_dict->child; item; item = item->next) names[count++] = item->string;
        qsort(names, count, sizeof(char *), cmp_str);
    }

    cJSON *soundscapes_list = cJSON_CreateArray();
    cJSON *soundscape_rows = cJSON_CreateArray();
    cJSON *soundscape_items = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(soundscapes_list, cJSON_CreateString(names[i]));

        cJSON *raw_entries = cJSON_GetObjectItem(soundscapes_dict, names[i]);
        int entry_count = 0;
        int infinite_repeat_count = 0;
        int finite_repeat_count = 0;

        /* Distinct sound titles referenced, sorted for the preview string. */
        int entry_total = (raw_entries && cJSON_IsObject(raw_entries)) ? cJSON_GetArraySize(raw_entries) : 0;
        const char **sound_names = entry_total > 0 ? calloc(entry_total, sizeof(char *)) : NULL;
        int sound_count = 0;

        if (raw_entries && cJSON_IsObject(raw_entries)) {
            for (cJSON *entry = raw_entries->child; entry; entry = entry->next) {
                if (!cJSON_IsObject(entry)) continue;
                entry_count++;

                const char *sound_title = json_get_string(entry, "sound", "");
                if (sound_title[0] && sound_names) {
                    bool dup = false;
                    for (int s = 0; s < sound_count; s++) {
                        if (strcmp(sound_names[s], sound_title) == 0) { dup = true; break; }
                    }
                    if (!dup) sound_names[sound_count++] = sound_title;
                }

                int repeat_count = parse_non_negative_int(json_get_string(entry, "repeat", "0"));
                if (cJSON_IsNumber(cJSON_GetObjectItem(entry, "repeat"))) {
                    repeat_count = json_get_int(entry, "repeat", 0);
                    if (repeat_count < 0) repeat_count = 0;
                }
                bool repeat_enabled = cJSON_GetObjectItem(entry, "repeat_enabled")
                                           ? json_get_bool(entry, "repeat_enabled", false)
                                           : (repeat_count > 0);

                if (repeat_enabled) {
                    if (repeat_count == 0) infinite_repeat_count++;
                    else finite_repeat_count++;
                }
            }
        }

        if (sound_names) qsort(sound_names, sound_count, sizeof(char *), cmp_str);

        char preview[160];
        preview[0] = '\0';
        int preview_shown = sound_count < 2 ? sound_count : 2;
        for (int s = 0; s < preview_shown; s++) {
            if (s > 0) strncat(preview, ", ", sizeof(preview) - strlen(preview) - 1);
            strncat(preview, sound_names[s], sizeof(preview) - strlen(preview) - 1);
        }
        int extra = sound_count - preview_shown;
        if (extra > 0) {
            char suffix[16];
            snprintf(suffix, sizeof(suffix), " +%d", extra);
            strncat(preview, suffix, sizeof(preview) - strlen(preview) - 1);
        }
        if (preview_shown == 0) strncpy(preview, "-", sizeof(preview) - 1);
        free(sound_names);

        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "name", names[i]);
        cJSON_AddNumberToObject(row, "entry_count", entry_count);
        cJSON_AddNumberToObject(row, "infinite_repeat_count", infinite_repeat_count);
        cJSON_AddNumberToObject(row, "finite_repeat_count", finite_repeat_count);
        cJSON_AddStringToObject(row, "sounds_preview", preview);
        cJSON_AddItemToArray(soundscape_rows, row);

        char id[128];
        slugify_id(names[i], id, sizeof(id));
        cJSON *item_ctx = cJSON_CreateObject();
        cJSON_AddStringToObject(item_ctx, "name", names[i]);
        cJSON_AddStringToObject(item_ctx, "id", id);
        cJSON_AddItemToArray(soundscape_items, item_ctx);
    }
    free(names);

    cJSON_AddItemToObject(ctx, "soundscapes", soundscapes_list);
    cJSON_AddItemToObject(ctx, "soundscape_rows", soundscape_rows);
    cJSON_AddNumberToObject(ctx, "soundscape_count", count);
    cJSON_AddStringToObject(ctx, "soundscape_plural_suffix", count == 1 ? "" : "s");
    cJSON_AddItemToObject(ctx, "soundscape_items", soundscape_items);

    if (include_active) {
        const char *active = sound_manager_get_active_soundscape();
        if (active && active[0]) cJSON_AddStringToObject(ctx, "active_soundscape", active);
        else cJSON_AddNullToObject(ctx, "active_soundscape");
    }

    cJSON_AddNumberToObject(ctx, "current_volume", get_current_volume());
}

http_response_t *view_soundscapes(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");
    const char *soundscape_name = request_get_form_field(req, "soundscape_name");

    if (action && soundscape_name && soundscape_name[0]) {
        cJSON *soundscapes = get_soundscapes_dict(true);
        persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);

        if (strcmp(action, "create") == 0) {
            if (!cJSON_GetObjectItem(soundscapes, soundscape_name)) {
                cJSON_AddItemToObject(soundscapes, soundscape_name, cJSON_CreateObject());
                persistent_dict_mark_dirty(store); persistent_dict_save(store);
            }
        } else if (strcmp(action, "delete") == 0) {
            if (cJSON_GetObjectItem(soundscapes, soundscape_name)) {
                cJSON_DeleteItemFromObject(soundscapes, soundscape_name);
                persistent_dict_mark_dirty(store); persistent_dict_save(store);
            }
        }
    }

    cJSON *ctx = build_global_context();

    if (req->method == HTTP_METHOD_GET) {
        cJSON_AddStringToObject(ctx, "page_title", "Soundscapes");
        add_soundscapes_context(ctx, false);
        cJSON *model = lighting_get_settings();
        if (model) {
            cJSON *s = cJSON_GetObjectItem(model, "sounds");
            if (s) cJSON_AddItemReferenceToObject(ctx, "sounds", s);
        }
    } else {
        /* SoundscapesView.post() only returns _soundscapes_context() (no
         * "sounds" list) - setup/soundscapes.html doesn't need it for the
         * create/delete round trip. */
        add_soundscapes_context(ctx, false);
    }

    return webserver_render_response("setup/soundscapes.html", ctx);
}

http_response_t *view_soundscapes_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    add_soundscapes_context(ctx, false);
    return webserver_render_response("setup/soundscapes_summary.html", ctx);
}

http_response_t *view_soundscapes_status(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    add_soundscapes_context(ctx, true);
    return webserver_render_response("soundscapes/buttons.html", ctx);
}

http_response_t *view_play_soundscape(http_request_t *req)
{
    const char *name = request_get_form_field(req, "soundscape");
    if (name && name[0]) {
        sound_manager_play_soundscape(name);
    }

    cJSON *ctx = build_global_context();
    add_soundscapes_context(ctx, true);
    return webserver_render_response("soundscapes/buttons.html", ctx);
}

http_response_t *view_stop_soundscape(http_request_t *req)
{
    sound_manager_stop_all();

    cJSON *ctx = build_global_context();
    add_soundscapes_context(ctx, true);
    return webserver_render_response("soundscapes/buttons.html", ctx);
}

/* Builds {soundscape_name, soundscape, sounds, entries, edit_entry_name,
 * edit_entry}. Mirrors _build_soundscape_edit_context(). */
static cJSON *build_soundscape_edit_context(const char *soundscape_name, cJSON *soundscape,
                                            cJSON *sounds_dict, const char *edit_entry_name)
{
    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "soundscape_name", soundscape_name ? soundscape_name : "");
    if (soundscape) cJSON_AddItemReferenceToObject(ctx, "soundscape", soundscape);

    int total = sounds_dict ? cJSON_GetArraySize(sounds_dict) : 0;
    const char **names = total > 0 ? calloc(total, sizeof(char *)) : NULL;
    int count = 0;
    if (names) {
        for (cJSON *item = sounds_dict->child; item; item = item->next) names[count++] = item->string;
        qsort(names, count, sizeof(char *), cmp_str);
    }
    cJSON *sounds_arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) cJSON_AddItemToArray(sounds_arr, cJSON_CreateString(names[i]));
    free(names);
    cJSON_AddItemToObject(ctx, "sounds", sounds_arr);

    cJSON *entries = cJSON_CreateArray();
    cJSON *edit_entry = cJSON_CreateObject();
    bool have_edit_entry = false;

    if (soundscape && cJSON_IsObject(soundscape)) {
        for (cJSON *entry = soundscape->child; entry; entry = entry->next) {
            if (!cJSON_IsObject(entry)) continue;
            const char *entry_name = entry->string;

            int repeat_count = json_get_int(entry, "repeat", 0);
            if (repeat_count < 0) repeat_count = 0;
            bool repeat_enabled = cJSON_GetObjectItem(entry, "repeat_enabled")
                                       ? json_get_bool(entry, "repeat_enabled", false)
                                       : (repeat_count > 0);

            const char *repeat_mode_label;
            char repeat_buf[16];
            if (!repeat_enabled) repeat_mode_label = "off";
            else if (repeat_count == 0) repeat_mode_label = "infinite";
            else {
                snprintf(repeat_buf, sizeof(repeat_buf), "%d", repeat_count);
                repeat_mode_label = repeat_buf;
            }

            cJSON *entry_ctx = cJSON_CreateObject();
            cJSON_AddStringToObject(entry_ctx, "name", entry_name);
            cJSON_AddStringToObject(entry_ctx, "sound", json_get_string(entry, "sound", ""));
            cJSON_AddNumberToObject(entry_ctx, "repeat", repeat_count);
            cJSON_AddBoolToObject(entry_ctx, "repeat_enabled", repeat_enabled);
            cJSON_AddStringToObject(entry_ctx, "repeat_mode_label", repeat_mode_label);
            cJSON_AddStringToObject(entry_ctx, "after", json_get_string(entry, "after", ""));
            cJSON_AddItemToArray(entries, cJSON_Duplicate(entry_ctx, 1));

            if (edit_entry_name && edit_entry_name[0] && strcmp(entry_name, edit_entry_name) == 0) {
                cJSON_Delete(edit_entry);
                edit_entry = entry_ctx;
                have_edit_entry = true;
            } else {
                cJSON_Delete(entry_ctx);
            }
        }
    }

    cJSON_AddItemToObject(ctx, "entries", entries);
    cJSON_AddStringToObject(ctx, "edit_entry_name", have_edit_entry ? edit_entry_name : "");
    cJSON_AddItemToObject(ctx, "edit_entry", edit_entry);

    return ctx;
}

http_response_t *view_soundscape_edit(http_request_t *req)
{
    if (req->method == HTTP_METHOD_GET) {
        const char *soundscape_name = request_get_query_param(req, "soundscape");
        const char *edit_entry_name = request_get_query_param(req, "edit_entry");

        cJSON *soundscapes = get_soundscapes_dict(false);
        cJSON *soundscape = (soundscape_name && soundscapes) ? cJSON_GetObjectItem(soundscapes, soundscape_name) : NULL;
        cJSON *model = lighting_get_settings();
        cJSON *sounds_dict = model ? cJSON_GetObjectItem(model, "sounds") : NULL;

        cJSON *ctx = build_soundscape_edit_context(soundscape_name, soundscape, sounds_dict, edit_entry_name);
        return webserver_render_response("setup/soundscape_edit.html", ctx);
    }

    /* POST: add_entry / delete_entry / update_entry */
    const char *soundscape_name = request_get_form_field(req, "soundscape_name");
    const char *action = request_get_form_field(req, "action");

    cJSON *soundscapes = get_soundscapes_dict(true);
    cJSON *soundscape = (soundscape_name && soundscape_name[0])
                             ? cJSON_GetObjectItem(soundscapes, soundscape_name)
                             : NULL;
    if (!soundscape && soundscape_name && soundscape_name[0]) {
        soundscape = cJSON_CreateObject();
        cJSON_AddItemToObject(soundscapes, soundscape_name, soundscape);
    }
    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);

    if (action && strcmp(action, "add_entry") == 0) {
        const char *entry_sound = request_get_form_field(req, "entry_sound");
        const char *entry_repeat = request_get_form_field(req, "entry_repeat");
        bool repeat_enabled = is_enabled_setting(request_get_form_field(req, "entry_repeat_enabled"));
        int repeat_count = parse_non_negative_int(entry_repeat);

        if (entry_sound && entry_sound[0] && soundscape_name && soundscape_name[0] && soundscape) {
            int entry_num = cJSON_GetArraySize(soundscape) + 1;
            char entry_name[32];
            snprintf(entry_name, sizeof(entry_name), "entry%d", entry_num);

            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "sound", entry_sound);
            cJSON_AddBoolToObject(entry, "repeat_enabled", repeat_enabled);
            cJSON_AddNumberToObject(entry, "repeat", repeat_count);
            cJSON_DeleteItemFromObject(soundscape, entry_name);
            cJSON_AddItemToObject(soundscape, entry_name, entry);
            persistent_dict_mark_dirty(store); persistent_dict_save(store);
        }
    } else if (action && strcmp(action, "delete_entry") == 0) {
        const char *entry_name = request_get_form_field(req, "entry_name");
        if (entry_name && entry_name[0] && soundscape && cJSON_GetObjectItem(soundscape, entry_name)) {
            cJSON_DeleteItemFromObject(soundscape, entry_name);
            persistent_dict_mark_dirty(store); persistent_dict_save(store);
        }
    } else if (action && strcmp(action, "update_entry") == 0) {
        const char *entry_name = request_get_form_field(req, "entry_name");
        const char *entry_sound = request_get_form_field(req, "entry_sound");
        const char *entry_repeat = request_get_form_field(req, "entry_repeat");
        bool repeat_enabled = is_enabled_setting(request_get_form_field(req, "entry_repeat_enabled"));
        int repeat_count = parse_non_negative_int(entry_repeat);

        if (entry_name && entry_name[0] && entry_sound && entry_sound[0] && soundscape_name && soundscape_name[0]) {
            cJSON *entry = soundscape ? cJSON_GetObjectItem(soundscape, entry_name) : NULL;
            if (entry && cJSON_IsObject(entry)) {
                cJSON_DeleteItemFromObject(entry, "sound");
                cJSON_AddStringToObject(entry, "sound", entry_sound);
                cJSON_DeleteItemFromObject(entry, "repeat_enabled");
                cJSON_AddBoolToObject(entry, "repeat_enabled", repeat_enabled);
                cJSON_DeleteItemFromObject(entry, "repeat");
                cJSON_AddNumberToObject(entry, "repeat", repeat_count);
                persistent_dict_mark_dirty(store); persistent_dict_save(store);
            }
        }

        /* Saving an entry returns to the manager list, closing the editor,
         * mirroring SoundscapeEditView.post's update_entry branch. */
        cJSON *ctx = build_global_context();
        add_soundscapes_context(ctx, false);
        return webserver_render_response("setup/soundscapes.html", ctx);
    }

    cJSON *model = lighting_get_settings();
    cJSON *sounds_dict = model ? cJSON_GetObjectItem(model, "sounds") : NULL;
    cJSON *ctx = build_soundscape_edit_context(soundscape_name, soundscape, sounds_dict, NULL);
    return webserver_render_response("setup/soundscape_edit.html", ctx);
}
