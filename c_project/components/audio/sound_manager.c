#include "sound_manager.h"
#include "audio_player.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "sound_manager";

#define MAX_PLAYING_SOUNDS 3
#define POLL_INTERVAL_MS   500

typedef struct {
    char title[64];
    int module_index;
    int loops_remaining;
    int file_number;
    bool active;
} playing_sound_t;

static playing_sound_t playing_sounds[MAX_PLAYING_SOUNDS];
static SemaphoreHandle_t sound_mutex = NULL;
static TaskHandle_t poll_task_handle = NULL;
static bool initialized = false;

static cJSON *get_sounds_dict(void)
{
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (!store) return NULL;

    cJSON *models = persistent_dict_get(store, "models");
    cJSON *current = persistent_dict_get(store, "current_model");
    if (!models || !current || !current->valuestring) return NULL;

    cJSON *model = cJSON_GetObjectItem(models, current->valuestring);
    if (!model) return NULL;

    return cJSON_GetObjectItem(model, "sounds");
}

static cJSON *get_sound_by_title(const char *title)
{
    cJSON *sounds = get_sounds_dict();
    if (!sounds) return NULL;
    return cJSON_GetObjectItem(sounds, title);
}

esp_err_t sound_manager_init(void)
{
    if (initialized) return ESP_OK;

    sound_mutex = xSemaphoreCreateMutex();
    memset(playing_sounds, 0, sizeof(playing_sounds));
    initialized = true;

    ESP_LOGI(TAG, "Sound manager initialized");
    return ESP_OK;
}

static void poll_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        xSemaphoreTake(sound_mutex, portMAX_DELAY);

        for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
            if (!playing_sounds[i].active) continue;

            bool still_playing = audio_player_is_module_playing(playing_sounds[i].module_index);

            if (!still_playing) {
                /* Sound ended — check for looping */
                if (playing_sounds[i].loops_remaining > 0) {
                    playing_sounds[i].loops_remaining--;
                    int mod = audio_player_play_file(playing_sounds[i].file_number, false);
                    if (mod >= 0) {
                        playing_sounds[i].module_index = mod;
                        ESP_LOGD(TAG, "Looping sound '%s' (%d remaining)",
                                 playing_sounds[i].title, playing_sounds[i].loops_remaining);
                    } else {
                        playing_sounds[i].active = false;
                    }
                } else {
                    ESP_LOGD(TAG, "Sound ended: '%s'", playing_sounds[i].title);
                    playing_sounds[i].active = false;
                }
            }
        }

        xSemaphoreGive(sound_mutex);
    }
}

esp_err_t sound_manager_start_polling(void)
{
    if (poll_task_handle) return ESP_OK;

    xTaskCreate(poll_task, "snd_poll", 2048, NULL, 3, &poll_task_handle);
    return ESP_OK;
}

esp_err_t sound_manager_play(const char *title, int loop_count_override)
{
    if (!title || !initialized) return ESP_ERR_INVALID_STATE;

    cJSON *sound = get_sound_by_title(title);
    if (!sound) {
        ESP_LOGW(TAG, "Sound not found: '%s'", title);
        return ESP_ERR_NOT_FOUND;
    }

    int file_number = json_get_int(sound, "file", 0);
    bool high_quality = json_get_bool(sound, "high_quality", false);
    int loop_count = (loop_count_override > 0) ? loop_count_override
                     : json_get_int(sound, "loop_count", 0);

    /* Stop sounds listed in "stops" */
    cJSON *stops = cJSON_GetObjectItem(sound, "stops");
    if (stops && cJSON_IsArray(stops)) {
        int stop_count = cJSON_GetArraySize(stops);
        for (int i = 0; i < stop_count; i++) {
            cJSON *item = cJSON_GetArrayItem(stops, i);
            if (item && item->valuestring) {
                sound_manager_stop(item->valuestring);
            }
        }
    }

    int module_idx = audio_player_play_file(file_number, high_quality);
    if (module_idx < 0) {
        ESP_LOGW(TAG, "No module available for sound '%s'", title);
        return ESP_FAIL;
    }

    /* Track the playing sound */
    xSemaphoreTake(sound_mutex, portMAX_DELAY);

    int slot = -1;
    for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
        if (!playing_sounds[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* All slots full — stop the oldest and reuse its slot */
        slot = 0;
        audio_player_stop_module(playing_sounds[slot].module_index);
        playing_sounds[slot].active = false;
    }

    strncpy(playing_sounds[slot].title, title, sizeof(playing_sounds[slot].title) - 1);
    playing_sounds[slot].module_index = module_idx;
    playing_sounds[slot].loops_remaining = loop_count;
    playing_sounds[slot].file_number = file_number;
    playing_sounds[slot].active = true;

    xSemaphoreGive(sound_mutex);

    ESP_LOGI(TAG, "Playing sound '%s' file=%d module=%d loops=%d",
             title, file_number, module_idx, loop_count);
    return ESP_OK;
}

esp_err_t sound_manager_stop(const char *title)
{
    if (!title || !initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(sound_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
        if (playing_sounds[i].active && strcmp(playing_sounds[i].title, title) == 0) {
            audio_player_stop_module(playing_sounds[i].module_index);
            playing_sounds[i].active = false;
            ESP_LOGI(TAG, "Stopped sound '%s'", title);
            break;
        }
    }

    xSemaphoreGive(sound_mutex);
    return ESP_OK;
}

void sound_manager_stop_all(void)
{
    if (!initialized) return;

    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    audio_player_stop_all();
    memset(playing_sounds, 0, sizeof(playing_sounds));
    xSemaphoreGive(sound_mutex);
}

bool sound_manager_is_playing(const char *title)
{
    if (!title || !initialized) return false;

    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
        if (playing_sounds[i].active && strcmp(playing_sounds[i].title, title) == 0) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(sound_mutex);
    return found;
}

cJSON *sound_manager_get_playing(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!initialized) return arr;

    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
        if (playing_sounds[i].active) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(playing_sounds[i].title));
        }
    }
    xSemaphoreGive(sound_mutex);
    return arr;
}
