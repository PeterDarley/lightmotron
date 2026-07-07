#ifndef YX5200_H
#define YX5200_H

#include "esp_err.h"
#include "driver/uart.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * YX5200 (DFPlayer) MP3 module driver.
 *
 * Communicates via UART with 10-byte command frames.
 */
typedef struct {
    uart_port_t uart_num;
    int tx_pin;
    int rx_pin;
    bool is_playing;
    int current_file;
    int volume;
    bool initialized;
    bool high_quality;               /* Prefer this module for high-quality sounds */
    int pending_stop_confirmations;  /* Debounce counter, see yx5200_query_status() */
} yx5200_t;

/**
 * Initialize a YX5200 module on the specified UART.
 */
esp_err_t yx5200_init(yx5200_t *player, uart_port_t uart_num, int tx_pin, int rx_pin, bool high_quality);

/**
 * Play a file by number (1-based index).
 */
esp_err_t yx5200_play_file(yx5200_t *player, int file_number);

/**
 * Stop playback.
 */
esp_err_t yx5200_stop(yx5200_t *player);

/**
 * Pause playback.
 */
esp_err_t yx5200_pause(yx5200_t *player);

/**
 * Resume playback.
 */
esp_err_t yx5200_resume(yx5200_t *player);

/**
 * Set volume (0-30).
 */
esp_err_t yx5200_set_volume(yx5200_t *player, int volume);

/**
 * Query playback status. Updates player->is_playing.
 */
esp_err_t yx5200_query_status(yx5200_t *player);

/**
 * Reset the module.
 */
esp_err_t yx5200_reset(yx5200_t *player);

#endif /* YX5200_H */
