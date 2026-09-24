#include "routes.h"
#include "views.h"
#include "webserver.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/*
 * Live refresh of the Setup page's summary cards.
 *
 * The cards behind the Setup modal (Filters, Custom Colors, ...) used to
 * refresh only when the modal was closed, so adding/deleting something looked
 * like it hadn't worked until a page reload. Successful POSTs to these
 * sections now carry an `HX-Trigger: {"summaryRefresh":"<section>"}` header;
 * templates/setup.html listens for that event and re-fetches just that
 * section's /<section>/summary card. Only POST registrations are wrapped
 * (GETs just render), and responses that redirect are left alone.
 */
static http_response_t *with_summary_refresh(http_response_t *res, http_request_t *req, const char *section)
{
    if (!res || req->method != HTTP_METHOD_POST || res->status_code != 200) return res;
    if (res->headers && cJSON_GetObjectItem(res->headers, "HX-Redirect")) return res;

    char value[64];
    snprintf(value, sizeof(value), "{\"summaryRefresh\":\"%s\"}", section);
    if (!res->headers) res->headers = cJSON_CreateObject();
    cJSON_DeleteItemFromObject(res->headers, "HX-Trigger");
    cJSON_AddStringToObject(res->headers, "HX-Trigger", value);
    return res;
}

#define REFRESH_ROUTE(fn, section)                                        \
    static http_response_t *fn##_refresh(http_request_t *req)             \
    {                                                                     \
        return with_summary_refresh(fn(req), req, section);               \
    }

REFRESH_ROUTE(view_models, "models")
REFRESH_ROUTE(view_named_range, "named_range")
REFRESH_ROUTE(view_named_range_set, "named_range")
REFRESH_ROUTE(view_named_range_remove_subrange, "named_range")
REFRESH_ROUTE(view_named_range_reorder, "named_range")
REFRESH_ROUTE(view_custom_colors, "custom_colors")
REFRESH_ROUTE(view_scenes, "scenes")
REFRESH_ROUTE(view_scene_edit, "scenes")
REFRESH_ROUTE(view_scene_edit_add_trigger, "scenes")
REFRESH_ROUTE(view_scene_edit_remove_trigger, "scenes")
REFRESH_ROUTE(view_effects, "effects")
REFRESH_ROUTE(view_effect_edit, "effects")
REFRESH_ROUTE(view_filters_post, "filters")
REFRESH_ROUTE(view_filter_edit, "filters")
REFRESH_ROUTE(view_sounds, "sounds")
REFRESH_ROUTE(view_sound_edit, "sounds")
REFRESH_ROUTE(view_soundscapes, "soundscapes")
REFRESH_ROUTE(view_soundscape_edit, "soundscapes")
REFRESH_ROUTE(view_system_settings, "system_settings")

/* The Updates card only shows the repository, and refreshing it triggers a
 * GitHub release check, so only refresh it when the repository was actually
 * changed -- not after every "Check Now"/"Install" POST. */
static http_response_t *view_updates_refresh(http_request_t *req)
{
    http_response_t *res = view_updates(req);
    const char *action = request_get_form_field(req, "action");
    if (action && strcmp(action, "save_repo") == 0) {
        res = with_summary_refresh(res, req, "updates");
    }
    return res;
}

/*
 * Route table. Mirrors web/routes.py's web_server.add_routes({...}) dict:
 * each Python entry maps ONE path to a View class that may implement get()
 * and/or post(); the C side has to register each (method, path) pair
 * explicitly, so a Python view with both get() and post() needs two
 * webserver_add_route() calls here pointing at the same handler (the
 * handler itself branches on req->method, same as views_filter_edit()/
 * views_scene_edit()/view_effect_edit() already do).
 */
