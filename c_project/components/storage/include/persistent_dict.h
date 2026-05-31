#ifndef PERSISTENT_DICT_H
#define PERSISTENT_DICT_H

#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>

/**
 * Lazy-loaded persistent dictionary backed by a JSON file on SPIFFS.
 *
 * Data is only loaded from disk on first access, not during initialization.
 */
typedef struct {
    char filepath[128];
    cJSON *data;
    bool loaded;
    bool dirty;
    SemaphoreHandle_t mutex;
} persistent_dict_t;

/**
 * Open (or get existing) persistent dict for the given file path.
 *
 * Returns a singleton per filepath. Thread-safe.
 */
persistent_dict_t *persistent_dict_open(const char *filepath);

/**
 * Get a value by key. Returns NULL if key not found.
 *
 * The returned cJSON pointer is owned by the dict — do not free it.
 * WARNING: Not safe if another thread may modify the same key concurrently.
 * Use persistent_dict_get_dup() in multi-threaded contexts.
 */
cJSON *persistent_dict_get(persistent_dict_t *pd, const char *key);

/**
 * Get a deep copy of a value by key. Returns NULL if key not found.
 *
 * The returned cJSON is owned by the caller and must be freed with cJSON_Delete().
 * Thread-safe against concurrent modifications.
 */
cJSON *persistent_dict_get_dup(persistent_dict_t *pd, const char *key);

/**
 * Set a value by key. The cJSON value is adopted (owned by the dict after this call).
 */
void persistent_dict_set(persistent_dict_t *pd, const char *key, cJSON *value);

/**
 * Delete a key from the dict.
 */
void persistent_dict_delete_key(persistent_dict_t *pd, const char *key);

/**
 * Check if a key exists.
 */
bool persistent_dict_has_key(persistent_dict_t *pd, const char *key);

/**
 * Save the current state to disk (SPIFFS).
 */
esp_err_t persistent_dict_save(persistent_dict_t *pd);

/**
 * Force reload from disk on next access.
 */
void persistent_dict_invalidate(persistent_dict_t *pd);

/**
 * Get the entire data object (for iteration). Do not free.
 */
cJSON *persistent_dict_get_all(persistent_dict_t *pd);

#endif /* PERSISTENT_DICT_H */
