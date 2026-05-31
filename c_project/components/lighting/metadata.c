#include "metadata.h"
#include "persistent_dict.h"
#include "json_helpers.h"

#include <string.h>

void metadata_get_scene(const char *scene_name, scene_metadata_t *meta)
{
    if (!meta) return;
    memset(meta, 0, sizeof(scene_metadata_t));

    if (!scene_name) return;

    persistent_dict_t *lighting = persistent_dict_open("/spiffs/data/lighting_settings.json");
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
}
