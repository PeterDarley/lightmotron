#ifndef COLORS_H
#define COLORS_H

#include "leds.h"
#include "cJSON.h"

/**
 * Resolve a built-in color name to RGB.
 *
 * Exact, case-sensitive match against the built-in name table only (mirrors
 * a Python dict lookup) - custom colors are not consulted here. Unknown
 * names default to white (255, 255, 255), matching Lighting.get_color().
 */
rgb_t color_resolve_name(const char *name);

/**
 * Parse a hex color string (#RRGGBB or RRGGBB) to RGB.
 *
 * Not used when resolving pattern/filter/effect color specs at runtime -
 * hex strings are only converted to RGB once, when a custom color is saved
 * via the web UI (see lights.settings["custom_colors"]).
 */
rgb_t color_parse_hex(const char *hex);

/**
 * Resolve any color specification (a "custom:name" reference, a built-in
 * name, or a JSON array) to RGB. Mirrors Lighting.get_color().
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
