#include "metadata.h"
#include "persistent_dict.h"
#include "json_helpers.h"

#include <string.h>

void metadata_get_scene(const char *scene_name, scene_metadata_t *meta)
{
    if (!meta) return;
    memset(meta, 0, sizeof(scene_metadata_t));

    if (!scene_name) return;

    persistent_dict_t *lighting = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    if (!lighting) return;

    cJSON *models = persistent_dict_get(lighting, "models");
    cJSON *current_name = persistent_dict_get(lighting, "current_model");
    if (!models || !current_name || !current_name->valuestring) return;

    cJSON *model = cJSON_GetObjectItem(models, current_name->valuestring);
    if (!model) return;

    cJSON *scene_settings = cJSON_GetObjectItem(model, "scene_settings");
    if (!scene_settings) return;

    cJSON *settings = cJSON_GetObjectItem(scene_settings, scene_name);
    if (!settings) return;

    /* Parse kills array */
    cJSON *kills = cJSON_GetObjectItem(settings, "kills");
    if (kills && cJSON_IsArray(kills)) {
        int size = cJSON_GetArraySize(kills);
        if (size > 8) size = 8;
        for (int i = 0; i < size; i++) {
            cJSON *item = cJSON_GetArrayItem(kills, i);
            if (item && item->valuestring) {
                strncpy(meta->kills[i], item->valuestring, 63);
                meta->kills_count++;
            }
        }
    }

    /* Parse sound */
    const char *sound = json_get_string(settings, "sound", NULL);
    if (sound) {
        strncpy(meta->sound, sound, sizeof(meta->sound) - 1);
    }

    /* Parse stop_sounds_on_start */
    cJSON *stop_start = cJSON_GetObjectItem(settings, "stop_sounds_on_start");
    if (stop_start && cJSON_IsArray(stop_start)) {
        int size = cJSON_GetArraySize(stop_start);
        if (size > 8) size = 8;
        for (int i = 0; i < size; i++) {
            cJSON *item = cJSON_GetArrayItem(stop_start, i);
            if (item && item->valuestring) {
                strncpy(meta->stop_sounds_on_start[i], item->valuestring, 63);
                meta->stop_sounds_on_start_count++;
            }
        }
    }

    /* Parse stop_sounds_on_end */
    cJSON *stop_end = cJSON_GetObjectItem(settings, "stop_sounds_on_end");
    if (stop_end && cJSON_IsArray(stop_end)) {
        int size = cJSON_GetArraySize(stop_end);
        if (size > 8) size = 8;
        for (int i = 0; i < size; i++) {
            cJSON *item = cJSON_GetArrayItem(stop_end, i);
            if (item && item->valuestring) {
                strncpy(meta->stop_sounds_on_end[i], item->valuestring, 63);
                meta->stop_sounds_on_end_count++;
            }
        }
    }

    /* Parse trigger_scenes_on_completion */
    cJSON *triggers = cJSON_GetObjectItem(settings, "trigger_scenes_on_completion");
    if (triggers && cJSON_IsArray(triggers)) {
        int size = cJSON_GetArraySize(triggers);
        if (size > 8) size = 8;
        for (int i = 0; i < size; i++) {
            cJSON *item = cJSON_GetArrayItem(triggers, i);
            if (item && item->valuestring) {
                strncpy(meta->trigger_scenes_on_completion[i], item->valuestring, 63);
                meta->trigger_scenes_on_completion_count++;
            }
        }
    }
}

/* Mirrors PATTERN_METADATA in lib/lighting/metadata.py exactly - name
 * spelling of required/optional params matters, since they are used
 * verbatim as web form field name suffixes (e.g. "pattern_param_width"). */
const pattern_metadata_t pattern_metadata_table[] = {
    {
        .name = "solid",
        .description = "Solid color",
        .required = {"target", "colors"},
        .required_count = 2,
        .optional = {0},
        .optional_count = 0,
        .color_count = 1,
    },
    {
        .name = "blink",
        .description = "Blinking color",
        .required = {"target", "colors"},
        .required_count = 2,
        .optional = {"duration", "frequency"},
        .optional_count = 2,
        .color_count = 2,
    },
    {
        .name = "pulse",
        .description = "Pulsing color",
        .required = {"target", "colors"},
        .required_count = 2,
        .optional = {"duration", "period"},
        .optional_count = 2,
        .color_count = 2,
    },
    {
        .name = "fade_in",
        .description = "Fade between two colors",
        .required = {"target", "colors"},
        .required_count = 2,
        .optional = {"duration"},
        .optional_count = 1,
        .color_count = 2,
    },
    {
        .name = "breathe",
        .description = "Breathing effect",
        .required = {"target", "colors"},
        .required_count = 2,
        .optional = {"duration"},
        .optional_count = 1,
        .color_count = 2,
    },
    {
        .name = "wave",
        .description = "Moving wave effect",
        .required = {"target", "colors"},
        .required_count = 2,
        .optional = {"duration", "number", "width", "reverse"},
        .optional_count = 4,
        .color_count = 2,
    },
    {
        .name = "cylon",
        .description = "Cylon bouncing effect",
        .required = {"target", "colors"},
        .required_count = 2,
        .optional = {"duration", "width"},
        .optional_count = 2,
        .color_count = 2,
    },
    {
        .name = "phaser_strip",
        .description = "Two waves from each end converging on a random meeting point",
        .required = {"target", "colors"},
        .required_count = 2,
        .optional = {"duration", "width"},
        .optional_count = 2,
        .color_count = 2,
    },
    { .name = NULL },
};

/* Mirrors FILTER_METADATA in lib/lighting/metadata.py exactly. */
const filter_metadata_t filter_metadata_table[] = {
    {
        .name = "null",
        .description = "No filter",
        .optional = {0},
        .optional_count = 0,
    },
    {
        .name = "sizzle",
        .description = "Sizzle filter",
        .optional = {"frequency", "variation_percent", "heat"},
        .optional_count = 3,
    },
    {
        .name = "scintillate",
        .description = "Scintillate filter",
        .optional = {"frequency", "variation_percent", "heat"},
        .optional_count = 3,
    },
    {
        .name = "brightness",
        .description = "Brightness filter",
        .optional = {"brightness"},
        .optional_count = 1,
    },
    {
        .name = "spike",
        .description = "Spike filter",
        .optional = {"color", "duration", "period", "variation", "heat", "scope"},
        .optional_count = 6,
    },
    {
        .name = "dropout",
        .description = "Dropout filter (black spike)",
        .optional = {"duration", "period", "variation", "heat", "scope"},
        .optional_count = 5,
    },
    { .name = NULL },
};

const pattern_metadata_t *metadata_get_pattern(const char *name)
{
    if (!name) return NULL;

    for (int i = 0; pattern_metadata_table[i].name != NULL; i++) {
        if (strcmp(name, pattern_metadata_table[i].name) == 0) {
            return &pattern_metadata_table[i];
        }
    }

    return NULL;
}

const filter_metadata_t *metadata_get_filter(const char *name)
{
    if (!name) return NULL;

    for (int i = 0; filter_metadata_table[i].name != NULL; i++) {
        if (strcmp(name, filter_metadata_table[i].name) == 0) {
            return &filter_metadata_table[i];
        }
    }

    return NULL;
}
