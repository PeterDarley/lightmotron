#ifndef IP_ANNOUNCEMENT_H
#define IP_ANNOUNCEMENT_H

#include "esp_err.h"

/**
 * Check whether the device's current WiFi IP address differs from the last
 * stored one and, if audio players are configured, announce the new IP via
 * a background task without blocking the caller.
 *
 * Silently does nothing if no audio players are configured or the WiFi
 * interface has no IP yet. Mirrors ip_announcement.py's
 * check_and_announce_ip().
 */
esp_err_t ip_announcement_check_and_announce(void);

/**
 * Mark an IP address as already announced so it won't be announced again.
 *
 * Intended to be called from the web UI when the user confirms they've
 * heard/recorded the address. Mirrors ip_announcement.py's
 * set_ip_announced().
 */
void ip_announcement_mark_announced(const char *ip_address);

#endif /* IP_ANNOUNCEMENT_H */
