#include "views.h"
#include "webserver.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

cJSON *build_global_context(void)
{
    cJSON *ctx = cJSON_CreateObject();

    persistent_dict_t *sys_store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);

    /* Theme. Mirrors web/context_processors.py's _theme_processor(): the
     * stored "theme" value is already a full filename with a ".css"
     * extension (see ThemeView.post()/_theme_response() in web/views.py),
     * and an unset/empty theme means "no theme CSS link at all" - there is
     * no on-disk "default.css" to fall back to, so theme_css must be an
     * empty string in that case (matching the template's
     * `{% if theme_css %}` guard in templates/base/imports.html). */
    if (sys_store) {
        cJSON *current_theme = persistent_dict_get_dup(sys_store, "theme");
        if (current_theme && current_theme->valuestring && strlen(current_theme->valuestring) > 0) {
            cJSON_AddStringToObject(ctx, "theme", current_theme->valuestring);

            char theme_css[128];
            snprintf(theme_css, sizeof(theme_css), "themes/%s", current_theme->valuestring);
            cJSON_AddStringToObject(ctx, "theme_css", theme_css);

            char theme_path[136];
            snprintf(theme_path, sizeof(theme_path), "/%s", theme_css);
            cJSON_AddStringToObject(ctx, "theme_css_path", theme_path);
        } else {
            cJSON_AddStringToObject(ctx, "theme", "");
            cJSON_AddStringToObject(ctx, "theme_css", "");
            cJSON_AddStringToObject(ctx, "theme_css_path", "");
        }
        cJSON_Delete(current_theme);

        /* Hostname */
        cJSON *hostname = persistent_dict_get_dup(sys_store, "hostname");
        if (hostname && hostname->valuestring) {
            cJSON_AddStringToObject(ctx, "hostname", hostname->valuestring);
        } else {
            cJSON_AddStringToObject(ctx, "hostname", "lightmotron");
        }
        cJSON_Delete(hostname);
    } else {
        cJSON_AddStringToObject(ctx, "theme", "");
        cJSON_AddStringToObject(ctx, "theme_css", "");
        cJSON_AddStringToObject(ctx, "theme_css_path", "");
        cJSON_AddStringToObject(ctx, "hostname", "lightmotron");
    }

    /* Current model name */
    cJSON_AddStringToObject(ctx, "current_model", lighting_get_current_model());

    return ctx;
}
