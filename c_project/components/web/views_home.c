#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "animation.h"
#include "sound_manager.h"
#include "comms.h"
#include "json_helpers.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

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

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ---------------------------------------------------------------------
 * Scenes context - mirrors _scenes_context() in web/views.py.
 * ------------------------------------------------------------------- */

/* Returns true if the scene has at least one entry without an explicit
 * cycle limit (a non-terminating effect) - mirrors
 * Lighting.is_scene_ongoing(). An entry's cycle limit may come from the
 * scene entry itself ("cycles") or, failing that, from the resolved
 * effect's own "cycles" field. Empty/malformed scenes are ongoing by
 * default, matching the Python docstring "preserve previous behavior". */
static bool scene_is_ongoing(cJSON *scene_entries, cJSON *effects_dict)
{
    if (!scene_entries) return true;

    bool has_cycle_limited = false;
    for (cJSON *entry = scene_entries->child; entry; entry = entry->next) {
        if (!cJSON_IsObject(entry)) continue;

        cJSON *effect_name_item = cJSON_GetObjectItem(entry, "effect");
        cJSON *cycles_item = cJSON_GetObjectItem(entry, "cycles");
        bool has_cycles;

        if (effect_name_item && cJSON_IsString(effect_name_item)) {
            cJSON *effect_def =
                effects_dict ? cJSON_GetObjectItem(effects_dict, effect_name_item->valuestring) : NULL;
            if (!effect_def || !cJSON_GetObjectItem(effect_def, "pattern")) {
                continue; /* _resolve_effect() would return {} - skipped, like Python's `if not effect...: continue` */
            }
            has_cycles = (cycles_item != NULL) || (cJSON_GetObjectItem(effect_def, "cycles") != NULL);
        } else {
            if (!cJSON_GetObjectItem(entry, "pattern")) continue;
            has_cycles = (cycles_item != NULL);
        }

        if (!has_cycles) return true;
        has_cycle_limited = true;
    }

    return !has_cycle_limited;
}

/* Builds the scenes / active-scenes / ongoing / immediate context shared by
 * the home page and the scene-panel HTMX fragment. Mirrors _scenes_context(). */
static void add_scenes_context(cJSON *ctx)
{
    cJSON *model = lighting_get_settings();
    cJSON *scenes_dict = model ? cJSON_GetObjectItem(model, "scenes") : NULL;
    cJSON *effects_dict = model ? cJSON_GetObjectItem(model, "effects") : NULL;

    int total = scenes_dict ? cJSON_GetArraySize(scenes_dict) : 0;
    const char **names = total > 0 ? calloc((size_t)total, sizeof(char *)) : NULL;
    if (names) {
        int i = 0;
        for (cJSON *item = scenes_dict->child; item; item = item->next) {
            names[i++] = item->string;
        }
        qsort(names, (size_t)total, sizeof(char *), cmp_str);
    }

    cJSON *scenes_arr = cJSON_CreateArray();
    cJSON *ongoing_arr = cJSON_CreateArray();
    cJSON *immediate_arr = cJSON_CreateArray();
    for (int i = 0; i < total; i++) {
        cJSON_AddItemToArray(scenes_arr, cJSON_CreateString(names[i]));
        cJSON *entries = cJSON_GetObjectItem(scenes_dict, names[i]);
        if (scene_is_ongoing(entries, effects_dict)) {
            cJSON_AddItemToArray(ongoing_arr, cJSON_CreateString(names[i]));
        } else {
            cJSON_AddItemToArray(immediate_arr, cJSON_CreateString(names[i]));
        }
    }
    cJSON_AddItemToObject(ctx, "scenes", scenes_arr);
    cJSON_AddItemToObject(ctx, "ongoing_scenes", ongoing_arr);
    cJSON_AddItemToObject(ctx, "immediate_scenes", immediate_arr);
    free(names);

    /* current_scene mirrors lights.scene_name (`_active_scenes[-1]` or ""),
     * independent of whether that scene still exists. active_scenes (and
     * its label) are filtered down to names that still exist, matching
     * `[name for name in lights._active_scenes if name in scene_names]`. */
    char active_names[MAX_ACTIVE_SCENES][64];
    int active_raw_count = lighting_get_active_scenes(active_names, MAX_ACTIVE_SCENES);
    const char *current_scene = (active_raw_count > 0) ? active_names[active_raw_count - 1] : "";

    cJSON *active_arr = cJSON_CreateArray();
    char label[512] = "";
    for (int i = 0; i < active_raw_count; i++) {
        if (!scenes_dict || !cJSON_GetObjectItem(scenes_dict, active_names[i])) continue;
        cJSON_AddItemToArray(active_arr, cJSON_CreateString(active_names[i]));
        if (label[0] != '\0') {
            strncat(label, ", ", sizeof(label) - strlen(label) - 1);
        }
        strncat(label, active_names[i], sizeof(label) - strlen(label) - 1);
    }
    cJSON_AddItemToObject(ctx, "active_scenes", active_arr);
    /* "\xE2\x80\x94" is the UTF-8 encoding of U+2014 EM DASH, matching Python's "—". */
    cJSON_AddStringToObject(ctx, "active_scenes_label", label[0] != '\0' ? label : "\xE2\x80\x94");
    cJSON_AddStringToObject(ctx, "current_scene", current_scene);
}

