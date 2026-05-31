#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
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

http_response_t *view_scenes(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *s = cJSON_GetObjectItem(model, "scenes");
        if (s) cJSON_AddItemReferenceToObject(ctx, "scenes", s);
        cJSON *ss = cJSON_GetObjectItem(model, "scene_settings");
        if (ss) cJSON_AddItemReferenceToObject(ctx, "scene_settings", ss);
        cJSON *e = cJSON_GetObjectItem(model, "effects");
        if (e) cJSON_AddItemReferenceToObject(ctx, "effects", e);
    }
    return render("setup/scenes.html", ctx);
}

http_response_t *view_scene_edit(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    const char *action = request_get_form_field(req, "action");

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model || !name) return response_create(400, "text/plain", "Bad Request");

    cJSON *scenes = cJSON_GetObjectItem(model, "scenes");
    if (!scenes) { scenes = cJSON_CreateObject(); cJSON_AddItemToObject(model, "scenes", scenes); }

    cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
    if (!scene_settings) { scene_settings = cJSON_CreateObject(); cJSON_AddItemToObject(model, "scene_settings", scene_settings); }

    if (action && strcmp(action, "delete") == 0) {
        cJSON_DeleteItemFromObject(scenes, name);
        cJSON_DeleteItemFromObject(scene_settings, name);
        persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        cJSON_AddItemReferenceToObject(ctx, "scenes", scenes);
        cJSON_AddItemReferenceToObject(ctx, "scene_settings", scene_settings);
        return render("setup/scenes_summary.html", ctx);
    }

    if (action && strcmp(action, "save") == 0) {
        const char *new_name = request_get_form_field(req, "new_name");
        const char *sound = request_get_form_field(req, "sound");

        cJSON *meta = cJSON_GetObjectItem(scene_settings, name);
        if (!meta) { meta = cJSON_CreateObject(); cJSON_AddItemToObject(scene_settings, name, meta); }

        if (sound) { cJSON_DeleteItemFromObject(meta, "sound"); cJSON_AddStringToObject(meta, "sound", sound); }

        if (new_name && strlen(new_name) > 0 && strcmp(new_name, name) != 0) {
            cJSON *sd = cJSON_DetachItemFromObject(scenes, name);
            if (sd) cJSON_AddItemToObject(scenes, new_name, sd);
            cJSON *md = cJSON_DetachItemFromObject(scene_settings, name);
            if (md) cJSON_AddItemToObject(scene_settings, new_name, md);
        }
        persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        cJSON_AddItemReferenceToObject(ctx, "scenes", scenes);
        cJSON_AddItemReferenceToObject(ctx, "scene_settings", scene_settings);
        return render("setup/scenes_summary.html", ctx);
    }

    /* Show edit form */
    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "scene_name", name);
    cJSON *scene = cJSON_GetObjectItem(scenes, name);
    if (scene) cJSON_AddItemReferenceToObject(ctx, "scene", scene);
    cJSON *meta = cJSON_GetObjectItem(scene_settings, name);
    if (meta) cJSON_AddItemReferenceToObject(ctx, "scene_meta", meta);
    cJSON *effects = cJSON_GetObjectItem(model, "effects");
    if (effects) cJSON_AddItemReferenceToObject(ctx, "effects", effects);
    cJSON_AddItemReferenceToObject(ctx, "all_scenes", scenes);
    return render("setup/scene_edit.html", ctx);
}

http_response_t *view_scene_edit_add_trigger(http_request_t *req)
{
    const char *scene_name = request_get_form_field(req, "scene_name");
    const char *trigger_scene = request_get_form_field(req, "trigger_scene");
    if (!scene_name || !trigger_scene) return response_create(400, "text/plain", "Bad Request");

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model) return response_create(500, "text/plain", "Error");

    cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
    if (!scene_settings) { scene_settings = cJSON_CreateObject(); cJSON_AddItemToObject(model, "scene_settings", scene_settings); }

    cJSON *meta = cJSON_GetObjectItem(scene_settings, scene_name);
    if (!meta) { meta = cJSON_CreateObject(); cJSON_AddItemToObject(scene_settings, scene_name, meta); }

    cJSON *triggers = cJSON_GetObjectItem(meta, "trigger_scenes_on_completion");
    if (!triggers) { triggers = cJSON_CreateArray(); cJSON_AddItemToObject(meta, "trigger_scenes_on_completion", triggers); }

    cJSON_AddItemToArray(triggers, cJSON_CreateString(trigger_scene));
    persistent_dict_save(store);

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "scene_name", scene_name);
    cJSON_AddItemReferenceToObject(ctx, "scene_meta", meta);
    cJSON *scenes = cJSON_GetObjectItem(model, "scenes");
    if (scenes) cJSON_AddItemReferenceToObject(ctx, "all_scenes", scenes);
    return render("setup/scene_edit_trigger_control.html", ctx);
}

http_response_t *view_scene_edit_remove_trigger(http_request_t *req)
{
    const char *scene_name = request_get_form_field(req, "scene_name");
    const char *trigger_scene = request_get_form_field(req, "trigger_scene");
    if (!scene_name || !trigger_scene) return response_create(400, "text/plain", "Bad Request");

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model) return response_create(500, "text/plain", "Error");

    cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
    if (scene_settings) {
        cJSON *meta = cJSON_GetObjectItem(scene_settings, scene_name);
        if (meta) {
            cJSON *triggers = cJSON_GetObjectItem(meta, "trigger_scenes_on_completion");
            if (triggers && cJSON_IsArray(triggers)) {
                int size = cJSON_GetArraySize(triggers);
                for (int i = 0; i < size; i++) {
                    cJSON *item = cJSON_GetArrayItem(triggers, i);
                    if (item && item->valuestring && strcmp(item->valuestring, trigger_scene) == 0) {
                        cJSON_DeleteItemFromArray(triggers, i);
                        break;
                    }
                }
            }
        }
        persistent_dict_save(store);
    }

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "scene_name", scene_name);
    cJSON *meta = scene_settings ? cJSON_GetObjectItem(scene_settings, scene_name) : NULL;
    if (meta) cJSON_AddItemReferenceToObject(ctx, "scene_meta", meta);
    cJSON *scenes = cJSON_GetObjectItem(model, "scenes");
    if (scenes) cJSON_AddItemReferenceToObject(ctx, "all_scenes", scenes);
    return render("setup/scene_edit_trigger_control.html", ctx);
}

http_response_t *view_scenes_color_select(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *cc = cJSON_GetObjectItem(model, "custom_colors");
        if (cc) cJSON_AddItemReferenceToObject(ctx, "custom_colors", cc);
    }
    const char *field = request_get_query_param(req, "field");
    if (field) cJSON_AddStringToObject(ctx, "field", field);
    return render("setup/color_select.html", ctx);
}

http_response_t *view_scenes_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *s = cJSON_GetObjectItem(model, "scenes");
        if (s) cJSON_AddItemReferenceToObject(ctx, "scenes", s);
        cJSON *ss = cJSON_GetObjectItem(model, "scene_settings");
        if (ss) cJSON_AddItemReferenceToObject(ctx, "scene_settings", ss);
    }
    return render("setup/scenes_summary.html", ctx);
}
