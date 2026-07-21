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
 * Stop a sound by title (fast, fire-and-forget — used for internal
 * "stops" lists where latency matters, mirrors _stop_sounds_by_title()).
 */
esp_err_t sound_manager_stop(const char *title);

/**
 * Stop a sound by title with hardware verification (mirrors
 * SoundManager.stop_sound() in lib/sounds.py): up to 5 stop attempts per
 * candidate module, re-querying hardware status between attempts, only
 * marking the sound stopped once the hardware confirms. Used by the web
 * UI's Stop button. Returns true if the sound was confirmed stopped.
 */
bool sound_manager_stop_verified(const char *title);

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

/**
 * Get the module index a playing sound is on, or -1 if not playing
 * (exposes the title->module mapping tracked internally so the web UI can
 * report which physical module is playing, matching Python's module_idx).
 */
int sound_manager_get_playing_module(const char *title);

/**
 * Play a soundscape by name - an ambient layer of one or more sounds
 * sequenced via "after" dependencies and independent repeat counts. Stops
 * any currently playing sounds/soundscape first.
 */
esp_err_t sound_manager_play_soundscape(const char *name);

/**
 * Get the name of the currently active soundscape, or "" if none is playing.
 * Self-heals stale state: if nothing is currently playing on any module,
 * advances (or completes) the soundscape before returning.
 */
const char *sound_manager_get_active_soundscape(void);

#endif /* SOUND_MANAGER_H */
