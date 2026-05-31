#include "audio_player.h"
#include "yx5200.h"
#include "esp_log.h"
#include "cJSON.h"

#include <string.h>

static const char *TAG = "audio_player";

#define MAX_MODULES 3

static yx5200_t modules[MAX_MODULES];
static int module_count = 0;
static int current_volume = 15;

esp_err_t audio_player_init_from_config(const cJSON *audio_players_config)
{
    if (!audio_players_config || !cJSON_IsArray(audio_players_config)) {
        ESP_LOGW(TAG, "No audio player config");
        return ESP_OK;
    }

    int count = cJSON_GetArraySize(audio_players_config);
    if (count > MAX_MODULES) count = MAX_MODULES;

    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_GetArrayItem(audio_players_config, i);
        if (!entry) continue;

        cJSON *uart_json = cJSON_GetObjectItem(entry, "uart");
        cJSON *tx_json = cJSON_GetObjectItem(entry, "tx");
        cJSON *rx_json = cJSON_GetObjectItem(entry, "rx");

        if (!uart_json || !tx_json || !rx_json) {
            ESP_LOGW(TAG, "Skipping audio module %d: missing config", i);
            continue;
        }

        int uart_num = uart_json->valueint;
        int tx_pin = tx_json->valueint;
        int rx_pin = rx_json->valueint;

        esp_err_t ret = yx5200_init(&modules[module_count],
                                     (uart_port_t)uart_num, tx_pin, rx_pin);
        if (ret == ESP_OK) {
            /* Reset and set default volume */
            yx5200_reset(&modules[module_count]);
            yx5200_set_volume(&modules[module_count], current_volume);
            module_count++;
            ESP_LOGI(TAG, "Audio module %d ready (UART%d)", module_count - 1, uart_num);
        }
    }

    ESP_LOGI(TAG, "Audio player initialized with %d modules", module_count);
    return ESP_OK;
}

void audio_player_reset_all(void)
{
    for (int i = 0; i < module_count; i++) {
        yx5200_reset(&modules[i]);
        yx5200_set_volume(&modules[i], current_volume);
    }
}

int audio_player_play_file(int file_number, bool high_quality_preferred)
{
    /* Find first available (not playing) module */
    for (int i = 0; i < module_count; i++) {
        yx5200_query_status(&modules[i]);
        if (!modules[i].is_playing) {
            esp_err_t ret = yx5200_play_file(&modules[i], file_number);
            if (ret == ESP_OK) return i;
        }
    }

    /* All busy — use first module (interrupt it) */
    if (module_count > 0) {
        yx5200_play_file(&modules[0], file_number);
        return 0;
    }

    ESP_LOGW(TAG, "No audio modules available");
    return -1;
}

esp_err_t audio_player_stop_module(int module_index)
{
    if (module_index < 0 || module_index >= module_count) return ESP_ERR_INVALID_ARG;
    return yx5200_stop(&modules[module_index]);
}

void audio_player_stop_all(void)
{
    for (int i = 0; i < module_count; i++) {
        yx5200_stop(&modules[i]);
    }
}

void audio_player_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 30) volume = 30;
    current_volume = volume;

    for (int i = 0; i < module_count; i++) {
        yx5200_set_volume(&modules[i], volume);
    }
}

int audio_player_get_volume(void)
{
    return current_volume;
}

cJSON *audio_player_check_health(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < module_count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "module", i);
        cJSON_AddBoolToObject(obj, "initialized", modules[i].initialized);
        cJSON_AddBoolToObject(obj, "playing", modules[i].is_playing);
        cJSON_AddNumberToObject(obj, "volume", modules[i].volume);
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

bool audio_player_is_module_playing(int module_index)
{
    if (module_index < 0 || module_index >= module_count) return false;
    yx5200_query_status(&modules[module_index]);
    return modules[module_index].is_playing;
}

int audio_player_get_module_count(void)
{
    return module_count;
}
