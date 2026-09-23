#include "json_helpers.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

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

static cJSON *read_json_path(const char *filepath)
{
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

cJSON *json_read_file(const char *filepath)
{
    if (!filepath) {
        return NULL;
    }

    cJSON *json = read_json_path(filepath);
    if (json) {
        return json;
    }

    /* Crash recovery: json_write_file() writes "<file>.tmp" and only then
     * renames it over the real file, so if the real file is missing or
     * unreadable but a complete temp file is still around, that's the
     * newest committed copy from a save that was interrupted between
     * close and rename. */
    char tmp_path[140];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);
    json = read_json_path(tmp_path);
    if (json) {
        ESP_LOGW(TAG, "%s unreadable -- recovered from %s", filepath, tmp_path);
    }
    return json;
}

/* Does the actual write; always runs on core 0 once json_writer_init() has
 * been called -- see json_write_file(). */
static esp_err_t write_file_now(const char *filepath, const cJSON *obj)
{
    /* Write to "<file>.tmp" first, then rename over the real file. This
     * is NOT redundant with LittleFS's own atomicity, contrary to an
     * earlier version of this comment: esp_littlefs syncs at open, and
     * fopen(path, "w") truncates, so opening the real file for writing
     * commits an EMPTY file immediately -- any crash between open and
     * close (and this codebase has had several, inside LittleFS metadata
     * compaction) wipes the data. Writing to a temp file leaves the real
     * one untouched until rename, which LittleFS performs as a single
     * atomic commit that replaces the destination. No remove() first:
     * that would open a window with no file at all. */
    uint32_t t_start = esp_log_timestamp();

    char *json_str = cJSON_PrintUnformatted(obj);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize JSON for %s", filepath);
        return ESP_FAIL;
    }

    char tmp_path[140];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", tmp_path);
        free(json_str);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fclose(f);
    free(json_str);

    if (written != len) {
        ESP_LOGE(TAG, "Write incomplete for %s (%d/%d)", tmp_path, (int)written, (int)len);
        remove(tmp_path);
        return ESP_FAIL;
    }

    if (rename(tmp_path, filepath) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s -> %s (old file left intact)", tmp_path, filepath);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wrote %s (%u bytes) in %ums on core %d", filepath, (unsigned)len,
             (unsigned)(esp_log_timestamp() - t_start), (int)xPortGetCoreID());

    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Core-0 writer task.
 *
 * Every "Interrupt wdt timeout on CPU1" panic during a settings save had the
 * same shape: the write ran on CPU1, CPU0 was parked in
 * spi_flash_op_block_func (with its non-IRAM interrupts already masked), and
 * CPU1 -- still in spi_flash_disable_interrupts_caches_and_other_cpu, just
 * before masking its OWN non-IRAM interrupts -- got stuck servicing a shared
 * interrupt (shared_intr_isr) that re-fired continuously for the full 8s
 * watchdog window. When a flash op runs on core 0 instead, core 1 is the one
 * parked, and spi_flash_op_block_func masks core 1's non-IRAM interrupts
 * before anything else, so whatever storms on core 1 can't run during the
 * op. All saves are therefore funneled through one task pinned to core 0
 * (which also serializes concurrent saves as a side benefit).
 * ------------------------------------------------------------------- */

#define WRITER_TASK_STACK 8192  /* ~2KB measured for the deepest LittleFS commit path; generous margin */

typedef struct {
    const char *filepath;
    const cJSON *obj;
    esp_err_t result;
    TaskHandle_t caller;
} write_request_t;

static QueueHandle_t s_write_queue = NULL;
static TaskHandle_t s_writer_task = NULL;

static void writer_task(void *arg)
{
    (void)arg;
    write_request_t *req;
    for (;;) {
        if (xQueueReceive(s_write_queue, &req, portMAX_DELAY) == pdTRUE) {
            req->result = write_file_now(req->filepath, req->obj);
            xTaskNotifyGive(req->caller);
        }
    }
}

esp_err_t json_writer_init(void)
{
    if (s_write_queue) return ESP_OK;

    s_write_queue = xQueueCreate(4, sizeof(write_request_t *));
    if (!s_write_queue) return ESP_ERR_NO_MEM;

    if (xTaskCreatePinnedToCore(writer_task, "json_writer", WRITER_TASK_STACK, NULL, 5,
                                &s_writer_task, 0) != pdPASS) {
        vQueueDelete(s_write_queue);
        s_write_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t json_write_file(const char *filepath, const cJSON *obj)
{
    if (!filepath || !obj) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Before json_writer_init() (boot-time default seeding, which runs from
     * app_main -- itself pinned to core 0) write directly. */
    if (!s_write_queue || xTaskGetCurrentTaskHandle() == s_writer_task) {
        return write_file_now(filepath, obj);
    }

    write_request_t req = {
        .filepath = filepath,
        .obj = obj,
        .result = ESP_FAIL,
        .caller = xTaskGetCurrentTaskHandle(),
    };
    write_request_t *req_ptr = &req;
    xQueueSend(s_write_queue, &req_ptr, portMAX_DELAY);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return req.result;
}
