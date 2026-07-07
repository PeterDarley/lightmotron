#include "audio_player.h"
#include "yx5200.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "audio_player";

#define MAX_MODULES 3
#define SYSTEM_SETTINGS_FILE "/spiffs/data/system_settings.json"
#define HEALTH_CHECK_SETTLE_MS 50

static yx5200_t modules[MAX_MODULES];
static int module_count = 0;
static int current_volume = 20; /* Matches AudioPlayer's default master_volume in lib/audio.py */

esp_err_t audio_player_init_from_config(const cJSON *audio_players_config)
{
    /* Load master volume / debug-logging from persistent system settings,
     * mirroring AudioPlayer.__init__ in lib/audio.py. Note: audio_reset_on_boot
     * is read and applied by the caller via audio_player_reset_all(). */
    persistent_dict_t *sys_settings = persistent_dict_open(SYSTEM_SETTINGS_FILE);
    if (sys_settings) {
        cJSON *all = persistent_dict_get_all(sys_settings);
        current_volume = json_get_int(all, "master_volume", 20);
        bool debug_logging = json_get_bool(all, "audio_debug_logging", false);
        esp_log_level_set(TAG, debug_logging ? ESP_LOG_DEBUG : ESP_LOG_INFO);
        esp_log_level_set("yx5200", debug_logging ? ESP_LOG_DEBUG : ESP_LOG_INFO);
    }

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
        cJSON *tx_json = cJSON_GetObjectItem(entry, "tx_pin");
        cJSON *rx_json = cJSON_GetObjectItem(entry, "rx_pin");

        if (!uart_json || !tx_json || !rx_json) {
            ESP_LOGW(TAG, "Skipping audio module %d: missing config", i);
            continue;
        }

        int uart_num = uart_json->valueint;
        int tx_pin = tx_json->valueint;
        int rx_pin = rx_json->valueint;
        bool high_quality = json_get_bool(entry, "high_quality", false);

        esp_err_t ret = yx5200_init(&modules[module_count],
                                     (uart_port_t)uart_num, tx_pin, rx_pin, high_quality);
        if (ret == ESP_OK) {
            yx5200_set_volume(&modules[module_count], current_volume);
            module_count++;
            ESP_LOGI(TAG, "Audio module %d ready (UART%d, hq=%d)", module_count - 1, uart_num, high_quality);
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
    if (module_count == 0) {
        ESP_LOGW(TAG, "No audio modules configured");
        return -1;
    }

    /* Fast path, mirroring AudioPlayer.play_file in lib/audio.py: modules that
     * don't think they're playing are immediately available; modules that
     * think they are get a fresh query to confirm they haven't actually
     * stopped. */
    bool candidate[MAX_MODULES] = {0};
    bool any_candidate = false;
    for (int i = 0; i < module_count; i++) {
        if (!modules[i].is_playing) {
            candidate[i] = true;
        } else {
            yx5200_query_status(&modules[i]);
            candidate[i] = !modules[i].is_playing;
        }
        any_candidate = any_candidate || candidate[i];
    }

    if (!any_candidate) {
        ESP_LOGW(TAG, "All %d modules busy", module_count);
        return -1;
    }

    int module_idx = -1;
    if (high_quality_preferred) {
        for (int i = 0; i < module_count; i++) {
            if (candidate[i] && modules[i].high_quality) {
                module_idx = i;
                break;
            }
        }
    }
    if (module_idx < 0) {
        for (int i = 0; i < module_count; i++) {
            if (candidate[i]) {
                module_idx = i;
                break;
            }
        }
    }

    /* Ensure volume is current before playing. */
    yx5200_set_volume(&modules[module_idx], current_volume);

    esp_err_t ret = yx5200_play_file(&modules[module_idx], file_number);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start playback on module %d", module_idx);
        return -1;
    }

    return module_idx;
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
    int healthy = 0;

    for (int i = 0; i < module_count; i++) {
        /* Actively probe responsiveness: issue a volume command and re-query
         * status, mirroring AudioPlayer.check_health in lib/audio.py. */
        bool ok = (yx5200_set_volume(&modules[i], current_volume) == ESP_OK);
        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_SETTLE_MS));
        yx5200_query_status(&modules[i]);

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "module", i);
        cJSON_AddBoolToObject(obj, "ok", ok);
        cJSON_AddNumberToObject(obj, "uart", modules[i].uart_num);
        cJSON_AddNumberToObject(obj, "tx_pin", modules[i].tx_pin);
        cJSON_AddNumberToObject(obj, "rx_pin", modules[i].rx_pin);
        cJSON_AddBoolToObject(obj, "playing", modules[i].is_playing);
        cJSON_AddNumberToObject(obj, "current_file", modules[i].current_file);
        cJSON_AddItemToArray(arr, obj);

        if (ok) healthy++;
        ESP_LOGI(TAG, "health module %d uart=%d tx=%d rx=%d ok=%d playing=%d",
                 i, modules[i].uart_num, modules[i].tx_pin, modules[i].rx_pin, ok, modules[i].is_playing);
    }

    ESP_LOGI(TAG, "health check summary %d/%d modules responsive", healthy, module_count);
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
