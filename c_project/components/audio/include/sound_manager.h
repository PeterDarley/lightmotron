#ifndef SOUND_MANAGER_H
#define SOUND_MANAGER_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>

/**
 * Initialize the sound manager.
 */
esp_err_t sound_manager_init(void);

/**
 * Start the polling task that detects ended sounds for looping/chaining.
 */
esp_err_t sound_manager_start_polling(void);

/**
 * Play a sound by title.
 *
 * loop_count_override: 0 = use setting, >0 = override loop count.
 */
esp_err_t sound_manager_play(const char *title, int loop_count_override);

/**
 * Stop a sound by title.
 */
esp_err_t sound_manager_stop(const char *title);

/**
 * Stop all currently playing sounds.
 */
void sound_manager_stop_all(void);

/**
 * Check if a specific sound is playing.
 */
bool sound_manager_is_playing(const char *title);

/**
 * Get the currently playing sound titles as a JSON array.
 */
cJSON *sound_manager_get_playing(void);

#endif /* SOUND_MANAGER_H */
