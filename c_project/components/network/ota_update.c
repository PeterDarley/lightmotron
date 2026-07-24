#include "ota_update.h"
#include "persistent_dict.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "ota_update";

#define GITHUB_API_URL_FMT "https://api.github.com/repos/%s/%s/releases/latest"
#define OTA_HTTP_RESPONSE_CAPACITY 8192

/* Defaults mirror web/views.py's _ota_repo_settings() fallback values */
#define DEFAULT_OTA_REPO_OWNER "PeterDarley"
#define DEFAULT_OTA_REPO_NAME "lightmotron"

/* Current firmware version (set at build time) */
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

/**
 * Read repo_owner/repo_name from system_settings.ota, falling back to the
 * same defaults used by web/views.py's _ota_repo_settings().
 */
static void load_repo_settings(char *repo_owner, size_t owner_len, char *repo_name, size_t name_len)
{
    strncpy(repo_owner, DEFAULT_OTA_REPO_OWNER, owner_len - 1);
    repo_owner[owner_len - 1] = '\0';
    strncpy(repo_name, DEFAULT_OTA_REPO_NAME, name_len - 1);
    repo_name[name_len - 1] = '\0';

    persistent_dict_t *sys = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *ota_settings = persistent_dict_get(sys, "ota");
    if (!ota_settings) {
        return;
    }

    cJSON *owner_json = cJSON_GetObjectItem(ota_settings, "repo_owner");
    if (owner_json && cJSON_IsString(owner_json) && owner_json->valuestring[0] != '\0') {
        strncpy(repo_owner, owner_json->valuestring, owner_len - 1);
        repo_owner[owner_len - 1] = '\0';
    }

    cJSON *name_json = cJSON_GetObjectItem(ota_settings, "repo_name");
    if (name_json && cJSON_IsString(name_json) && name_json->valuestring[0] != '\0') {
        strncpy(repo_name, name_json->valuestring, name_len - 1);
        repo_name[name_len - 1] = '\0';
    }
}

typedef struct {
    char *buffer;
    int size;
    int capacity;
} http_response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_buffer_t *resp = (http_response_buffer_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && resp && resp->buffer) {
        int copy_len = evt->data_len;
        if (resp->size + copy_len >= resp->capacity) {
            copy_len = resp->capacity - resp->size - 1;
        }
        if (copy_len > 0) {
            memcpy(resp->buffer + resp->size, evt->data, copy_len);
            resp->size += copy_len;
            resp->buffer[resp->size] = '\0';
        }
    }

    return ESP_OK;
}

/**
 * Find the first release asset whose filename ends in ".bin" and copy its
 * browser_download_url into *out_url*. Returns true if found.
 */
static bool find_firmware_asset_url(const cJSON *release, char *out_url, size_t out_url_len)
{
    cJSON *assets = cJSON_GetObjectItem(release, "assets");
    if (!assets || !cJSON_IsArray(assets)) {
        return false;
    }

    int asset_count = cJSON_GetArraySize(assets);
    for (int i = 0; i < asset_count; i++) {
        cJSON *asset = cJSON_GetArrayItem(assets, i);
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        cJSON *download_url = cJSON_GetObjectItem(asset, "browser_download_url");

        if (!name || !cJSON_IsString(name) || !download_url || !cJSON_IsString(download_url)) {
            continue;
        }

        size_t name_len = strlen(name->valuestring);
        if (name_len > 4 && strcmp(name->valuestring + name_len - 4, ".bin") == 0) {
            strncpy(out_url, download_url->valuestring, out_url_len - 1);
            out_url[out_url_len - 1] = '\0';
            return true;
        }
    }

    return false;
}

/**
 * Check the configured GitHub repository's latest release for a newer
 * firmware build.
 *
 * NOTE: this is an architectural adaptation, not a line-for-line port of
 * lib/ota_update.py. The Python OTAUpdater syncs individual tracked files
 * from a git tree (appropriate for MicroPython, which runs interpreted
 * .py source from a filesystem). A compiled ESP-IDF firmware image can't
 * be patched file-by-file; the only sound equivalent is to flash a whole
 * new firmware binary via ESP-IDF's OTA partition mechanism, so this
 * checks GitHub's "latest release" API for a `.bin` asset instead of
 * walking the repo's git tree. See c_plan.md's OTA section, which already
 * specified this GitHub-releases + esp_https_ota approach.
 */
esp_err_t ota_check_for_update(ota_update_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(ota_update_info_t));
    /* Strip a leading v/V the same way the release tag is stripped below,
     * so a build made exactly at tag "v1.2.0" compares equal to a GitHub
     * release also tagged "v1.2.0" rather than always looking "different". */
    const char *build_version = FIRMWARE_VERSION;
    if (build_version[0] == 'v' || build_version[0] == 'V') {
        build_version++;
    }
    strncpy(info->current_version, build_version, sizeof(info->current_version) - 1);

    char repo_owner[64];
    char repo_name[64];
    load_repo_settings(repo_owner, sizeof(repo_owner), repo_name, sizeof(repo_name));

    char url[256];
    snprintf(url, sizeof(url), GITHUB_API_URL_FMT, repo_owner, repo_name);

    http_response_buffer_t resp = {0};
    resp.buffer = malloc(OTA_HTTP_RESPONSE_CAPACITY);
    if (!resp.buffer) {
        ESP_LOGE(TAG, "OTA check: out of memory");
        return ESP_ERR_NO_MEM;
    }
    resp.capacity = OTA_HTTP_RESPONSE_CAPACITY;
    resp.buffer[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buffer);
        ESP_LOGE(TAG, "OTA check: failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "User-Agent", "lightmotron-ota");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGW(TAG, "OTA check failed: err=%s status=%d", esp_err_to_name(err), status_code);
        free(resp.buffer);
        /* Not fatal -- leave update_available = false and let the caller retry later */
        return ESP_OK;
    }

    cJSON *release = cJSON_Parse(resp.buffer);
    free(resp.buffer);
    if (!release) {
        ESP_LOGW(TAG, "OTA check: failed to parse release JSON");
        return ESP_OK;
    }

    cJSON *tag = cJSON_GetObjectItem(release, "tag_name");
    const char *tag_str = (tag && cJSON_IsString(tag)) ? tag->valuestring : "";
    const char *version_str = tag_str;
    if (version_str[0] == 'v' || version_str[0] == 'V') {
        version_str++;
    }
    strncpy(info->latest_version, version_str, sizeof(info->latest_version) - 1);

    bool has_asset = find_firmware_asset_url(release, info->download_url, sizeof(info->download_url));

    if (tag_str[0] != '\0' && has_asset && strcmp(info->latest_version, info->current_version) != 0) {
        info->update_available = true;
    }

    cJSON_Delete(release);

    ESP_LOGI(TAG, "OTA check: current=%s latest=%s update_available=%d",
             info->current_version, info->latest_version, info->update_available);

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
        .crt_bundle_attach = esp_crt_bundle_attach,
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
