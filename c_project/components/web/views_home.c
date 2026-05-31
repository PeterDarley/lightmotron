#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "animation.h"
#include "sound_manager.h"
#include "wifi_manager.h"
#include "comms.h"
#include "timing.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/**
 * Helper: render a template with context and return an HTML response.
 *
 * Takes ownership of ctx (frees it after rendering).
 */
static http_response_t *render(const char *template_file, cJSON *ctx)
{
    char *html = template_render_file(template_file, ctx);
    cJSON_Delete(ctx);

    if (!html) {
        return response_create(500, "text/plain", "Template render error");
    }

    http_response_t *res = response_create(200, "text/html", html);
    free(html);
    return res;
}

http_response_t *view_home(http_request_t *req)
{
    cJSON *ctx = build_global_context();

    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *scenes = cJSON_GetObjectItem(model, "scenes");
        if (scenes) {
            cJSON_AddItemReferenceToObject(ctx, "scenes", scenes);
        }

        cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
        if (scene_settings) {
            cJSON_AddItemReferenceToObject(ctx, "scene_settings", scene_settings);
        }
    }

    /* Active scenes */
    char active_names[8][64];
    int active_count = lighting_get_active_scenes(active_names, 8);
    cJSON *active_arr = cJSON_CreateArray();
    for (int i = 0; i < active_count; i++) {
        cJSON_AddItemToArray(active_arr, cJSON_CreateString(active_names[i]));
    }
    cJSON_AddItemToObject(ctx, "active_scenes", active_arr);

    return render("home.html", ctx);
}

http_response_t *view_set_scene(http_request_t *req)
{
    const char *scene = request_get_form_field(req, "scene");
    const char *action = request_get_form_field(req, "action");

    if (scene) {
        if (action && strcmp(action, "add") == 0) {
            lighting_add_scene(scene);
        } else if (action && strcmp(action, "remove") == 0) {
            lighting_remove_scene(scene);
        } else {
            lighting_set_scene(scene);
        }
    }

    /* Return scene panel partial */
    cJSON *ctx = build_global_context();

    char active_names[8][64];
    int active_count = lighting_get_active_scenes(active_names, 8);
    cJSON *active_arr = cJSON_CreateArray();
    for (int i = 0; i < active_count; i++) {
        cJSON_AddItemToArray(active_arr, cJSON_CreateString(active_names[i]));
    }
    cJSON_AddItemToObject(ctx, "active_scenes", active_arr);

    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *scenes = cJSON_GetObjectItem(model, "scenes");
        if (scenes) cJSON_AddItemReferenceToObject(ctx, "scenes", scenes);

        cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
        if (scene_settings) cJSON_AddItemReferenceToObject(ctx, "scene_settings", scene_settings);
    }

    return render("scenes/scene_panel.html", ctx);
}

http_response_t *view_scene_panel_status(http_request_t *req)
{
    cJSON *ctx = build_global_context();

    char active_names[8][64];
    int active_count = lighting_get_active_scenes(active_names, 8);
    cJSON *active_arr = cJSON_CreateArray();
    for (int i = 0; i < active_count; i++) {
        cJSON_AddItemToArray(active_arr, cJSON_CreateString(active_names[i]));
    }
    cJSON_AddItemToObject(ctx, "active_scenes", active_arr);

    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *scenes = cJSON_GetObjectItem(model, "scenes");
        if (scenes) cJSON_AddItemReferenceToObject(ctx, "scenes", scenes);

        cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
        if (scene_settings) cJSON_AddItemReferenceToObject(ctx, "scene_settings", scene_settings);
    }

    return render("scenes/scene_panel.html", ctx);
}

http_response_t *view_animation(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");

    if (action) {
        if (strcmp(action, "start") == 0) animation_start();
        else if (strcmp(action, "stop") == 0) animation_stop();
        else if (strcmp(action, "pause") == 0) animation_pause();
        else if (strcmp(action, "resume") == 0) animation_resume();
    }

    cJSON *ctx = build_global_context();
    cJSON_AddBoolToObject(ctx, "animation_running", animation_is_running());
    cJSON_AddBoolToObject(ctx, "animation_paused", animation_is_paused());

    return render("animation/buttons.html", ctx);
}

http_response_t *view_storage(http_request_t *req)
{
    cJSON *ctx = build_global_context();

    size_t free_heap = esp_get_free_heap_size();
    char heap_str[32];
    snprintf(heap_str, sizeof(heap_str), "%u KB", (unsigned)(free_heap / 1024));
    cJSON_AddStringToObject(ctx, "free_heap", heap_str);

    return render("storage.html", ctx);
}

http_response_t *view_status(http_request_t *req)
{
    cJSON *ctx = build_global_context();

    cJSON_AddStringToObject(ctx, "wifi_status",
                            wifi_manager_is_connected() ? "connected" : "disconnected");
    cJSON_AddStringToObject(ctx, "ip_address", wifi_manager_get_ip());

    uint32_t uptime = timing_uptime_seconds();
    char uptime_str[32];
    snprintf(uptime_str, sizeof(uptime_str), "%uh %um %us",
             (unsigned)(uptime / 3600),
             (unsigned)((uptime % 3600) / 60),
             (unsigned)(uptime % 60));
    cJSON_AddStringToObject(ctx, "uptime", uptime_str);
    cJSON_AddBoolToObject(ctx, "animation_running", animation_is_running());

    return render("status.html", ctx);
}

http_response_t *view_setup(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "current_model", lighting_get_current_model());
    return render("setup.html", ctx);
}

http_response_t *view_confirm(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");
    const char *target = request_get_form_field(req, "target");

    cJSON *ctx = build_global_context();
    if (action) cJSON_AddStringToObject(ctx, "action", action);
    if (target) cJSON_AddStringToObject(ctx, "target", target);

    return render("setup/confirm_delete.html", ctx);
}
