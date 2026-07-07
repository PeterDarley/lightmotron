#ifndef METADATA_H
#define METADATA_H

#include "cJSON.h"

/**
 * Scene metadata structure (kills, sound triggers).
 */
typedef struct {
    char kills[8][64];
    int kills_count;
    char sound[64];
    char stop_sounds_on_start[8][64];
    int stop_sounds_on_start_count;
    char stop_sounds_on_end[8][64];
    int stop_sounds_on_end_count;
} scene_metadata_t;

/**
 * Get metadata for a scene.
 */
void metadata_get_scene(const char *scene_name, scene_metadata_t *meta);

/**
 * Maximum number of required/optional parameter names a single pattern or
 * filter entry can have. Large enough for every entry currently defined in
 * lib/lighting/metadata.py (wave has the most, at 4 optional params).
 */
#define METADATA_MAX_PARAMS 8

/**
 * Describes one entry of PATTERN_METADATA from lib/lighting/metadata.py:
 * a pattern's human-readable description, its required/optional parameter
 * names (used verbatim as web form field name suffixes, e.g.
 * "pattern_param_width"), and how many colors it consumes from a job's
 * "colors" list.
 */
typedef struct {
    const char *name;
    const char *description;
    const char *required[METADATA_MAX_PARAMS];
    int required_count;
    const char *optional[METADATA_MAX_PARAMS];
    int optional_count;
    int color_count;
} pattern_metadata_t;

/**
 * Describes one entry of FILTER_METADATA from lib/lighting/metadata.py.
 * Filters have no "required" param list in Python - only a description and
 * optional params.
 */
typedef struct {
    const char *name;
    const char *description;
    const char *optional[METADATA_MAX_PARAMS];
    int optional_count;
} filter_metadata_t;

/**
 * Static table mirroring PATTERN_METADATA, terminated by a {NULL, ...} entry.
 */
extern const pattern_metadata_t pattern_metadata_table[];

/**
 * Static table mirroring FILTER_METADATA, terminated by a {NULL, ...} entry.
 */
extern const filter_metadata_t filter_metadata_table[];

/**
 * Look up a pattern's metadata by name. Returns NULL if not found, matching
 * Lighting.get_pattern_metadata() returning PATTERN_METADATA (a dict lookup
 * against it would raise KeyError / miss).
 */
const pattern_metadata_t *metadata_get_pattern(const char *name);

/**
 * Look up a filter's metadata by name. Returns NULL if not found.
 */
const filter_metadata_t *metadata_get_filter(const char *name);

#endif /* METADATA_H */