void routes_register_all(void)
{
    /* Home & general */
    webserver_add_route(HTTP_METHOD_GET, "/", view_home);
    webserver_add_route(HTTP_METHOD_POST, "/set_scene", view_set_scene);
    webserver_add_route(HTTP_METHOD_GET, "/scenes/panel/status", view_scene_panel_status);
    webserver_add_route(HTTP_METHOD_POST, "/animation", view_animation);
    webserver_add_route(HTTP_METHOD_GET, "/storage", view_storage);
    webserver_add_route(HTTP_METHOD_GET, "/setup", view_setup);
    webserver_add_route(HTTP_METHOD_GET, "/status", view_status);
    webserver_add_route(HTTP_METHOD_POST, "/confirm", view_confirm);

    /* Models. ModelsView has both get() and post() in Python. */
    webserver_add_route(HTTP_METHOD_GET, "/models", view_models);
    webserver_add_route(HTTP_METHOD_POST, "/models", view_models_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/models/summary", view_models_summary);
    webserver_add_route(HTTP_METHOD_POST, "/models/set", view_models_set);
    webserver_add_route(HTTP_METHOD_POST, "/models/wrap", view_models_wrap);

    /* Named ranges. NamedRangeView has both get() and post() in Python. */
    webserver_add_route(HTTP_METHOD_GET, "/named_range", view_named_range);
    webserver_add_route(HTTP_METHOD_POST, "/named_range", view_named_range_refresh);
    webserver_add_route(HTTP_METHOD_POST, "/named_range/set", view_named_range_set_refresh);
    webserver_add_route(HTTP_METHOD_POST, "/named_range/remove_subrange", view_named_range_remove_subrange_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/named_range/summary", view_named_range_summary);
    webserver_add_route(HTTP_METHOD_GET, "/named_range/reorder", view_named_range_reorder);
    webserver_add_route(HTTP_METHOD_POST, "/named_range/reorder", view_named_range_reorder_refresh);

    /* Custom colors. CustomColorsView has both get() and post() in Python. */
    webserver_add_route(HTTP_METHOD_GET, "/custom_colors", view_custom_colors);
    webserver_add_route(HTTP_METHOD_POST, "/custom_colors", view_custom_colors_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/custom_colors/summary", view_custom_colors_summary);

    /* Scenes. GET/POST /scenes and /scenes/edit are each handled by a single
     * view function that branches on req->method, mirroring the Python side
     * where one View class implements both get() and post(). */
    webserver_add_route(HTTP_METHOD_GET, "/scenes", view_scenes);
    webserver_add_route(HTTP_METHOD_POST, "/scenes", view_scenes_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/scenes/edit", view_scene_edit);
    webserver_add_route(HTTP_METHOD_POST, "/scenes/edit", view_scene_edit_refresh);
    webserver_add_route(HTTP_METHOD_POST, "/scenes/edit/add_trigger_scene", view_scene_edit_add_trigger_refresh);
    webserver_add_route(HTTP_METHOD_POST, "/scenes/edit/remove_trigger_scene", view_scene_edit_remove_trigger_refresh);
    /* ColorSelectView.post() only - Python never registers a GET handler for it. */
    webserver_add_route(HTTP_METHOD_POST, "/scenes/color_select", view_scenes_color_select);
    webserver_add_route(HTTP_METHOD_GET, "/scenes/summary", view_scenes_summary);

    /* Effects */
    webserver_add_route(HTTP_METHOD_GET, "/effects", view_effects);
    webserver_add_route(HTTP_METHOD_POST, "/effects", view_effects_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/effects/edit", view_effect_edit);
    webserver_add_route(HTTP_METHOD_POST, "/effects/edit", view_effect_edit_refresh);
    webserver_add_route(HTTP_METHOD_POST, "/effects/color_select", view_effects_color_select);
    webserver_add_route(HTTP_METHOD_GET, "/effects/summary", view_effects_summary);

    /* Filters. view_filters (GET-only listing) and view_filters_post
     * (create/delete) mirror FiltersView.get()/post(); view_filter_edit
     * handles both methods itself, like view_scene_edit/view_effect_edit. */
    webserver_add_route(HTTP_METHOD_GET, "/filters", view_filters);
    webserver_add_route(HTTP_METHOD_POST, "/filters", view_filters_post_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/filters/edit", view_filter_edit);
    webserver_add_route(HTTP_METHOD_POST, "/filters/edit", view_filter_edit_refresh);
    webserver_add_route(HTTP_METHOD_POST, "/filters/color_select", view_filter_color_select);
    webserver_add_route(HTTP_METHOD_GET, "/filters/summary", view_filters_summary);

    /* Sounds. SoundsView and SoundEditView each have both get() and post(). */
    webserver_add_route(HTTP_METHOD_GET, "/sounds", view_sounds);
    webserver_add_route(HTTP_METHOD_POST, "/sounds", view_sounds_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/sounds/summary", view_sounds_summary);
    webserver_add_route(HTTP_METHOD_GET, "/sounds/edit", view_sound_edit);
    webserver_add_route(HTTP_METHOD_POST, "/sounds/edit", view_sound_edit_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/sounds/status", view_sounds_status);
    webserver_add_route(HTTP_METHOD_POST, "/sounds/play", view_play_sound);
    webserver_add_route(HTTP_METHOD_POST, "/sounds/stop", view_stop_sound);
    webserver_add_route(HTTP_METHOD_POST, "/sounds/stop-all", view_stop_all_sounds);
    webserver_add_route(HTTP_METHOD_POST, "/audio/volume", view_audio_volume);

    /* Soundscapes. SoundscapesView and SoundscapeEditView each have both
     * get() and post(). */
    webserver_add_route(HTTP_METHOD_GET, "/soundscapes", view_soundscapes);
    webserver_add_route(HTTP_METHOD_POST, "/soundscapes", view_soundscapes_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/soundscapes/summary", view_soundscapes_summary);
    webserver_add_route(HTTP_METHOD_GET, "/soundscapes/status", view_soundscapes_status);
    webserver_add_route(HTTP_METHOD_GET, "/soundscapes/edit", view_soundscape_edit);
    webserver_add_route(HTTP_METHOD_POST, "/soundscapes/edit", view_soundscape_edit_refresh);
    webserver_add_route(HTTP_METHOD_POST, "/soundscapes/play", view_play_soundscape);
    webserver_add_route(HTTP_METHOD_POST, "/soundscapes/stop", view_stop_soundscape);

    /* System. Several of these are POST-only in Python (RestoreConfirmView,
     * SystemRebootConfirmView) even though they render a confirmation
     * fragment rather than performing the action - registered as POST here
     * to match, not GET. ThemeView/SystemSettingsView have both get() and
     * post(). */
    webserver_add_route(HTTP_METHOD_GET, "/backup", view_backup);
    webserver_add_route(HTTP_METHOD_POST, "/restore", view_restore);
    webserver_add_route(HTTP_METHOD_POST, "/restore/confirm", view_restore_confirm);
    webserver_add_route(HTTP_METHOD_GET, "/theme", view_theme);
    webserver_add_route(HTTP_METHOD_POST, "/theme", view_theme);
    webserver_add_route(HTTP_METHOD_POST, "/theme/delete", view_theme_delete);
    webserver_add_route(HTTP_METHOD_POST, "/theme/upload", view_theme_upload);
    webserver_add_route(HTTP_METHOD_GET, "/hostname", view_hostname);
    webserver_add_route(HTTP_METHOD_POST, "/hostname", view_hostname);
    webserver_add_route(HTTP_METHOD_GET, "/system_settings", view_system_settings);
    webserver_add_route(HTTP_METHOD_POST, "/system_settings", view_system_settings_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/system_settings/summary", view_system_settings_summary);
    webserver_add_route(HTTP_METHOD_POST, "/system_settings/ip_announced", view_system_settings_ip_announced);
    webserver_add_route(HTTP_METHOD_POST, "/system_reboot", view_system_reboot);
    webserver_add_route(HTTP_METHOD_POST, "/system_reboot/confirm", view_system_reboot_confirm);
    webserver_add_route(HTTP_METHOD_GET, "/updates", view_updates);
    webserver_add_route(HTTP_METHOD_POST, "/updates", view_updates_refresh);
    webserver_add_route(HTTP_METHOD_GET, "/updates/summary", view_updates_summary);
}
