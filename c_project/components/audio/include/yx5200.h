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
    bool responsive;                 /* False once no_response_streak crosses the
                                       * threshold - no physical module detected (or
                                       * it stopped responding). Reset to true the
                                       * moment a valid status frame is received
                                       * again, so a module plugged in later recovers
                                       * on its own. */
    int no_response_streak;          /* Consecutive inconclusive/no-frame status
                                       * reads, see yx5200_query_status(). */
    bool known_present;              /* Sticky - set true the first time this module
                                       * ever answers a status query, never cleared
                                       * again. Distinguishes "definitely nothing ever
                                       * attached" from "was proven real, currently
                                       * just quiet" -- see yx5200_could_recover(). */
    uint32_t reprobe_after_ms;       /* Boot-relative ms timestamp (esp_log_timestamp())
                                       * at which an unresponsive module is next eligible
                                       * for a recovery query, see yx5200_recover_if_due(). */
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

/**
 * True if the module has answered a status query recently enough to be
 * considered physically present. False after NO_RESPONSE_THRESHOLD
 * consecutive inconclusive reads (see yx5200_query_status()) -- e.g. no
 * module wired up at all.
 */
bool yx5200_is_responsive(const yx5200_t *player);

/**
 * If the module is currently marked unresponsive and its reprobe cooldown
 * has elapsed, issues a fresh status query to give it a chance to recover.
 * yx5200_is_responsive()'s doc comment promises a module "recovers on its
 * own" once it answers again, but that can only actually happen if
 * something queries it -- callers that skip unresponsive modules outright
 * for performance (like audio_player_play_file()) should call this first
 * instead of checking yx5200_is_responsive() directly, or a module that
 * had one bad second (busy loading the next track, not physically gone)
 * stays marked dead for the rest of the boot. No-op if already responsive
 * or the cooldown hasn't elapsed yet, so a genuinely-absent module still
 * isn't queried on every single call.
 */
void yx5200_recover_if_due(yx5200_t *player);

/**
 * True if this module is either currently responsive, or has proven itself
 * physically present at some point since boot (see known_present). Used by
 * callers deciding whether a retry loop is worth continuing: a module that
 * has genuinely never answered once should give up quickly, but one that
 * played real audio earlier and has simply gone quiet (e.g. busy decoding,
 * not ignoring status queries forever) deserves to keep being retried
 * rather than being treated the same as hardware that was never there.
 */
bool yx5200_could_recover(const yx5200_t *player);

#endif /* YX5200_H */
