#include "asset_cache.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "asset_cache";

/* The webassets partition is mounted here. It holds only templates + www
 * (the "data" settings partition is a separate mount at /data), so every
 * file under this root is a servable asset worth caching. */
#define ASSET_MOUNT "/spiffs"
#define ASSET_MAX_FILES 128
#define ASSET_MAX_PATH 160

typedef struct {
    char path[ASSET_MAX_PATH]; /* full VFS path, e.g. /spiffs/templates/home.html */
    char *data;                /* NUL-terminated file contents */
    size_t size;               /* byte length (excluding the terminator) */
} asset_entry_t;

static asset_entry_t entries[ASSET_MAX_FILES];
static int entry_count = 0;
static bool initialized = false;

/* Read one file fully into a freshly allocated, NUL-terminated buffer and
 * register it. Returns true on success. */
static bool cache_one_file(const char *full_path)
{
    if (entry_count >= ASSET_MAX_FILES) {
        ESP_LOGW(TAG, "Cache full (%d files), skipping %s", ASSET_MAX_FILES, full_path);
        return false;
    }

    FILE *f = fopen(full_path, "r");
    if (!f) {
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0) {
        fclose(f);
        return false;
    }

    /* Force PSRAM regardless of size. Plain malloc() would honor
     * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL and pin every buffer under that
     * threshold (4KB in this project) to internal DRAM -- with ~100+ small
     * template/CSS/JS files that starves the internal RAM client-task
     * stacks need (xTaskCreate stacks can't live in PSRAM), causing
     * "Failed to spawn client task" under load. */
    char *buf = heap_caps_malloc((size_t)file_size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory caching %s (%ld bytes)", full_path, file_size);
        fclose(f);
        return false;
    }

    size_t read = fread(buf, 1, (size_t)file_size, f);
    fclose(f);
    buf[read] = '\0';

    asset_entry_t *entry = &entries[entry_count];
    strncpy(entry->path, full_path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
    entry->data = buf;
    entry->size = read;
    entry_count++;
    return true;
}

/* Recursively walks dir_path, caching every regular file found at any depth.
 * Unlike SPIFFS (a flat filesystem where "templates/home.html" was always
 * one literal filename with no real directory objects, so a single
 * non-recursive readdir on the mount root used to reach everything),
 * LittleFS has genuine directories -- "templates" and "www" are real DT_DIR
 * entries under the mount root, with their contents only visible one level
 * down. d_type is used as a fast-path hint but stat() is the authoritative
 * check, since not every VFS driver populates d_type reliably. */
static void scan_directory(const char *dir_path, size_t *total_bytes)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open %s", dir_path);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char full_path[ASSET_MAX_PATH];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);
        if (n <= 0 || n >= (int)sizeof(full_path)) {
            ESP_LOGW(TAG, "Path too long, skipping: %s", ent->d_name);
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            scan_directory(full_path, total_bytes);
            continue;
        }

        if (cache_one_file(full_path)) {
            *total_bytes += entries[entry_count - 1].size;
        }
    }
    closedir(dir);
}

void asset_cache_init(void)
{
    if (initialized) {
        return;
    }
    initialized = true;

    size_t total_bytes = 0;
    scan_directory(ASSET_MOUNT, &total_bytes);
    if (entry_count == 0) {
        ESP_LOGE(TAG, "No assets cached under %s; assets will be served from flash (or 404)", ASSET_MOUNT);
    }

    ESP_LOGI(TAG, "Cached %d asset(s), %u bytes total", entry_count, (unsigned)total_bytes);
}

const char *asset_cache_get(const char *full_path, size_t *out_size)
{
    if (!full_path) {
        return NULL;
    }
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].path, full_path) == 0) {
            if (out_size) *out_size = entries[i].size;
            return entries[i].data;
        }
    }
    return NULL;
}

int asset_cache_count(void)
{
    return entry_count;
}

const char *asset_cache_path_at(int index)
{
    if (index < 0 || index >= entry_count) {
        return NULL;
    }
    return entries[index].path;
}
