#include "routes.h"
#include "views.h"
#include "webserver.h"

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

    /* Models */
    webserver_add_route(HTTP_METHOD_GET, "/models", view_models);
    webserver_add_route(HTTP_METHOD_GET, "/models/summary", view_models_summary);
    webserver_add_route(HTTP_METHOD_POST, "/models/set", view_models_set);
    webserver_add_route(HTTP_METHOD_POST, "/models/wrap", view_models_wrap);

    /* Named ranges */
    webserver_add_route(HTTP_METHOD_GET, "/named_range", view_named_range);
    webserver_add_route(HTTP_METHOD_POST, "/named_range/set", view_named_range_set);
    webserver_add_route(HTTP_METHOD_POST, "/named_range/remove_subrange", view_named_range_remove_subrange);
    webserver_add_route(HTTP_METHOD_GET, "/named_range/summary", view_named_range_summary);

    /* Custom colors */
    webserver_add_route(HTTP_METHOD_GET, "/custom_colors", view_custom_colors);
    webserver_add_route(HTTP_METHOD_GET, "/custom_colors/summary", view_custom_colors_summary);

    /* Scenes */
    webserver_add_route(HTTP_METHOD_GET, "/scenes", view_scenes);
    webserver_add_route(HTTP_METHOD_POST, "/scenes/edit", view_scene_edit);
    webserver_add_route(HTTP_METHOD_POST, "/scenes/edit/add_trigger_scene", view_scene_edit_add_trigger);
    webserver_add_route(HTTP_METHOD_POST, "/scenes/edit/remove_trigger_scene", view_scene_edit_remove_trigger);
    webserver_add_route(HTTP_METHOD_GET, "/scenes/color_select", view_scenes_color_select);
    webserver_add_route(HTTP_METHOD_GET, "/scenes/summary", view_scenes_summary);

    /* Effects */
    webserver_add_route(HTTP_METHOD_GET, "/effects", view_effects);
    webserver_add_route(HTTP_METHOD_POST, "/effects/edit", view_effect_edit);
    webserver_add_route(HTTP_METHOD_GET, "/effects/color_select", view_effects_color_select);
    webserver_add_route(HTTP_METHOD_GET, "/effects/summary", view_effects_summary);

    /* Filters */
    webserver_add_route(HTTP_METHOD_GET, "/filters", view_filters);
    webserver_add_route(HTTP_METHOD_POST, "/filters/edit", view_filter_edit);
    webserver_add_route(HTTP_METHOD_GET, "/filters/summary", view_filters_summary);

    /* Sounds */
    webserver_add_route(HTTP_METHOD_GET, "/sounds", view_sounds);
    webserver_add_route(HTTP_METHOD_GET, "/sounds/summary", view_sounds_summary);
    webserver_add_route(HTTP_METHOD_POST, "/sounds/edit", view_sound_edit);
    webserver_add_route(HTTP_METHOD_GET, "/sounds/status", view_sounds_status);
    webserver_add_route(HTTP_METHOD_POST, "/sounds/play", view_play_sound);
    webserver_add_route(HTTP_METHOD_POST, "/sounds/stop", view_stop_sound);
    webserver_add_route(HTTP_METHOD_POST, "/sounds/stop-all", view_stop_all_sounds);
    webserver_add_route(HTTP_METHOD_POST, "/audio/volume", view_audio_volume);

    /* Soundscapes */
    webserver_add_route(HTTP_METHOD_GET, "/soundscapes", view_soundscapes);
    webserver_add_route(HTTP_METHOD_GET, "/soundscapes/summary", view_soundscapes_summary);
    webserver_add_route(HTTP_METHOD_GET, "/soundscapes/status", view_soundscapes_status);
    webserver_add_route(HTTP_METHOD_POST, "/soundscapes/edit", view_soundscape_edit);
    webserver_add_route(HTTP_METHOD_POST, "/soundscapes/play", view_play_soundscape);
    webserver_add_route(HTTP_METHOD_POST, "/soundscapes/stop", view_stop_soundscape);

    /* System */
    webserver_add_route(HTTP_METHOD_GET, "/backup", view_backup);
    webserver_add_route(HTTP_METHOD_POST, "/restore", view_restore);
    webserver_add_route(HTTP_METHOD_GET, "/restore/confirm", view_restore_confirm);
    webserver_add_route(HTTP_METHOD_GET, "/theme", view_theme);
    webserver_add_route(HTTP_METHOD_POST, "/theme/delete", view_theme_delete);
    webserver_add_route(HTTP_METHOD_POST, "/theme/upload", view_theme_upload);
    webserver_add_route(HTTP_METHOD_GET, "/hostname", view_hostname);
    webserver_add_route(HTTP_METHOD_GET, "/system_settings", view_system_settings);
    webserver_add_route(HTTP_METHOD_GET, "/system_settings/summary", view_system_settings_summary);
    webserver_add_route(HTTP_METHOD_POST, "/system_reboot", view_system_reboot);
    webserver_add_route(HTTP_METHOD_GET, "/system_reboot/confirm", view_system_reboot_confirm);
    webserver_add_route(HTTP_METHOD_GET, "/updates", view_updates);
    webserver_add_route(HTTP_METHOD_GET, "/updates/summary", view_updates_summary);
}
