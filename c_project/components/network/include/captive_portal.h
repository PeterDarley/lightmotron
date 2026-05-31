#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include "esp_err.h"

/**
 * Start the captive portal.
 *
 * Creates a WiFi AP ("lightmotron-setup"), starts a DNS server that redirects
 * all queries to the AP IP, and serves a setup page for WiFi credential entry.
 * On successful credential submission, saves to storage and reboots.
 */
esp_err_t captive_portal_start(void);

/**
 * Stop the captive portal and clean up resources.
 */
esp_err_t captive_portal_stop(void);

#endif /* CAPTIVE_PORTAL_H */
