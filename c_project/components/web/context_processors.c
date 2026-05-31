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

    persistent_dict_t *sys_store = persistent_dict_open("/spiffs/data/system_settings.json");

    /* Theme */
    if (sys_store) {
        cJSON *current_theme = persistent_dict_get_dup(sys_store, "theme");
        if (current_theme && current_theme->valuestring && strlen(current_theme->valuestring) > 0) {
            cJSON_AddStringToObject(ctx, "theme", current_theme->valuestring);

            char theme_path[128];
            snprintf(theme_path, sizeof(theme_path), "/themes/%s.css", current_theme->valuestring);
            cJSON_AddStringToObject(ctx, "theme_css_path", theme_path);
        } else {
            cJSON_AddStringToObject(ctx, "theme", "default");
            cJSON_AddStringToObject(ctx, "theme_css_path", "/themes/default.css");
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
        cJSON_AddStringToObject(ctx, "theme", "default");
        cJSON_AddStringToObject(ctx, "theme_css_path", "/themes/default.css");
        cJSON_AddStringToObject(ctx, "hostname", "lightmotron");
    }

    /* Current model name */
    cJSON_AddStringToObject(ctx, "current_model", lighting_get_current_model());

    return ctx;
}
