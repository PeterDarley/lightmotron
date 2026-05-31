#include "ota_update.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota_update";

#define OTA_RECV_BUFFER_SIZE 1024
#define GITHUB_API_URL "https://api.github.com/repos/%s/releases/latest"

/* Current firmware version (set at build time) */
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

esp_err_t ota_check_for_update(ota_update_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(ota_update_info_t));
    strncpy(info->current_version, FIRMWARE_VERSION, sizeof(info->current_version) - 1);

    /* TODO: Read OTA repo URL from settings, construct GitHub API URL */
    /* For now, mark no update available */
    info->update_available = false;

    ESP_LOGI(TAG, "OTA check: current=%s", info->current_version);
    return ESP_OK;
}

esp_err_t ota_apply_update(const char *url)
{
    if (!url || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, rebooting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }

    return ret;
}
