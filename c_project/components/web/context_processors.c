#include "views.h"
#include "webserver.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Adds `key` to ctx holding an upper-cased copy of `value` -- the template
 * engine has no filter syntax (no `{{ x|upper }}`) so this has to happen
 * on the C side rather than in the template. */
static void add_uppercase_string(cJSON *ctx, const char *key, const char *value)
{
    char upper[128];
    size_t i = 0;
    for (; value[i] != '\0' && i < sizeof(upper) - 1; i++) {
        upper[i] = (char)toupper((unsigned char)value[i]);
    }
    upper[i] = '\0';
    cJSON_AddStringToObject(ctx, key, upper);
}

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

        /* Hostname. hostname_upper is a separate context key (not an
         * in-place change to "hostname") so pages that display/edit the
         * real, as-stored value (system_settings.html, status.html, ...)
         * are unaffected -- it exists purely for the header/title's
         * all-caps display. */
        cJSON *hostname = persistent_dict_get_dup(sys_store, "hostname");
        if (hostname && hostname->valuestring) {
            cJSON_AddStringToObject(ctx, "hostname", hostname->valuestring);
            add_uppercase_string(ctx, "hostname_upper", hostname->valuestring);
        } else {
            cJSON_AddStringToObject(ctx, "hostname", "lightmotron");
            add_uppercase_string(ctx, "hostname_upper", "lightmotron");
        }
        cJSON_Delete(hostname);
    } else {
        cJSON_AddStringToObject(ctx, "theme", "");
        cJSON_AddStringToObject(ctx, "theme_css", "");
        cJSON_AddStringToObject(ctx, "theme_css_path", "");
        cJSON_AddStringToObject(ctx, "hostname", "lightmotron");
        add_uppercase_string(ctx, "hostname_upper", "lightmotron");
    }

    /* Current model name */
    cJSON_AddStringToObject(ctx, "current_model", lighting_get_current_model());

    return ctx;
}
