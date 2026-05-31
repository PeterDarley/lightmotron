#ifndef MDNS_SETUP_H
#define MDNS_SETUP_H

#include "esp_err.h"

/**
 * Initialize mDNS with the given hostname.
 *
 * Registers <hostname>.local for HTTP service discovery.
 */
esp_err_t mdns_setup_init(const char *hostname);

/**
 * Update the mDNS hostname (e.g. after user changes it in settings).
 */
esp_err_t mdns_setup_set_hostname(const char *hostname);

#endif /* MDNS_SETUP_H */
