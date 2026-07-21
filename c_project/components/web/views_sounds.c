#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "sound_manager.h"
#include "audio_player.h"
#include "lighting.h"
#include "json_helpers.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Sound settings live on the current model, mirroring SoundManager.get_sounds()
 * in lib/sounds.py (models-based storage; legacy top-level "sounds" fallback
 * is not modeled in the C port since models are the only supported layout). */
static cJSON *get_sounds_dict(bool create_if_missing)
{
    cJSON *model = lighting_get_settings();
    if (!model) return NULL;

    cJSON *sounds = cJSON_GetObjectItem(model, "sounds");
    if (!sounds && create_if_missing) {
        sounds = cJSON_CreateObject();
        cJSON_AddItemToObject(model, "sounds", sounds);
    }
    return sounds;
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

/* Returns true if `title` appears in the sound_manager's currently-playing
 * titles array (mirrors _playing_sounds_by_title() membership check). */
static bool title_is_playing(cJSON *playing_arr, const char *title)
{
    if (!playing_arr || !title) return false;
    for (cJSON *item = playing_arr->child; item; item = item->next) {
        if (cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, title) == 0) return true;
    }
    return false;
}

/* Build a sorted "sounds" list of {title, file, high_quality, show_on_home,
 * is_playing, module_idx}. Mirrors _sounds_list()/_sounds_context(). When
 * include_playing is true, entries currently playing get is_playing=true
 * and module_idx set via sound_manager_get_playing_module(). */
static cJSON *build_sounds_list(cJSON *sounds_dict, bool include_playing, bool home_only)
{
    cJSON *result = cJSON_CreateArray();
    if (!sounds_dict) return result;

    int total = cJSON_GetArraySize(sounds_dict);
    if (total <= 0) return result;

    const char **names = calloc(total, sizeof(char *));
    if (!names) return result;

    int count = 0;
    for (cJSON *item = sounds_dict->child; item; item = item->next) {
        if (home_only && !json_get_bool(item, "show_on_home", true)) continue;
        names[count++] = item->string;
    }
    qsort(names, count, sizeof(char *), cmp_str);

    cJSON *playing = include_playing ? sound_manager_get_playing() : NULL;

    for (int i = 0; i < count; i++) {
        cJSON *sound = cJSON_GetObjectItem(sounds_dict, names[i]);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "title", names[i]);
        cJSON_AddNumberToObject(entry, "file", json_get_int(sound, "file", 0));
        cJSON_AddBoolToObject(entry, "high_quality", json_get_bool(sound, "high_quality", false));
        cJSON_AddBoolToObject(entry, "show_on_home", json_get_bool(sound, "show_on_home", true));
        bool is_playing = title_is_playing(playing, names[i]);
        cJSON_AddBoolToObject(entry, "is_playing", is_playing);
        cJSON_AddNumberToObject(entry, "module_idx",
                                is_playing ? sound_manager_get_playing_module(names[i]) : -1);
        cJSON_AddItemToArray(result, entry);
    }

    if (playing) cJSON_Delete(playing);
    free(names);
    return result;
}

/* Builds the {sounds, current_volume, sounds_poll_interval_seconds} context
 * shared by several views. Mirrors _sounds_context(). */
static void add_sounds_context(cJSON *ctx, bool include_playing, bool home_only)
{
    cJSON *sounds_dict = get_sounds_dict(false);
    cJSON *sounds_list = build_sounds_list(sounds_dict, include_playing, home_only);

    bool any_playing = false;
    if (include_playing) {
        for (cJSON *item = sounds_list->child; item; item = item->next) {
            if (cJSON_IsTrue(cJSON_GetObjectItem(item, "is_playing"))) { any_playing = true; break; }
        }
    }

    cJSON_AddItemToObject(ctx, "sounds", sounds_list);
    cJSON_AddNumberToObject(ctx, "current_volume", get_current_volume());
    cJSON_AddNumberToObject(ctx, "sounds_poll_interval_seconds", any_playing ? 5 : 30);
}

