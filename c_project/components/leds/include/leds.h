#ifndef LEDS_H
#define LEDS_H

#include "esp_err.h"
#include "cJSON.h"

#include <stdint.h>
#include <stdbool.h>

/**
 * RGB color tuple.
 */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

/**
 * Color order enum for NeoPixel strips.
 */
typedef enum {
    COLOR_ORDER_GRB,
    COLOR_ORDER_RGB,
    COLOR_ORDER_RGBW,
} color_order_t;

/**
 * NeoPixel strip configuration.
 */
typedef struct {
    int pin;
    int num_leds;
    color_order_t color_order;
    bool brightness_curve;
} strip_config_t;

/**
 * Initialize LEDs from a cJSON config array (from settings).
 */
esp_err_t leds_init_from_config(const cJSON *neopixels_array);

/**
 * Initialize LEDs with explicit strip configs.
 */
esp_err_t leds_init(const strip_config_t *configs, int num_strips);

/**
 * Set a single pixel color.
 */
void leds_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b);

/**
 * Set a pixel with an rgb_t struct.
 */
void leds_set_pixel_rgb(int index, rgb_t color);

/**
 * Get the current color of a pixel.
 */
rgb_t leds_get_pixel(int index);

/**
 * Push the current buffer to the LED hardware.
 */
esp_err_t leds_show(void);

/**
 * Clear all LEDs (set to black).
 */
void leds_clear(void);

/**
 * Get total LED count across all strips.
 */
int leds_total_count(void);

/**
 * Get the number of strips configured.
 */
int leds_strip_count(void);

/**
 * Apply gamma correction to a value.
 */
uint8_t leds_gamma_correct(uint8_t value);

#endif /* LEDS_H */
