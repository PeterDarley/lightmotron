#ifndef BILLBOARD_H
#define BILLBOARD_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * High-level billboard interface for scrolling/static text on MAX7219 chain.
 */

/**
 * Initialize billboard from settings (BILLBOARD config).
 */
esp_err_t billboard_init(int mosi_pin, int sck_pin, int cs_pin, int num_modules, int brightness);

/**
 * Display static text at pixel position.
 */
void billboard_static_text(const char *text, int x, int y);

/**
 * Scroll text across display (non-blocking, runs in FreeRTOS task).
 */
void billboard_scroll_text(const char *text, int delay_ms, int repeat);

/**
 * Stop any active scroll.
 */
void billboard_stop_scroll(void);

/**
 * Clear the display.
 */
void billboard_clear(void);

/**
 * Set brightness (0-15).
 */
void billboard_set_brightness(int brightness);

/**
 * Check if currently scrolling.
 */
bool billboard_is_scrolling(void);

#endif /* BILLBOARD_H */
