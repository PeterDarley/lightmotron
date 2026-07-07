#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * WiFi connection states.
 */
typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED,
} wifi_state_t;

/**
 * Initialize the WiFi manager (event loop, netif, WiFi driver).
 *
 * Must be called once before connect.
 */
esp_err_t wifi_manager_init(void);

/**
 * Set the DHCP/mDNS hostname advertised by the STA interface.
 *
 * Mirrors comms.py's use of MicroPython's network.hostname(), which is set
 * before connecting so the hostname is used for the whole session. Safe to
 * call before or after the interface has connected.
 */
esp_err_t wifi_manager_set_hostname(const char *hostname);

/**
 * Connect to a WiFi network in STA mode.
 *
 * Before connecting, scans for nearby networks and resolves *ssid* to the
 * exact-case broadcast SSID (case-insensitive match), mirroring
 * comms.py's WIFIManager._resolve_ssid() so stored credentials with
 * different capitalisation still connect.
 *
 * Blocks until connected or timeout (10s).
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * Disconnect from the current network.
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * Get the current WiFi state.
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * Get the current IP address as a string.
 *
 * Returns pointer to static buffer. Empty string if not connected.
 */
const char *wifi_manager_get_ip(void);

/**
 * Get the current RSSI (signal strength).
 */
int8_t wifi_manager_get_rssi(void);

/**
 * Check if WiFi is currently connected.
 */
bool wifi_manager_is_connected(void);

#endif /* WIFI_MANAGER_H */
