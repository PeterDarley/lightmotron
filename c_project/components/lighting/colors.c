#include "colors.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "colors";

/* Built-in named colors */
typedef struct {
    const char *name;
    uint8_t r, g, b;
} named_color_t;

static const named_color_t builtin_colors[] = {
    {"red",        255, 0,   0},
    {"green",      0,   255, 0},
    {"blue",       0,   0,   255},
    {"white",      255, 255, 255},
    {"black",      0,   0,   0},
    {"yellow",     255, 255, 0},
    {"cyan",       0,   255, 255},
    {"magenta",    255, 0,   255},
    {"orange",     255, 165, 0},
    {"purple",     128, 0,   128},
    {"pink",       255, 192, 203},
    {"gold",       255, 215, 0},
    {"silver",     192, 192, 192},
    {"lime",       0,   255, 0},
    {"teal",       0,   128, 128},
    {"indigo",     75,  0,   130},
    {"violet",     238, 130, 238},
    {"coral",      255, 127, 80},
    {"turquoise",  64,  224, 208},
    {"crimson",    220, 20,  60},
    {"navy",       0,   0,   128},
    {"olive",      128, 128, 0},
    {"maroon",     128, 0,   0},
    {"aquamarine", 127, 255, 212},
    {"chartreuse", 127, 255, 0},
    {"salmon",     250, 128, 114},
    {"khaki",      240, 230, 140},
    {"plum",       221, 160, 221},
    {"tan",        210, 180, 140},
    {"peru",       205, 133, 63},
    {"sienna",     160, 82,  45},
    {NULL, 0, 0, 0},
};

uint8_t color_clamp(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

rgb_t color_interpolate(rgb_t a, rgb_t b, float factor)
{
    if (factor <= 0.0f) return a;
    if (factor >= 1.0f) return b;

    rgb_t result;
    result.r = color_clamp((int)(a.r + (b.r - a.r) * factor));
    result.g = color_clamp((int)(a.g + (b.g - a.g) * factor));
    result.b = color_clamp((int)(a.b + (b.b - a.b) * factor));
    return result;
}

rgb_t color_parse_hex(const char *hex)
{
    rgb_t black = {0, 0, 0};
    if (!hex) return black;

    /* Skip # prefix */
    if (*hex == '#') hex++;

    if (strlen(hex) != 6) return black;

    char rs[3] = {hex[0], hex[1], '\0'};
    char gs[3] = {hex[2], hex[3], '\0'};
    char bs[3] = {hex[4], hex[5], '\0'};

    rgb_t result;
    result.r = (uint8_t)strtol(rs, NULL, 16);
    result.g = (uint8_t)strtol(gs, NULL, 16);
    result.b = (uint8_t)strtol(bs, NULL, 16);
    return result;
}

rgb_t color_resolve_name(const char *name)
{
    rgb_t black = {0, 0, 0};
    if (!name) return black;

    /* Check custom colors first */
    persistent_dict_t *lighting = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (lighting) {
        cJSON *models = persistent_dict_get(lighting, "models");
        cJSON *current_name = persistent_dict_get(lighting, "current_model");
        if (models && current_name && current_name->valuestring) {
            cJSON *model = cJSON_GetObjectItem(models, current_name->valuestring);
            if (model) {
                cJSON *custom = cJSON_GetObjectItem(model, "custom_colors");
                if (custom) {
                    cJSON *color = cJSON_GetObjectItem(custom, name);
                    if (color && cJSON_IsString(color)) {
                        return color_parse_hex(color->valuestring);
                    }
                }
            }
        }
    }

    /* Check built-in colors (case-insensitive) */
    for (int i = 0; builtin_colors[i].name != NULL; i++) {
        if (strcasecmp(name, builtin_colors[i].name) == 0) {
            rgb_t result = {
                builtin_colors[i].r,
                builtin_colors[i].g,
                builtin_colors[i].b
            };
            return result;
        }
    }

    return black;
}

rgb_t color_resolve(const char *spec)
{
    rgb_t black = {0, 0, 0};
    if (!spec) return black;

    /* Skip whitespace */
    while (*spec == ' ') spec++;

    /* Hex color */
    if (*spec == '#' || (strlen(spec) == 6 && isxdigit(spec[0]))) {
        return color_parse_hex(spec);
    }

    /* Named color */
    return color_resolve_name(spec);
}

rgb_t color_resolve_json(const cJSON *spec)
{
    rgb_t black = {0, 0, 0};
    if (!spec) return black;

    if (cJSON_IsString(spec)) {
        return color_resolve(spec->valuestring);
    }

    if (cJSON_IsArray(spec) && cJSON_GetArraySize(spec) >= 3) {
        rgb_t result;
        result.r = color_clamp(cJSON_GetArrayItem(spec, 0)->valueint);
        result.g = color_clamp(cJSON_GetArrayItem(spec, 1)->valueint);
        result.b = color_clamp(cJSON_GetArrayItem(spec, 2)->valueint);
        return result;
    }

    return black;
}
