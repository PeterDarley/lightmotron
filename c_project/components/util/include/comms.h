#ifndef COMMS_H
#define COMMS_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize the onboard status LED (if available).
 */
esp_err_t comms_init_status_led(int gpio_pin);

/**
 * Set status LED state.
 */
void comms_set_status_led(bool on);

/**
 * Blink the status LED n times with given on/off durations.
 */
void comms_blink_status_led(int count, int on_ms, int off_ms);

/**
 * Get WiFi connection status string.
 */
const char *comms_get_wifi_status(void);

/**
 * Get IP address as string. Returns "0.0.0.0" if not connected.
 */
const char *comms_get_ip_address(void);

/**
 * Human-readable name for the chip's last reset reason (power-on, brownout,
 * watchdog, panic, ...). Mirrors reset_cause_name() in lib/utils.py.
 */
const char *comms_reset_reason_name(void);

#endif /* COMMS_H */
