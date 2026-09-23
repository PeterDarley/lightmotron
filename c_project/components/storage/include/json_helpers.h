#ifndef JSON_HELPERS_H
#define JSON_HELPERS_H

#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Get a string value from a JSON object, with a default fallback.
 */
const char *json_get_string(const cJSON *obj, const char *key, const char *default_val);

/**
 * Get an integer value from a JSON object, with a default fallback.
 */
int json_get_int(const cJSON *obj, const char *key, int default_val);

/**
 * Get a boolean value from a JSON object, with a default fallback.
 */
bool json_get_bool(const cJSON *obj, const char *key, bool default_val);

/**
 * Get a double value from a JSON object, with a default fallback.
 */
double json_get_double(const cJSON *obj, const char *key, double default_val);

/**
 * Get a nested object from a JSON object. Returns NULL if not found.
 */
cJSON *json_get_object(const cJSON *obj, const char *key);

/**
 * Get a nested array from a JSON object. Returns NULL if not found.
 */
cJSON *json_get_array(const cJSON *obj, const char *key);

/**
 * Deep clone a cJSON object.
 */
cJSON *json_deep_clone(const cJSON *obj);

/**
 * Read a JSON file from LittleFS and parse it. Caller must free with cJSON_Delete().
 */
cJSON *json_read_file(const char *filepath);

/**
 * Write a cJSON object to a file on LittleFS (temp file + atomic rename).
 * Once json_writer_init() has run, the write is performed on a dedicated
 * task pinned to core 0 and this call blocks until it completes.
 */
esp_err_t json_write_file(const char *filepath, const cJSON *obj);

/**
 * Start the core-0 writer task used by json_write_file(). Call once at boot,
 * after the data partition is mounted.
 */
esp_err_t json_writer_init(void);

#endif /* JSON_HELPERS_H */
