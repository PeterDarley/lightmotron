#include "colors.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

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

    persistent_dict_t *lighting = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
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

rgb_t color_from_hsv(float hue, float saturation, float value)
{
    rgb_t result;

    if (saturation <= 0.0f) {
        uint8_t gray = color_clamp((int)(value * 255.0f));
        result.r = result.g = result.b = gray;
        return result;
    }

    hue = hue - floorf(hue);            /* wrap into 0..1 */
    int sector = (int)(hue * 6.0f);
    float offset = hue * 6.0f - sector;
    float p = value * (1.0f - saturation);
    float q = value * (1.0f - saturation * offset);
    float t = value * (1.0f - saturation * (1.0f - offset));
    sector %= 6;

    float red, green, blue;
    switch (sector) {
        case 0:  red = value; green = t;     blue = p;     break;
        case 1:  red = q;     green = value; blue = p;     break;
        case 2:  red = p;     green = value; blue = t;     break;
        case 3:  red = p;     green = q;     blue = value; break;
        case 4:  red = t;     green = p;     blue = value; break;
        default: red = value; green = p;     blue = q;     break;
    }

    result.r = color_clamp((int)(red * 255.0f));
    result.g = color_clamp((int)(green * 255.0f));
    result.b = color_clamp((int)(blue * 255.0f));
    return result;
}

void color_to_hsv(rgb_t color, float *hue, float *saturation, float *value)
{
    float r_norm = color.r / 255.0f;
    float g_norm = color.g / 255.0f;
    float b_norm = color.b / 255.0f;

    float maximum = fmaxf(r_norm, fmaxf(g_norm, b_norm));
    float minimum = fminf(r_norm, fminf(g_norm, b_norm));
    float chroma = maximum - minimum;

    float h;
    if (chroma == 0.0f) {
        h = 0.0f;
    } else if (maximum == r_norm) {
        h = fmodf((g_norm - b_norm) / chroma, 6.0f) / 6.0f;
    } else if (maximum == g_norm) {
        h = (((b_norm - r_norm) / chroma) + 2.0f) / 6.0f;
    } else {
        h = (((r_norm - g_norm) / chroma) + 4.0f) / 6.0f;
    }
    if (h < 0.0f) h += 1.0f;

    if (hue) *hue = h;
    if (saturation) *saturation = (maximum == 0.0f) ? 0.0f : (chroma / maximum);
    if (value) *value = maximum;
}

rgb_t color_heat(int heat)
{
    int clamped = heat < 0 ? 0 : (heat > 255 ? 255 : heat);
    int scaled = (clamped * 191) / 255;
    uint8_t ramp = (uint8_t)((scaled & 0x3F) << 2);

    rgb_t result;
    if (scaled > 128) {
        result.r = 255; result.g = 255; result.b = ramp;
    } else if (scaled > 64) {
        result.r = 255; result.g = ramp; result.b = 0;
    } else {
        result.r = ramp; result.g = 0; result.b = 0;
    }
    return result;
}