http_response_t *view_sounds(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");
    const char *sound_title = request_get_form_field(req, "sound_title");

    if (action && sound_title && sound_title[0]) {
        cJSON *sounds = get_sounds_dict(true);
        persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);

        if (strcmp(action, "create_sound") == 0) {
            if (!cJSON_GetObjectItem(sounds, sound_title)) {
                cJSON *sound = cJSON_CreateObject();
                cJSON_AddNumberToObject(sound, "file", 1);
                cJSON_AddBoolToObject(sound, "high_quality", false);
                cJSON_AddBoolToObject(sound, "show_on_home", true);
                cJSON_AddNumberToObject(sound, "loop_count", 0);
                cJSON_AddNullToObject(sound, "chain_next");
                cJSON_AddItemToObject(sounds, sound_title, sound);
                persistent_dict_save(store);
            }
        } else if (strcmp(action, "delete_sound") == 0) {
            if (cJSON_GetObjectItem(sounds, sound_title)) {
                cJSON_DeleteItemFromObject(sounds, sound_title);
                persistent_dict_save(store);
            }
        }
    }

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "page_title", "Sounds");
    add_sounds_context(ctx, false, false);
    return webserver_render_response("setup/sounds.html", ctx);
}

http_response_t *view_sounds_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    add_sounds_context(ctx, false, false);
    return webserver_render_response("setup/sounds_summary.html", ctx);
}

http_response_t *view_sound_edit(http_request_t *req)
{
    cJSON *sounds = get_sounds_dict(false);

    if (req->method == HTTP_METHOD_GET) {
        const char *sound_title = request_get_query_param(req, "sound");
        cJSON *sound = (sound_title && sounds) ? cJSON_GetObjectItem(sounds, sound_title) : NULL;
        if (!sound_title || !sound) {
            return response_create(200, "text/html", "<p class=\"text-danger small\">Sound not found.</p>");
        }

        cJSON *ctx = build_global_context();
        cJSON_AddStringToObject(ctx, "sound_title", sound_title);
        cJSON_AddNumberToObject(ctx, "sound_file", json_get_int(sound, "file", 1));
        cJSON_AddBoolToObject(ctx, "sound_high_quality", json_get_bool(sound, "high_quality", false));
        cJSON_AddBoolToObject(ctx, "sound_show_on_home", json_get_bool(sound, "show_on_home", true));
        cJSON_AddNumberToObject(ctx, "sound_loop_count", json_get_int(sound, "loop_count", 0));
        const char *chain_next = json_get_string(sound, "chain_next", NULL);
        if (chain_next) cJSON_AddStringToObject(ctx, "sound_chain_next", chain_next);
        else cJSON_AddNullToObject(ctx, "sound_chain_next");

        int total = sounds ? cJSON_GetArraySize(sounds) : 0;
        const char **names = total > 0 ? calloc(total, sizeof(char *)) : NULL;
        int count = 0;
        if (names) {
            for (cJSON *item = sounds->child; item; item = item->next) names[count++] = item->string;
            qsort(names, count, sizeof(char *), cmp_str);
        }
        cJSON *available = cJSON_CreateArray();
        for (int i = 0; i < count; i++) cJSON_AddItemToArray(available, cJSON_CreateString(names[i]));
        free(names);
        cJSON_AddItemToObject(ctx, "available_sounds", available);

        return webserver_render_response("setup/sound_edit.html", ctx);
    }

    /* POST: update_sound */
    const char *action = request_get_form_field(req, "action");
    const char *old_sound_title = request_get_form_field(req, "old_sound_title");
    const char *sound_title = request_get_form_field(req, "sound_title");

    if (action && strcmp(action, "update_sound") == 0 && old_sound_title && old_sound_title[0] &&
        sound_title && sound_title[0] && sounds) {
        cJSON *sound = cJSON_GetObjectItem(sounds, old_sound_title);
        if (sound) {
            const char *file = request_get_form_field(req, "sound_file");
            const char *high_quality = request_get_form_field(req, "sound_high_quality");
            const char *show_on_home = request_get_form_field(req, "sound_show_on_home");
            const char *loop_count = request_get_form_field(req, "sound_loop_count");
            const char *chain_next = request_get_form_field(req, "sound_chain_next");

            cJSON_DeleteItemFromObject(sound, "file");
            cJSON_AddNumberToObject(sound, "file", file ? atoi(file) : 1);

            cJSON_DeleteItemFromObject(sound, "high_quality");
            cJSON_AddBoolToObject(sound, "high_quality", high_quality && strcmp(high_quality, "1") == 0);

            cJSON_DeleteItemFromObject(sound, "show_on_home");
            cJSON_AddBoolToObject(sound, "show_on_home", show_on_home && strcmp(show_on_home, "1") == 0);

            cJSON_DeleteItemFromObject(sound, "loop_count");
            cJSON_AddNumberToObject(sound, "loop_count", loop_count ? atoi(loop_count) : 0);

            cJSON_DeleteItemFromObject(sound, "chain_next");
            if (chain_next && chain_next[0]) cJSON_AddStringToObject(sound, "chain_next", chain_next);
            else cJSON_AddNullToObject(sound, "chain_next");

            if (strcmp(old_sound_title, sound_title) != 0) {
                if (!cJSON_GetObjectItem(sounds, sound_title)) {
                    cJSON *detached = cJSON_DetachItemFromObject(sounds, old_sound_title);
                    cJSON_AddItemToObject(sounds, sound_title, detached);
                }
                /* else: new title collides with an existing sound - keep old title,
                 * matching SoundEditView.post's silent no-rename fallback. */
            }

            persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
            persistent_dict_save(store);
        }
    }

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "page_title", "Sounds");
    add_sounds_context(ctx, false, false);
    return webserver_render_response("setup/sounds.html", ctx);
}

