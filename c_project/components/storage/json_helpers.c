#include "json_helpers.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "json_helpers";

const char *json_get_string(const cJSON *obj, const char *key, const char *default_val)
{
    if (!obj || !key) {
        return default_val;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return default_val;
}

int json_get_int(const cJSON *obj, const char *key, int default_val)
{
    if (!obj || !key) {
        return default_val;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_val;
}

bool json_get_bool(const cJSON *obj, const char *key, bool default_val)
{
    if (!obj || !key) {
        return default_val;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_val;
}

double json_get_double(const cJSON *obj, const char *key, double default_val)
{
    if (!obj || !key) {
        return default_val;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    return default_val;
}

cJSON *json_get_object(const cJSON *obj, const char *key)
{
    if (!obj || !key) {
        return NULL;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsObject(item)) {
        return item;
    }
    return NULL;
}

cJSON *json_get_array(const cJSON *obj, const char *key)
{
    if (!obj || !key) {
        return NULL;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsArray(item)) {
        return item;
    }
    return NULL;
}

cJSON *json_deep_clone(const cJSON *obj)
{
    if (!obj) {
        return NULL;
    }
    return cJSON_Duplicate(obj, 1);
}

cJSON *json_read_file(const char *filepath)
{
    if (!filepath) {
        return NULL;
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGD(TAG, "File not found: %s", filepath);
        return NULL;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 256 * 1024) {
        if (file_size > 256 * 1024) {
            ESP_LOGE(TAG, "File too large: %s (%ld bytes)", filepath, file_size);
        }
        fclose(f);
        return NULL;
    }

    /* Allocate buffer and read */
    char *buffer = malloc(file_size + 1);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes for %s", file_size, filepath);
        fclose(f);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, file_size, f);
    fclose(f);
    buffer[bytes_read] = '\0';

    /* Parse JSON */
    cJSON *json = cJSON_Parse(buffer);
    free(buffer);

    if (!json) {
        ESP_LOGE(TAG, "JSON parse error in %s: %s", filepath, cJSON_GetErrorPtr());
        return NULL;
    }

    return json;
}

esp_err_t json_write_file(const char *filepath, const cJSON *obj)
{
    if (!filepath || !obj) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Writes directly to filepath -- no temp-file+rename dance. That
     * pattern (write to "file.tmp", then rename over the real file) is a
     * carryover from SPIFFS, which didn't guarantee a single file's
     * writes were atomic. LittleFS already does: per its own design docs,
     * a file's contents+attrs are committed atomically on sync/close, and
     * a power-loss mid-write reverts to the file's previous committed
     * state rather than leaving it torn. So the app-level atomicity the
     * tmp+rename dance was providing is redundant here.
     *
     * It was also expensive: every "Interrupt wdt timeout on CPU1" panic
     * in this investigation landed inside lfs_dir_splittingcompact, which
     * only runs *during* a directory-metadata commit -- and the old
     * tmp+rename version did 3 of those per save (create .tmp, remove old
     * file, rename), instead of the 1 a direct write needs. Timing
     * checkpoints kept (though there are fewer steps now) in case a
     * single commit can still stall on its own. */
    uint32_t t_start = esp_log_timestamp();

    char *json_str = cJSON_PrintUnformatted(obj);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize JSON for %s", filepath);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "write %s: t+%ums opening", filepath, (unsigned)(esp_log_timestamp() - t_start));
    FILE *f = fopen(filepath, "w");
    ESP_LOGI(TAG, "write %s: t+%ums opened (f=%p)", filepath, (unsigned)(esp_log_timestamp() - t_start), (void *)f);
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", filepath);
        free(json_str);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    ESP_LOGI(TAG, "write %s: t+%ums writing %u bytes", filepath, (unsigned)(esp_log_timestamp() - t_start), (unsigned)len);
    size_t written = fwrite(json_str, 1, len, f);
    ESP_LOGI(TAG, "write %s: t+%ums fwrite done (%u/%u), closing", filepath,
             (unsigned)(esp_log_timestamp() - t_start), (unsigned)written, (unsigned)len);
    fclose(f);
    ESP_LOGI(TAG, "write %s: t+%ums closed/done", filepath, (unsigned)(esp_log_timestamp() - t_start));
    free(json_str);

    if (written != len) {
        ESP_LOGE(TAG, "Write incomplete for %s (%d/%d)", filepath, (int)written, (int)len);
        return ESP_FAIL;
    }

    return ESP_OK;
}
