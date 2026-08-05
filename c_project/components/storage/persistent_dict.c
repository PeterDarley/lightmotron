#include "persistent_dict.h"
#include "json_helpers.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "persistent_dict";

#define MAX_DICT_INSTANCES 8

static persistent_dict_t instances[MAX_DICT_INSTANCES];
static int instance_count = 0;
static SemaphoreHandle_t global_mutex = NULL;

static void ensure_global_mutex(void)
{
    if (!global_mutex) {
        global_mutex = xSemaphoreCreateMutex();
    }
}

static void ensure_loaded(persistent_dict_t *pd)
{
    if (pd->loaded) {
        return;
    }

    cJSON *file_data = json_read_file(pd->filepath);
    if (file_data && cJSON_IsObject(file_data)) {
        pd->data = file_data;
    } else {
        /* File missing, unreadable, invalid JSON, or valid JSON that isn't an
         * object (e.g. an array or scalar) — fall back to an empty dict, same
         * as PersistentDict._ensure_loaded() in lib/storage.py. */
        if (file_data) {
            cJSON_Delete(file_data);
        }
        pd->data = cJSON_CreateObject();
    }
    pd->loaded = true;
    pd->dirty = false;
}

persistent_dict_t *persistent_dict_open(const char *filepath)
{
    ensure_global_mutex();
    if (!global_mutex) {
        ESP_LOGE(TAG, "Failed to create global_mutex");
        return NULL;
    }
    xSemaphoreTake(global_mutex, portMAX_DELAY);

    /* Check if already opened */
    for (int i = 0; i < instance_count; i++) {
        if (strcmp(instances[i].filepath, filepath) == 0) {
            xSemaphoreGive(global_mutex);
            return &instances[i];
        }
    }

    /* Create new instance */
    if (instance_count >= MAX_DICT_INSTANCES) {
        ESP_LOGE(TAG, "Max dict instances reached");
        xSemaphoreGive(global_mutex);
        return NULL;
    }

    persistent_dict_t *pd = &instances[instance_count];
    strncpy(pd->filepath, filepath, sizeof(pd->filepath) - 1);
    pd->filepath[sizeof(pd->filepath) - 1] = '\0';
    pd->data = NULL;
    pd->loaded = false;
    pd->dirty = false;
    pd->mutex = xSemaphoreCreateMutex();
    if (!pd->mutex) {
        ESP_LOGE(TAG, "Failed to create mutex for %s", filepath);
        xSemaphoreGive(global_mutex);
        return NULL;
    }
    instance_count++;

    xSemaphoreGive(global_mutex);
    return pd;
}

cJSON *persistent_dict_get(persistent_dict_t *pd, const char *key)
{
    if (!pd) {
        return NULL;
    }

    xSemaphoreTake(pd->mutex, portMAX_DELAY);
    ensure_loaded(pd);
    cJSON *result = cJSON_GetObjectItem(pd->data, key);
    xSemaphoreGive(pd->mutex);
    return result;
}

cJSON *persistent_dict_get_dup(persistent_dict_t *pd, const char *key)
{
    if (!pd) {
        return NULL;
    }

    xSemaphoreTake(pd->mutex, portMAX_DELAY);
    ensure_loaded(pd);
    cJSON *item = cJSON_GetObjectItem(pd->data, key);
    cJSON *result = item ? cJSON_Duplicate(item, 1) : NULL;
    xSemaphoreGive(pd->mutex);
    return result;
}

void persistent_dict_set(persistent_dict_t *pd, const char *key, cJSON *value)
{
    if (!pd || !key || !value) {
        return;
    }

    xSemaphoreTake(pd->mutex, portMAX_DELAY);
    ensure_loaded(pd);

    /* Remove existing key if present */
    if (cJSON_HasObjectItem(pd->data, key)) {
        cJSON_DeleteItemFromObject(pd->data, key);
    }

    cJSON_AddItemToObject(pd->data, key, value);
    pd->dirty = true;
    xSemaphoreGive(pd->mutex);
}

void persistent_dict_delete_key(persistent_dict_t *pd, const char *key)
{
    if (!pd || !key) {
        return;
    }

    xSemaphoreTake(pd->mutex, portMAX_DELAY);
    ensure_loaded(pd);

    if (cJSON_HasObjectItem(pd->data, key)) {
        cJSON_DeleteItemFromObject(pd->data, key);
        pd->dirty = true;
    }

    xSemaphoreGive(pd->mutex);
}

bool persistent_dict_has_key(persistent_dict_t *pd, const char *key)
{
    if (!pd || !key) {
        return false;
    }

    xSemaphoreTake(pd->mutex, portMAX_DELAY);
    ensure_loaded(pd);
    bool result = cJSON_HasObjectItem(pd->data, key);
    xSemaphoreGive(pd->mutex);
    return result;
}

esp_err_t persistent_dict_save(persistent_dict_t *pd)
{
    if (!pd) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(pd->mutex, portMAX_DELAY);

    if (!pd->loaded || !pd->dirty) {
        xSemaphoreGive(pd->mutex);
        return ESP_OK;
    }

    esp_err_t ret = json_write_file(pd->filepath, pd->data);
    if (ret == ESP_OK) {
        pd->dirty = false;
        ESP_LOGD(TAG, "Saved: %s", pd->filepath);
    } else {
        ESP_LOGE(TAG, "Failed to save: %s", pd->filepath);
    }

    xSemaphoreGive(pd->mutex);
    return ret;
}

void persistent_dict_invalidate(persistent_dict_t *pd)
{
    if (!pd) {
        return;
    }

    xSemaphoreTake(pd->mutex, portMAX_DELAY);

    if (pd->data) {
        cJSON_Delete(pd->data);
        pd->data = NULL;
    }
    pd->loaded = false;
    pd->dirty = false;

    xSemaphoreGive(pd->mutex);
}

cJSON *persistent_dict_get_all(persistent_dict_t *pd)
{
    if (!pd) {
        return NULL;
    }

    xSemaphoreTake(pd->mutex, portMAX_DELAY);
    ensure_loaded(pd);
    cJSON *result = pd->data;
    xSemaphoreGive(pd->mutex);
    return result;
}
