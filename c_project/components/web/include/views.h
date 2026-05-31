#ifndef WEB_VIEWS_H
#define WEB_VIEWS_H

#include "webserver.h"

/**
 * All view functions follow the route_handler_t signature:
 *   http_response_t *view_xxx(http_request_t *req);
 *
 * Each view builds a cJSON context, calls render_template(), and returns
 * a response via response_create() or response_redirect().
 */

/* Home & general views */
http_response_t *view_home(http_request_t *req);
http_response_t *view_set_scene(http_request_t *req);
http_response_t *view_scene_panel_status(http_request_t *req);
http_response_t *view_animation(http_request_t *req);
http_response_t *view_storage(http_request_t *req);
http_response_t *view_status(http_request_t *req);
http_response_t *view_confirm(http_request_t *req);

/* Setup */
http_response_t *view_setup(http_request_t *req);

/* Models */
http_response_t *view_models(http_request_t *req);
http_response_t *view_models_summary(http_request_t *req);
http_response_t *view_models_set(http_request_t *req);
http_response_t *view_models_wrap(http_request_t *req);

/* Named ranges */
http_response_t *view_named_range(http_request_t *req);
http_response_t *view_named_range_set(http_request_t *req);
http_response_t *view_named_range_remove_subrange(http_request_t *req);
http_response_t *view_named_range_summary(http_request_t *req);

/* Custom colors */
http_response_t *view_custom_colors(http_request_t *req);
http_response_t *view_custom_colors_summary(http_request_t *req);

/* Scenes */
http_response_t *view_scenes(http_request_t *req);
http_response_t *view_scene_edit(http_request_t *req);
http_response_t *view_scene_edit_add_trigger(http_request_t *req);
http_response_t *view_scene_edit_remove_trigger(http_request_t *req);
http_response_t *view_scenes_color_select(http_request_t *req);
http_response_t *view_scenes_summary(http_request_t *req);

/* Effects */
http_response_t *view_effects(http_request_t *req);
http_response_t *view_effect_edit(http_request_t *req);
http_response_t *view_effects_color_select(http_request_t *req);
http_response_t *view_effects_summary(http_request_t *req);

/* Filters */
http_response_t *view_filters(http_request_t *req);
http_response_t *view_filter_edit(http_request_t *req);
http_response_t *view_filters_summary(http_request_t *req);

/* Sounds */
http_response_t *view_sounds(http_request_t *req);
http_response_t *view_sounds_summary(http_request_t *req);
http_response_t *view_sound_edit(http_request_t *req);
http_response_t *view_sounds_status(http_request_t *req);
http_response_t *view_play_sound(http_request_t *req);
http_response_t *view_stop_sound(http_request_t *req);
http_response_t *view_stop_all_sounds(http_request_t *req);
http_response_t *view_audio_volume(http_request_t *req);

/* Soundscapes */
http_response_t *view_soundscapes(http_request_t *req);
http_response_t *view_soundscapes_summary(http_request_t *req);
http_response_t *view_soundscapes_status(http_request_t *req);
http_response_t *view_soundscape_edit(http_request_t *req);
http_response_t *view_play_soundscape(http_request_t *req);
http_response_t *view_stop_soundscape(http_request_t *req);

/* System */
http_response_t *view_backup(http_request_t *req);
http_response_t *view_restore(http_request_t *req);
http_response_t *view_restore_confirm(http_request_t *req);
http_response_t *view_theme(http_request_t *req);
http_response_t *view_theme_delete(http_request_t *req);
http_response_t *view_theme_upload(http_request_t *req);
http_response_t *view_hostname(http_request_t *req);
http_response_t *view_system_settings(http_request_t *req);
http_response_t *view_system_settings_summary(http_request_t *req);
http_response_t *view_system_reboot(http_request_t *req);
http_response_t *view_system_reboot_confirm(http_request_t *req);
http_response_t *view_updates(http_request_t *req);
http_response_t *view_updates_summary(http_request_t *req);

/**
 * Build the global template context (theme, hostname, model name).
 *
 * Returns a cJSON object that the caller owns and must add view-specific
 * keys to before passing to render_template().
 */
cJSON *build_global_context(void);

#endif /* WEB_VIEWS_H */