http_response_t *view_sounds_status(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    add_sounds_context(ctx, true, true);
    return webserver_render_response("sounds/buttons.html", ctx);
}

http_response_t *view_play_sound(http_request_t *req)
{
    const char *title = request_get_form_field(req, "title");
    if (!title || !title[0]) {
        return response_create(200, "text/html",
            "<div class=\"alert alert-danger small py-2 mb-0\">No sound title provided</div>");
    }

    cJSON *sound = get_sounds_dict(false);
    cJSON *sound_def = sound ? cJSON_GetObjectItem(sound, title) : NULL;
    int file_number = sound_def ? json_get_int(sound_def, "file", 0) : 0;

    esp_err_t err = sound_manager_play(title, 0);
    if (err == ESP_ERR_NOT_FOUND) {
        char buf[192];
        snprintf(buf, sizeof(buf), "<div class=\"alert alert-danger small py-2 mb-0\">Sound '%s' not found</div>", title);
        return response_create(200, "text/html", buf);
    }
    if (err != ESP_OK) {
        return response_create(200, "text/html",
            "<div class=\"alert alert-danger small py-2 mb-0\">All audio modules are busy</div>");
    }

    int module_idx = sound_manager_get_playing_module(title);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "<form hx-post=\"/sounds/stop\" hx-target=\"#sound-%d-result\" hx-swap=\"innerHTML\">"
        "<input type=\"hidden\" name=\"module_idx\" value=\"%d\">"
        "<input type=\"hidden\" name=\"title\" value=\"%s\">"
        "<input type=\"hidden\" name=\"file\" value=\"%d\">"
        "<button type=\"submit\" class=\"btn btn-sm btn-danger theme-sound-stop-btn\">&#9632; %s</button>"
        "</form>", file_number, module_idx, title, file_number, title);
    return response_create(200, "text/html", buf);
}

http_response_t *view_stop_sound(http_request_t *req)
{
    const char *title = request_get_form_field(req, "title");
    const char *file_str = request_get_form_field(req, "file");

    if (title && title[0]) {
        /* UI Stop uses the hardware-verified stop, matching StopSoundView ->
         * SoundManager.stop_sound() in Python; internal stops-lists keep the
         * fast sound_manager_stop(). */
        sound_manager_stop_verified(title);
    }

    int file_number = file_str ? atoi(file_str) : 0;
    char buf[384];
    snprintf(buf, sizeof(buf),
        "<form hx-post=\"/sounds/play\" hx-target=\"#sound-%d-result\" hx-swap=\"innerHTML\">"
        "<input type=\"hidden\" name=\"title\" value=\"%s\">"
        "<button type=\"submit\" class=\"btn btn-sm btn-outline-primary theme-sound-btn\">%s</button>"
        "</form>", file_number, title ? title : "", title ? title : "");
    return response_create(200, "text/html", buf);
}

http_response_t *view_stop_all_sounds(http_request_t *req)
{
    /* Mirrors StopAllSoundsView: stops audio player modules directly rather
     * than going through SoundManager (which also clears soundscape state) -
     * this is a deliberate asymmetry versus /soundscapes/stop in the Python
     * source. */
    audio_player_stop_all();

    cJSON *ctx = build_global_context();
    add_sounds_context(ctx, true, true);
    return webserver_render_response("sounds/buttons.html", ctx);
}

http_response_t *view_audio_volume(http_request_t *req)
{
    const char *volume_str = request_get_form_field(req, "master-volume");
    int volume = 20;
    if (volume_str) {
        volume = atoi(volume_str);
        if (volume < 0) volume = 0;
        if (volume > 30) volume = 30;
    }

    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    if (store) {
        persistent_dict_set(store, "master_volume", cJSON_CreateNumber(volume));
        persistent_dict_save(store);
    }

    audio_player_set_volume(volume);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d/30", volume);
    return response_create(200, "text/plain", buf);
}
