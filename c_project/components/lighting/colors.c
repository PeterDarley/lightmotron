#include "colors.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "colors";

/* Built-in named colors */
typedef struct {
    const char *name;
    uint8_t r, g, b;
} named_color_t;

/* Must match the `colors` dict in lib/lighting/colors.py exactly (name
 * spelling and RGB values) - names are looked up with an exact, case
 * sensitive match, same as a Python dict lookup. */
static const named_color_t builtin_colors[] = {
    /* Neutrals */
    {"white",       255, 255, 255},
    {"warm_white",  255, 220, 160},
    {"cool_white",  180, 210, 255},
    {"dim_white",   64,  64,  64},
    {"silver",      180, 180, 200},
    {"grey",        128, 128, 128},
    {"black",       0,   0,   0},
    /* Reds / oranges */
    {"red",         255, 0,   0},
    {"dark_red",    128, 0,   0},
    {"orange",      255, 100, 0},
    {"amber",       255, 160, 0},
    {"gold",        255, 200, 0},
    {"yellow",      255, 255, 0},
    /* Greens */
    {"green",       0,   255, 0},
    {"dark_green",  0,   128, 0},
    {"lime",        128, 255, 0},
    {"teal",        0,   180, 128},
    /* Blues / purples */
    {"cyan",        0,   255, 255},
    {"ice_blue",    80,  160, 255},
    {"blue",        0,   0,   255},
    {"dark_blue",   0,   0,   128},
    {"indigo",      60,  0,   180},
    {"violet",      180, 0,   255},
    {"purple",      128, 0,   128},
    {"magenta",     255, 0,   255},
    {"pink",        255, 80,  150},
    /* Specialty / model */
    {"fire",        255, 40,  0},
    {"plasma",      0,   200, 255},
    {"engine_glow", 100, 40,  255},
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
    /* Mirrors `colors.get(input, (255, 255, 255))` in Lighting.get_color():
     * an exact, case-sensitive dict-style lookup against the built-in table
     * only (custom colors are never consulted here - only via the
     * "custom:" prefix, see color_resolve()). Unknown names default to
     * white, not black. */
    rgb_t white = {255, 255, 255};
    if (!name) return white;

    for (int i = 0; builtin_colors[i].name != NULL; i++) {
        if (strcmp(name, builtin_colors[i].name) == 0) {
            rgb_t result = {
                builtin_colors[i].r,
                builtin_colors[i].g,
                builtin_colors[i].b
            };
            return result;
        }
    }

    return white;
}

static rgb_t color_resolve_custom(const char *name)
{
    /* Mirrors the "custom:" branch of Lighting.get_color(): looks up
     * self.settings["custom_colors"][name], stored as a 3-element [r, g, b]
     * array (not a hex string - hex strings are only used transiently in
     * the web UI when saving a custom color). Missing name/model/dict
     * defaults to white. */
    rgb_t white = {255, 255, 255};
    if (!name) return white;

    persistent_dict_t *lighting = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (!lighting) return white;

    cJSON *models = persistent_dict_get(lighting, "models");
    cJSON *current_name = persistent_dict_get(lighting, "current_model");
    if (!models || !current_name || !current_name->valuestring) return white;

    cJSON *model = cJSON_GetObjectItem(models, current_name->valuestring);
    if (!model) return white;

    cJSON *custom = cJSON_GetObjectItem(model, "custom_colors");
    if (!custom) return white;

    cJSON *color = cJSON_GetObjectItem(custom, name);
    if (!color || !cJSON_IsArray(color) || cJSON_GetArraySize(color) < 3) return white;

    rgb_t result;
    result.r = color_clamp(cJSON_GetArrayItem(color, 0)->valueint);
    result.g = color_clamp(cJSON_GetArrayItem(color, 1)->valueint);
    result.b = color_clamp(cJSON_GetArrayItem(color, 2)->valueint);
    return result;
}

rgb_t color_resolve(const char *spec)
{
    /* Mirrors Lighting.get_color() for string input: a "custom:" prefix
     * looks up a saved custom color; anything else is an exact named-color
     * lookup. There is no hex-string parsing at this layer in Python -
     * hex is only ever converted to RGB once, at custom-color save time. */
    rgb_t white = {255, 255, 255};
    if (!spec) return white;

    if (strncmp(spec, "custom:", 7) == 0) {
        return color_resolve_custom(spec + 7);
    }

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
