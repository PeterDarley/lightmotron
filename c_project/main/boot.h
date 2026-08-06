#ifndef BOOT_H
#define BOOT_H

#include "esp_err.h"

/**
 * Initialize all system components in the correct order.
 *
 * Sequence: NVS → LittleFS → Storage defaults → WiFi (hostname set, then
 * connect; captive portal on missing/failed credentials) → mDNS →
 * IP announcement → onboard-LED IP flash (background task) → Web routes/
 * server → LEDs → Billboard → Audio → Lighting → Animation
 */
esp_err_t boot_init(void);

/**
 * Seed default values into persistent storage if not already present.
 */
esp_err_t boot_seed_defaults(void);

#endif /* BOOT_H */
