/**
 * IP address announcement — plays the device's IP address over audio when
 * it differs from the last stored IP, without blocking the caller.
 *
 * Ported from ip_announcement.py. One deliberate behavioral simplification:
 * audio_player_play_file() on this port always succeeds when at least one
 * module is configured (it interrupts whatever is currently playing rather
 * than raising Python's NoPlayersAvailable), so the retry-until-free loop
 * here only actually retries when zero modules are configured at all.
 */
#include "ip_announcement.h"
#include "wifi_manager.h"
#include "persistent_dict.h"
#include "audio_player.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "ip_announcement";

#define IP_ANNOUNCE_TASK_STACK 4096
#define IP_ANNOUNCE_TASK_PRIORITY 2

typedef struct {
    char ip_address[16];
    int voice;
} ip_announce_args_t;

static void store_ip_address(const char *ip_address)
{
    persistent_dict_t *sys = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *val = cJSON_CreateString(ip_address);
    persistent_dict_set(sys, "stored_ip_address", val);
    persistent_dict_save(sys);
    ESP_LOGI(TAG, "Stored IP address: %s", ip_address);
}

static const char *get_stored_ip_address(void)
{
    static char stored[16];
    stored[0] = '\0';

    persistent_dict_t *sys = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *val = persistent_dict_get(sys, "stored_ip_address");
    if (val && cJSON_IsString(val)) {
        strncpy(stored, val->valuestring, sizeof(stored) - 1);
        stored[sizeof(stored) - 1] = '\0';
    }

    return stored;
}

/**
 * Play one file, retrying until a module is free or max_wait_s elapses.
 * Mirrors ip_announcement.py's _play_with_retry().
 */
static bool play_with_retry(int file_number, int max_wait_s)
{
    int attempts = max_wait_s * 10;
    for (int i = 0; i < attempts; i++) {
        if (audio_player_play_file(file_number, false) >= 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "Timed out waiting to play file %d", file_number);
    return false;
}

/**
 * Background task: announce the IP address twice (now, then again after a
 * 15s pause), playing the "my IP address is" intro then each octet's
 * digits with a 3s pause between octets. Restores the previous volume when
 * done. Mirrors ip_announcement.py's _announce_ip_address_background().
 */
static void ip_announce_task(void *pvParameters)
{
    ip_announce_args_t *args = (ip_announce_args_t *)pvParameters;
    char ip_address[16];
    strncpy(ip_address, args->ip_address, sizeof(ip_address) - 1);
    ip_address[sizeof(ip_address) - 1] = '\0';
    int voice = args->voice;
    free(args);

    if (voice != 8 && voice != 9) {
        voice = 8;
    }

    /* Parse IP octets */
    int octets[4];
    int octet_count = 0;
    char *token = strtok(ip_address, ".");
    while (token && octet_count < 4) {
        octets[octet_count++] = atoi(token);
        token = strtok(NULL, ".");
    }

    if (octet_count != 4) {
        ESP_LOGW(TAG, "IP address has %d parts, expected 4", octet_count);
        vTaskDelete(NULL);
        return;
    }

    int original_volume = audio_player_get_volume();
    audio_player_set_volume(30);

    /* "my ip address is" = 9908.mp3 or 9909.mp3 */
    int intro_file = 9900 + voice;

    for (int announcement_pass = 0; announcement_pass < 2; announcement_pass++) {
        if (announcement_pass > 0) {
            vTaskDelay(pdMS_TO_TICKS(15000));
        }

        if (!play_with_retry(intro_file, 10)) {
            break;
        }

        for (int octet_idx = 0; octet_idx < 4; octet_idx++) {
            char digit_str[4];
            int digit_count = snprintf(digit_str, sizeof(digit_str), "%d", octets[octet_idx]);

            /* Leading zeros within an octet are naturally skipped since
             * digit_str has no leading zero padding. */
            for (int d = 0; d < digit_count; d++) {
                int digit = digit_str[d] - '0';
                int digit_file = 9900 + (voice * 10) + digit;
                play_with_retry(digit_file, 10);
            }

            if (octet_idx < 3) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        }
    }

    audio_player_set_volume(original_volume);
    vTaskDelete(NULL);
}

esp_err_t ip_announcement_check_and_announce(void)
{
    persistent_dict_t *sys = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *audio_players = persistent_dict_get(sys, "audio_players");

    if (!audio_players || !cJSON_IsArray(audio_players) || cJSON_GetArraySize(audio_players) == 0) {
        /* No audio players configured, skip announcement */
        return ESP_OK;
    }

    const char *current_ip = wifi_manager_get_ip();
    if (!current_ip || current_ip[0] == '\0') {
        ESP_LOGI(TAG, "No IP address available");
        return ESP_OK;
    }

    const char *stored_ip = get_stored_ip_address();
    if (strcmp(current_ip, stored_ip) == 0) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "IP changed from '%s' to '%s'", stored_ip, current_ip);

    int voice = 8 + (rand() % 2);

    ip_announce_args_t *args = malloc(sizeof(ip_announce_args_t));
    if (!args) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(args->ip_address, current_ip, sizeof(args->ip_address) - 1);
    args->ip_address[sizeof(args->ip_address) - 1] = '\0';
    args->voice = voice;

    if (xTaskCreate(ip_announce_task, "ip_announce", IP_ANNOUNCE_TASK_STACK, args,
                     IP_ANNOUNCE_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start announcement task");
        free(args);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ip_announcement_mark_announced(const char *ip_address)
{
    if (!ip_address) {
        return;
    }

    store_ip_address(ip_address);
    ESP_LOGI(TAG, "Marked IP %s as announced", ip_address);
}
