#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * OTA update status information.
 */
typedef struct {
    bool update_available;
    char current_version[32];
    char latest_version[32];
    char download_url[256];
} ota_update_info_t;

/**
 * Check GitHub releases for available updates.
 *
 * Populates info struct with version details.
 */
esp_err_t ota_check_for_update(ota_update_info_t *info);

/**
 * Apply the OTA update from the given URL.
 *
 * Downloads firmware to inactive partition, sets boot, reboots on success.
 */
esp_err_t ota_apply_update(const char *url);

#endif /* OTA_UPDATE_H */
