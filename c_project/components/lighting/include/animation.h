#ifndef ANIMATION_H
#define ANIMATION_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * Callback type for animation stop event.
 */
typedef void (*animation_stop_cb_t)(void);

/**
 * Start the animation tick loop (FreeRTOS task at 40Hz).
 */
esp_err_t animation_start(void);

/**
 * Stop the animation tick loop.
 *
 * Callbacks fire after the tick loop exits.
 */
esp_err_t animation_stop(void);

/**
 * Pause the animation (tick loop continues but does nothing).
 */
void animation_pause(void);

/**
 * Resume the animation after pause.
 */
void animation_resume(void);

/**
 * Reset animation state (clears all jobs, resets tick counter).
 */
void animation_reset(void);

/**
 * Check if animation is currently running.
 */
bool animation_is_running(void);

/**
 * Check if animation is paused.
 */
bool animation_is_paused(void);

/**
 * Get the current tick number.
 */
uint32_t animation_get_tick(void);

/**
 * Register a callback to be called when animation stops.
 */
void animation_on_stop(animation_stop_cb_t callback);

/**
 * Set the frame interval in milliseconds (default 25ms = 40Hz).
 */
void animation_set_frame_interval(int ms);

#endif /* ANIMATION_H */
