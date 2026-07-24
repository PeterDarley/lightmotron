#ifndef WEB_VIEWS_H
#define WEB_VIEWS_H

#include "webserver.h"

/**
 * All view functions follow the route_handler_t signature:
 *   http_response_t *view_xxx(http_request_t *req);
 *
 * Each view builds a cJSON context (starting from build_global_context())
 * and returns a response via webserver_render_response(),
 * response_create(), or response_redirect().
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

/**
 * Build the {named_ranges} context (sorted by name, mirrors
 * get_named_range_summary_entries("name")). Exposed so view_setup() can
 * merge it into the setup page's initial render, matching SetupView.get().
 */
void add_named_ranges_summary_context(cJSON *ctx);

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

/**
 * Build the {scenes} context ({name, effect_count} list, mirrors
 * ScenesSummaryView.get()). Exposed so view_setup() can merge it into the
 * setup page's initial render, matching SetupView.get().
 */
void add_scenes_summary_context(cJSON *ctx);

/* Effects */
http_response_t *view_effects(http_request_t *req);
http_response_t *view_effect_edit(http_request_t *req);
http_response_t *view_effects_color_select(http_request_t *req);
http_response_t *view_effects_summary(http_request_t *req);

/**
 * Build the {effect_names} context (sorted names, mirrors
 * EffectsSummaryView.get()). Exposed so view_setup() can merge it into the
 * setup page's initial render, matching SetupView.get().
 */
void add_effects_summary_context(cJSON *ctx);

/* Filters */
http_response_t *view_filters(http_request_t *req);
http_response_t *view_filters_post(http_request_t *req);
http_response_t *view_filter_edit(http_request_t *req);
http_response_t *view_filter_color_select(http_request_t *req);
http_response_t *view_filters_summary(http_request_t *req);

/**
 * Build the {filter_names} context (sorted names, mirrors
 * FiltersSummaryView.get()). Exposed so view_setup() can merge it into the
 * setup page's initial render, matching SetupView.get().
 */
void add_filters_summary_context(cJSON *ctx);

/* Sounds */
http_response_t *view_sounds(http_request_t *req);
http_response_t *view_sounds_summary(http_request_t *req);
http_response_t *view_sound_edit(http_request_t *req);
http_response_t *view_sounds_status(http_request_t *req);
http_response_t *view_play_sound(http_request_t *req);
http_response_t *view_stop_sound(http_request_t *req);
http_response_t *view_stop_all_sounds(http_request_t *req);
http_response_t *view_audio_volume(http_request_t *req);

/**
 * Build the {sounds, ...} context (mirrors _sounds_context()). Exposed so
 * view_setup() can merge it into the setup page's initial render, matching
 * SetupView.get().
 */
void add_sounds_context(cJSON *ctx, bool include_playing, bool home_only);

/* Soundscapes */
http_response_t *view_soundscapes(http_request_t *req);
http_response_t *view_soundscapes_summary(http_request_t *req);
http_response_t *view_soundscapes_status(http_request_t *req);
http_response_t *view_soundscape_edit(http_request_t *req);
http_response_t *view_play_soundscape(http_request_t *req);
http_response_t *view_stop_soundscape(http_request_t *req);

/**
 * Build the {soundscapes, soundscape_rows, ..., active_soundscape,
 * current_volume} context (mirrors _soundscapes_context()). Exposed so
 * view_home() can merge it into the home page context for the soundscapes
 * panel, matching HomeView.get() in web/views.py.
 */
void add_soundscapes_context(cJSON *ctx, bool include_active);

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
http_response_t *view_system_settings_ip_announced(http_request_t *req);
http_response_t *view_system_reboot(http_request_t *req);
http_response_t *view_system_reboot_confirm(http_request_t *req);
http_response_t *view_updates(http_request_t *req);
http_response_t *view_updates_summary(http_request_t *req);

/**
 * Build the global template context (theme, hostname, model name).
 *
 * Returns a cJSON object that the caller owns and must add view-specific
 * keys to before passing to webserver_render_response().
 */
cJSON *build_global_context(void);

#endif /* WEB_VIEWS_H */
