#ifndef BOOT_H
#define BOOT_H

#include "esp_err.h"

/**
 * Initialize all system components in the correct order.
 *
 * Sequence: NVS → SPIFFS → Storage defaults → WiFi → mDNS → Web server →
 * Audio → LEDs → Billboard → Animation
 */
esp_err_t boot_init(void);

/**
 * Seed default values into persistent storage if not already present.
 */
esp_err_t boot_seed_defaults(void);

#endif /* BOOT_H */
