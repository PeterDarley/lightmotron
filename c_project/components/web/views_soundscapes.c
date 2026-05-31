#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "sound_manager.h"
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

http_response_t *view_soundscapes(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *ss = cJSON_GetObjectItem(model, "soundscapes");
        if (ss) cJSON_AddItemReferenceToObject(ctx, "soundscapes", ss);
        cJSON *s = cJSON_GetObjectItem(model, "sounds");
        if (s) cJSON_AddItemReferenceToObject(ctx, "sounds", s);
    }
    return render("setup/soundscapes.html", ctx);
}

http_response_t *view_soundscapes_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *ss = cJSON_GetObjectItem(model, "soundscapes");
        if (ss) cJSON_AddItemReferenceToObject(ctx, "soundscapes", ss);
    }
    return render("setup/soundscapes_summary.html", ctx);
}

http_response_t *view_soundscapes_status(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *playing = sound_manager_get_playing();
    cJSON_AddItemToObject(ctx, "playing_sounds", playing);
    return render("soundscapes/buttons.html", ctx);
}

http_response_t *view_soundscape_edit(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    const char *action = request_get_form_field(req, "action");

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model || !name) return response_create(400, "text/plain", "Bad Request");

    cJSON *soundscapes = cJSON_GetObjectItem(model, "soundscapes");
    if (!soundscapes) { soundscapes = cJSON_CreateObject(); cJSON_AddItemToObject(model, "soundscapes", soundscapes); }

    if (action && strcmp(action, "delete") == 0) {
        cJSON_DeleteItemFromObject(soundscapes, name);
        persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        cJSON_AddItemReferenceToObject(ctx, "soundscapes", soundscapes);
        return render("setup/soundscapes_summary.html", ctx);
    }

    if (action && strcmp(action, "save") == 0) {
        const char *new_name = request_get_form_field(req, "new_name");

        cJSON *scape = cJSON_GetObjectItem(soundscapes, name);
        if (!scape) { scape = cJSON_CreateObject(); cJSON_AddItemToObject(soundscapes, name, scape); }

        if (new_name && strlen(new_name) > 0 && strcmp(new_name, name) != 0) {
            cJSON *data = cJSON_DetachItemFromObject(soundscapes, name);
            if (data) cJSON_AddItemToObject(soundscapes, new_name, data);
        }
        persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        cJSON_AddItemReferenceToObject(ctx, "soundscapes", soundscapes);
        return render("setup/soundscapes_summary.html", ctx);
    }

    /* Show edit form */
    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "soundscape_name", name);
    cJSON *scape = cJSON_GetObjectItem(soundscapes, name);
    if (scape) cJSON_AddItemReferenceToObject(ctx, "soundscape", scape);
    cJSON *sounds = cJSON_GetObjectItem(model, "sounds");
    if (sounds) cJSON_AddItemReferenceToObject(ctx, "sounds", sounds);
    return render("setup/soundscape_edit.html", ctx);
}

http_response_t *view_play_soundscape(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    if (name) sound_manager_play(name, 0);
    return view_soundscapes_status(req);
}

http_response_t *view_stop_soundscape(http_request_t *req)
{
    sound_manager_stop_all();
    return view_soundscapes_status(req);
}
