#ifndef COLORS_H
#define COLORS_H

#include "leds.h"
#include "cJSON.h"

/**
 * Resolve a color name to RGB.
 *
 * Checks custom colors first, then built-in named colors.
 */
rgb_t color_resolve_name(const char *name);

/**
 * Parse a hex color string (#RRGGBB or RRGGBB) to RGB.
 */
rgb_t color_parse_hex(const char *hex);

/**
 * Resolve any color specification (name, hex, or JSON array).
 */
rgb_t color_resolve(const char *spec);

/**
 * Resolve a color from a cJSON value (string or array).
 */
rgb_t color_resolve_json(const cJSON *spec);

/**
 * Interpolate between two colors by factor (0.0 = a, 1.0 = b).
 */
rgb_t color_interpolate(rgb_t a, rgb_t b, float factor);

/**
 * Clamp an integer value to 0-255.
 */
uint8_t color_clamp(int value);

#endif /* COLORS_H */
