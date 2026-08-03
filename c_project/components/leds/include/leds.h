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

/* Longest supported color-order string plus NUL, e.g. "RGBW". Any 3 or 4
 * character permutation of R/G/B/W is accepted (matches lib/leds.py); an
 * invalid or wrong-length string falls back to "GRB" at init time. */
#define LED_COLOR_ORDER_MAXLEN 5

/* Max per-strip color-order override ranges (generous headroom above the
 * realistic single-digit case; only bounds this fixed-size runtime config,
 * not JSON storage, which holds an unbounded list). */
#define MAX_SEGMENTS_PER_STRIP 4

/**
 * One LED-index range (inclusive, local to its owning strip) that uses a
 * different color order than the strip's own base color_order. Mirrors
 * lib/leds.py's per-strip "segments" config. Must have the same channel
 * count as the owning strip's color_order -- mixing channel counts within
 * one strip isn't supported (see leds.c).
 */
typedef struct {
    int start;
    int end;
    char color_order[LED_COLOR_ORDER_MAXLEN];
} strip_segment_config_t;

/**
 * NeoPixel strip configuration.
 */
typedef struct {
    int pin;
    int num_leds;
    char color_order[LED_COLOR_ORDER_MAXLEN];
    bool brightness_curve;
    strip_segment_config_t segments[MAX_SEGMENTS_PER_STRIP];
    int num_segments;
} strip_config_t;

/* Default GPIO pin for an onboard RGB NeoPixel used by some ESP32 dev
 * boards (mirrors DEFAULT_ONBOARD_NEOPIXEL_PIN in lib/leds.py). */
#define DEFAULT_ONBOARD_NEOPIXEL_PIN 48

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
 * Fill every pixel (across all strips) with the given color. Does not push
 * to hardware until leds_show() is called. Mirrors LEDs.fill().
 */
void leds_fill(uint8_t r, uint8_t g, uint8_t b);

/**
 * Set pixels [start, end) (end exclusive, clamped to the valid range) to
 * the given color. Mirrors LEDs.range().
 */
void leds_range(int start, int end, uint8_t r, uint8_t g, uint8_t b);

/**
 * Turn the given pixel indexes white and all others black, then push to
 * hardware immediately. Mirrors LEDs.identify().
 */
void leds_identify(const int *indexes, int count);

/**
 * Generate a rainbow color for position 0-255. Mirrors LEDs.wheel().
 */
rgb_t leds_wheel(int pos);

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
 * Set the overall brightness scale (0.0-1.0, clamped). Applied on top of
 * the per-pixel brightness curve in leds_show(). Mirrors the LEDs.brightness
 * property.
 */
void leds_set_brightness(float brightness);

/**
 * Get the current overall brightness scale (0.0-1.0).
 */
float leds_get_brightness(void);

/**
 * Apply the quadratic brightness curve to a single channel value (matches
 * LEDs._apply_brightness_curve_to_rgb's curve_component helper).
 */
uint8_t leds_apply_brightness_curve(uint8_t value);

/**
 * Initialize a single onboard RGB NeoPixel, bypassing the configured
 * strips entirely (used e.g. for boot-time IP-address flashing). Pass a
 * negative pin to use DEFAULT_ONBOARD_NEOPIXEL_PIN. Mirrors OnboardLED.
 */
esp_err_t onboard_led_init(int pin);

/**
 * Set the onboard NeoPixel to the given RGB color (0-255 per channel). No
 * brightness/gamma scaling is applied, matching OnboardLED.set().
 */
void onboard_led_set(uint8_t r, uint8_t g, uint8_t b);

/**
 * Turn the onboard NeoPixel off. Mirrors OnboardLED.off().
 */
void onboard_led_off(void);

#endif /* LEDS_H */
