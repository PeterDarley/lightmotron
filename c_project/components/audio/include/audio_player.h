#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>

/**
 * Initialize audio player modules from config array.
 */
esp_err_t audio_player_init_from_config(const cJSON *audio_players_config);

/**
 * Reset all audio modules.
 */
void audio_player_reset_all(void);

/**
 * Play a file on an available module.
 *
 * high_quality_preferred: prefer modules marked as high quality.
 * Returns the module index used, or -1 on failure.
 */
int audio_player_play_file(int file_number, bool high_quality_preferred);

/**
 * Stop playback on a specific module.
 */
esp_err_t audio_player_stop_module(int module_index);

/**
 * Stop all modules.
 */
void audio_player_stop_all(void);

/**
 * Set volume on all modules (0-30).
 */
void audio_player_set_volume(int volume);

/**
 * Get current volume level.
 */
int audio_player_get_volume(void);

/**
 * Check health of all modules.
 */
cJSON *audio_player_check_health(void);

/**
 * Query if a specific module is still playing.
 */
bool audio_player_is_module_playing(int module_index);

/**
 * Get number of initialized modules.
 */
int audio_player_get_module_count(void);

#endif /* AUDIO_PLAYER_H */
