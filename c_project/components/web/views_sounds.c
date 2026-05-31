#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "sound_manager.h"
#include "audio_player.h"
#include "lighting.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static http_response_t *render(const char *tpl, cJSON *ctx)
{
    char *html = template_render_file(tpl, ctx);
    cJSON_Delete(ctx);
    if (!html) return response_create(500, "text/plain", "Template error");
    http_response_t *res = response_create(200, "text/html", html);
    free(html);
    return res;
}

http_response_t *view_sounds(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *s = cJSON_GetObjectItem(model, "sounds");
        if (s) cJSON_AddItemReferenceToObject(ctx, "sounds", s);
    }
    cJSON_AddNumberToObject(ctx, "volume", audio_player_get_volume());
    return render("setup/sounds.html", ctx);
}

http_response_t *view_sounds_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *s = cJSON_GetObjectItem(model, "sounds");
        if (s) cJSON_AddItemReferenceToObject(ctx, "sounds", s);
    }
    return render("setup/sounds_summary.html", ctx);
}

http_response_t *view_sound_edit(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    const char *action = request_get_form_field(req, "action");

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model || !name) return response_create(400, "text/plain", "Bad Request");

    cJSON *sounds = cJSON_GetObjectItem(model, "sounds");
    if (!sounds) { sounds = cJSON_CreateObject(); cJSON_AddItemToObject(model, "sounds", sounds); }

    if (action && strcmp(action, "delete") == 0) {
        cJSON_DeleteItemFromObject(sounds, name);
        persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        cJSON_AddItemReferenceToObject(ctx, "sounds", sounds);
        return render("setup/sounds_summary.html", ctx);
    }

    if (action && strcmp(action, "save") == 0) {
        const char *new_name = request_get_form_field(req, "new_name");
        const char *file = request_get_form_field(req, "file");
        const char *loop_count = request_get_form_field(req, "loop_count");
        const char *high_quality = request_get_form_field(req, "high_quality");

        cJSON *sound = cJSON_GetObjectItem(sounds, name);
        if (!sound) { sound = cJSON_CreateObject(); cJSON_AddItemToObject(sounds, name, sound); }

        if (file) { cJSON_DeleteItemFromObject(sound, "file"); cJSON_AddNumberToObject(sound, "file", atoi(file)); }
        if (loop_count) { cJSON_DeleteItemFromObject(sound, "loop_count"); cJSON_AddNumberToObject(sound, "loop_count", atoi(loop_count)); }
        cJSON_DeleteItemFromObject(sound, "high_quality");
        cJSON_AddBoolToObject(sound, "high_quality", high_quality != NULL && strcmp(high_quality, "on") == 0);

        if (new_name && strlen(new_name) > 0 && strcmp(new_name, name) != 0) {
            cJSON *data = cJSON_DetachItemFromObject(sounds, name);
            if (data) cJSON_AddItemToObject(sounds, new_name, data);
        }
        persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        cJSON_AddItemReferenceToObject(ctx, "sounds", sounds);
        return render("setup/sounds_summary.html", ctx);
    }

    /* Show edit form */
    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "sound_name", name);
    cJSON *sound = cJSON_GetObjectItem(sounds, name);
    if (sound) cJSON_AddItemReferenceToObject(ctx, "sound", sound);
    cJSON_AddItemReferenceToObject(ctx, "all_sounds", sounds);
    return render("setup/sound_edit.html", ctx);
}

http_response_t *view_sounds_status(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *playing = sound_manager_get_playing();
    cJSON_AddItemToObject(ctx, "playing_sounds", playing);
    cJSON_AddNumberToObject(ctx, "volume", audio_player_get_volume());
    return render("sounds/buttons.html", ctx);
}

http_response_t *view_play_sound(http_request_t *req)
{
    const char *title = request_get_form_field(req, "title");
    if (title) sound_manager_play(title, 0);
    return view_sounds_status(req);
}

http_response_t *view_stop_sound(http_request_t *req)
{
    const char *title = request_get_form_field(req, "title");
    if (title) sound_manager_stop(title);
    return view_sounds_status(req);
}

http_response_t *view_stop_all_sounds(http_request_t *req)
{
    sound_manager_stop_all();
    return view_sounds_status(req);
}

http_response_t *view_audio_volume(http_request_t *req)
{
    const char *volume_str = request_get_form_field(req, "volume");
    if (volume_str) audio_player_set_volume(atoi(volume_str));

    cJSON *ctx = build_global_context();
    cJSON_AddNumberToObject(ctx, "volume", audio_player_get_volume());
    return render("sounds/volume_control.html", ctx);
}
