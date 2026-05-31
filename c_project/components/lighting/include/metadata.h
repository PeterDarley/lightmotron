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

#endif /* METADATA_H */