/* Mirrors _animation_context(). Note: the /animation route only supports
 * "start"/"stop" in current Python (AnimationView.post() 400s on anything
 * else) - pause/resume are not exposed through this endpoint. */
static void add_animation_context(cJSON *ctx)
{
    bool running = animation_is_running();
    cJSON_AddBoolToObject(ctx, "animation_running", running);
    cJSON_AddBoolToObject(ctx, "animation_stopped", !running);
}

http_response_t *view_home(http_request_t *req)
{
    (void)req;
    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "message", "Lighting");
    cJSON_AddStringToObject(ctx, "page_title", "Home");
    add_scenes_context(ctx);
    add_animation_context(ctx);
    /* Mirrors HomeView.get()'s context.update(_soundscapes_context(include_active=True))
     * for the soundscapes panel included by home.html. */
    add_soundscapes_context(ctx, true);
    return render("home.html", ctx);
}

http_response_t *view_set_scene(http_request_t *req)
{
    const char *scene = request_get_form_field(req, "scene");
    const char *action = request_get_form_field(req, "action");
    if (!action) action = "set";

    cJSON *model = lighting_get_settings();
    cJSON *scenes_dict = model ? cJSON_GetObjectItem(model, "scenes") : NULL;

    if (!scene || !scenes_dict || !cJSON_GetObjectItem(scenes_dict, scene)) {
        return response_create(400, "text/plain", "Invalid scene");
    }

    if (strcmp(action, "add") == 0) {
        lighting_add_scene(scene);
    } else if (strcmp(action, "remove") == 0) {
        lighting_remove_scene(scene);
    } else {
        /* Known gap: Python's "set" branch also forwards any extra form
         * fields as lights.set_scene(scene_name, **extra_kwargs); the C
         * lighting_set_scene() API takes no such passthrough parameters. */
        lighting_set_scene(scene);
    }

    cJSON *ctx = build_global_context();
    add_scenes_context(ctx);
    return render("scenes/scene_panel.html", ctx);
}

http_response_t *view_scene_panel_status(http_request_t *req)
{
    (void)req;
    cJSON *ctx = build_global_context();
    add_scenes_context(ctx);
    return render("scenes/scene_panel.html", ctx);
}

http_response_t *view_animation(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");

    if (action && strcmp(action, "start") == 0) {
        animation_start();
    } else if (action && strcmp(action, "stop") == 0) {
        animation_stop();
    } else {
        return response_create(400, "text/plain", "Invalid action");
    }

    cJSON *ctx = build_global_context();
    add_animation_context(ctx);
    return render("animation/buttons.html", ctx);
}

/* view_storage / view_status / view_setup / view_confirm moved to
 * views_system.c (system-level pages, matching how views.h already
 * groups them separately from the home views above). */
