#include "views.h"
#include "webserver.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>

/* Adds `key` to ctx holding an upper-cased, dash-to-space copy of `value`
 * (e.g. "event-horizon" -> "EVENT HORIZON") for display in the title/
 * header -- the template engine has no filter syntax (no `{{ x|upper }}`)
 * so this has to happen on the C side rather than in the template. */
static void add_uppercase_string(cJSON *ctx, const char *key, const char *value)
{
    char upper[128];
    size_t i = 0;
    for (; value[i] != '\0' && i < sizeof(upper) - 1; i++) {
        upper[i] = value[i] == '-' ? ' ' : (char)toupper((unsigned char)value[i]);
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
    /* Favicon. A theme can ship themes/<name>.svg next to its stylesheet
     * (themes/nautilus.css -> themes/nautilus.svg); otherwise the default
     * /favicon.svg is used. Checked here so the page links to a file that
     * exists. */
    char favicon_href[128];
    snprintf(favicon_href, sizeof(favicon_href), "/favicon.svg");

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

            /* themes/<stem>.svg, where <stem> is the theme name without ".css" */
            char stem[96];
            snprintf(stem, sizeof(stem), "%s", current_theme->valuestring);
            size_t stem_len = strlen(stem);
            if (stem_len > 4 && strcmp(stem + stem_len - 4, ".css") == 0) {
                stem[stem_len - 4] = '\0';
            }
            /* The stat() below reads flash, and every page render would
             * otherwise repeat it. Flash reads are slow and block the LED
             * interrupt on the other core (see the interrupt-watchdog notes),
             * so the answer is cached per theme name and only looked up again
             * when the theme changes. */
            static char favicon_theme[128] = "";
            static char favicon_cached[128] = "/favicon.svg";
            if (strcmp(favicon_theme, current_theme->valuestring) != 0) {
                char icon_fs[160], icon_href[128];
                snprintf(icon_fs, sizeof(icon_fs), "/spiffs/www/themes/%s.svg", stem);
                snprintf(icon_href, sizeof(icon_href), "/themes/%s.svg", stem);
                struct stat st;
                snprintf(favicon_cached, sizeof(favicon_cached), "%s",
                         stat(icon_fs, &st) == 0 ? icon_href : "/favicon.svg");
                snprintf(favicon_theme, sizeof(favicon_theme), "%s", current_theme->valuestring);
            }
            snprintf(favicon_href, sizeof(favicon_href), "%s", favicon_cached);
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

    /* Favicon for base/base_head.html (theme icon, or the default). */
    cJSON_AddStringToObject(ctx, "favicon_href", favicon_href);

    return ctx;
}
