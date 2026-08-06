#include "yx5200.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "yx5200";

#define YX5200_START_BYTE 0x7E
#define YX5200_END_BYTE   0xEF
#define YX5200_VERSION    0xFF
#define YX5200_LENGTH     0x06

/* Feedback byte for frames we send: request an ACK/response from the module.
 * Response frames sent back BY the module always carry feedback=0x00, which
 * is how find_response_frame() tells a genuine response apart from an echo. */
#define YX5200_FEEDBACK_REQUEST  0x01
#define YX5200_FEEDBACK_RESPONSE 0x00

/* Commands */
#define CMD_PLAY_FILE     0x12  /* Play <n>.mp3 from the /MP3/ folder (0001-9999) */
#define CMD_SET_VOLUME    0x06
#define CMD_STOP          0x16
#define CMD_PAUSE         0x0E
#define CMD_RESUME        0x0D
#define CMD_RESET         0x0C
#define CMD_QUERY_STATUS  0x42

#define STATUS_PLAYING    0x01

#define UART_BUF_SIZE 256
#define RESPONSE_BUF_SIZE 32
#define QUERY_WAIT_MS 100
#define POST_COMMAND_DRAIN_MS 50
#define RESET_SETTLE_MS 700

/* Some modules briefly report a non-playing state while a track is still
 * audible. Require several consecutive stopped observations before clearing
 * cached playback state. Mirrors YX5200Player._required_stop_confirmations
 * in lib/audio.py. */
#define REQUIRED_STOP_CONFIRMATIONS 6

/* After this many consecutive inconclusive/no-frame status reads, stop
 * assuming the module is merely being slow and conclude nothing is
 * physically there to respond (or it's stopped responding). Mirrors
 * YX5200Player._no_response_threshold in lib/audio.py. */
#define NO_RESPONSE_THRESHOLD 5

static void build_frame(uint8_t *frame, uint8_t cmd, uint16_t param)
{
    frame[0] = YX5200_START_BYTE;
    frame[1] = YX5200_VERSION;
    frame[2] = YX5200_LENGTH;
    frame[3] = cmd;
    frame[4] = YX5200_FEEDBACK_REQUEST;
    frame[5] = (param >> 8) & 0xFF;
    frame[6] = param & 0xFF;

    /* Checksum: -(version + length + cmd + feedback + param_h + param_l) */
    int16_t checksum = -(int16_t)(frame[1] + frame[2] + frame[3] +
                                   frame[4] + frame[5] + frame[6]);
    frame[7] = (checksum >> 8) & 0xFF;
    frame[8] = checksum & 0xFF;
    frame[9] = YX5200_END_BYTE;
}

/* Hex-dumps up to `len` bytes of `data` under `label`, gated by the same
 * "yx5200" log tag audio_player_init_from_config() flips to DEBUG when
 * "audio_debug_logging" is enabled in System Settings -- no separate toggle
 * needed. Added for first-time hardware bring-up: shows exactly what went
 * out and what (if anything) came back, rather than just the pass/fail
 * conclusion the existing "inconclusive"/"no valid frame" logs give. */
static void log_hex(const char *label, int uart_num, const uint8_t *data, int len)
{
    char hex[RESPONSE_BUF_SIZE * 3 + 1];
    int off = 0;
    for (int i = 0; i < len && off < (int)sizeof(hex) - 4; i++) {
        off += snprintf(hex + off, sizeof(hex) - off, "%02X ", data[i]);
    }
    ESP_LOGD(TAG, "uart=%d %s (%d bytes): %s", uart_num, label, len, len > 0 ? hex : "(none)");
}

/* Write a command frame without waiting for/draining any response. Used by
 * yx5200_query_status(), which manages its own read timing. */
static esp_err_t send_command_raw(yx5200_t *player, uint8_t cmd, uint16_t param)
{
    uint8_t frame[10];
    build_frame(frame, cmd, param);
    log_hex("TX", player->uart_num, frame, 10);

    int written = uart_write_bytes(player->uart_num, (const char *)frame, 10);
    if (written != 10) {
        ESP_LOGE(TAG, "UART write failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Write a command frame and drain whatever trickles back shortly after, just
 * to keep the RX buffer tidy (mirrors YX5200Player.send_command in
 * lib/audio.py, which does a short best-effort read after every command). */
static esp_err_t send_command(yx5200_t *player, uint8_t cmd, uint16_t param)
{
    esp_err_t ret = send_command_raw(player, cmd, param);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(POST_COMMAND_DRAIN_MS));
    uint8_t scratch[RESPONSE_BUF_SIZE];
    int drained = uart_read_bytes(player->uart_num, scratch, sizeof(scratch), 0);
    log_hex("post-command drain", player->uart_num, scratch, drained);
    return ESP_OK;
}

/* Search raw UART bytes for a valid 10-byte DFPlayer response frame matching
 * expected_cmd. A response frame is distinguished from an echoed command by
 * its feedback byte, which the module always sets to 0x00. Mirrors
 * YX5200Player._find_response_frame in lib/audio.py. */
static bool find_response_frame(const uint8_t *data, int len, uint8_t expected_cmd,
                                 uint8_t *status_high, uint8_t *status_low)
{
    for (int i = 0; i + 9 < len; i++) {
        if (data[i] != YX5200_START_BYTE) continue;
        if (data[i + 9] != YX5200_END_BYTE) continue;
        if (data[i + 1] != YX5200_VERSION || data[i + 2] != YX5200_LENGTH) continue;
        if (data[i + 3] != expected_cmd) continue;
        if (data[i + 4] != YX5200_FEEDBACK_RESPONSE) continue;

        *status_high = data[i + 5];
        *status_low = data[i + 6];
        return true;
    }
    return false;
}

esp_err_t yx5200_init(yx5200_t *player, uart_port_t uart_num, int tx_pin, int rx_pin, bool high_quality)
{
    if (!player) return ESP_ERR_INVALID_ARG;

    player->uart_num = uart_num;
    player->tx_pin = tx_pin;
    player->rx_pin = rx_pin;
    player->is_playing = false;
    player->current_file = 0;
    player->volume = 15;
    player->initialized = false;
    player->high_quality = high_quality;
    player->pending_stop_confirmations = 0;
    player->responsive = true;   /* Optimistic until proven otherwise - see yx5200_query_status() */
    player->no_response_streak = 0;

    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(uart_num, UART_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(uart_num, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed");
        return ret;
    }

    ret = uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART pin config failed");
        return ret;
    }

    player->initialized = true;
    ESP_LOGI(TAG, "YX5200 initialized on UART%d (TX=%d, RX=%d, hq=%d)", uart_num, tx_pin, rx_pin, high_quality);
    return ESP_OK;
}

esp_err_t yx5200_play_file(yx5200_t *player, int file_number)
{
    if (!player || !player->initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = send_command(player, CMD_PLAY_FILE, (uint16_t)file_number);
    if (ret == ESP_OK) {
        player->is_playing = true;
        player->current_file = file_number;
        player->pending_stop_confirmations = 0;
    }
    return ret;
}

esp_err_t yx5200_stop(yx5200_t *player)
{
    if (!player || !player->initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = send_command(player, CMD_STOP, 0);
    if (ret == ESP_OK) {
        player->is_playing = false;
        player->current_file = 0;
        player->pending_stop_confirmations = 0;
    }
    return ret;
}

esp_err_t yx5200_set_volume(yx5200_t *player, int volume)
{
    if (!player || !player->initialized) return ESP_ERR_INVALID_STATE;
    if (volume < 0) volume = 0;
    if (volume > 30) volume = 30;

    esp_err_t ret = send_command(player, CMD_SET_VOLUME, (uint16_t)volume);
    if (ret == ESP_OK) {
        player->volume = volume;
    }
    return ret;
}

/* Called on every inconclusive/no-frame status read. After
 * NO_RESPONSE_THRESHOLD consecutive misses, concludes nothing is physically
 * there and stops treating the module as playing (so callers aren't stuck
 * thinking it's permanently busy). */
static void note_no_response(yx5200_t *player)
{
    if (player->no_response_streak < NO_RESPONSE_THRESHOLD) {
        player->no_response_streak++;
    }
    ESP_LOGD(TAG, "uart=%d no_response_streak=%d/%d", player->uart_num,
             player->no_response_streak, NO_RESPONSE_THRESHOLD);
    if (player->no_response_streak >= NO_RESPONSE_THRESHOLD && player->responsive) {
        ESP_LOGW(TAG, "uart=%d: no response after %d attempts, assuming no module attached",
                 player->uart_num, player->no_response_streak);
        player->responsive = false;
        player->is_playing = false;
        player->pending_stop_confirmations = 0;
    }
}

/* Called whenever a valid status frame is actually received - proves a
 * module is physically present and responding, even if it had previously
 * been marked unresponsive. */
static void note_response_ok(yx5200_t *player)
{
    player->no_response_streak = 0;
    if (!player->responsive) {
        ESP_LOGI(TAG, "uart=%d: responding again", player->uart_num);
        player->responsive = true;
    }
}

bool yx5200_is_responsive(const yx5200_t *player)
{
    return player && player->responsive;
}

esp_err_t yx5200_query_status(yx5200_t *player)
{
    if (!player || !player->initialized) return ESP_ERR_INVALID_STATE;

    /* Drain any stale bytes left over from a previous command before asking
     * for status, mirroring the drain in YX5200Player.query_status(). */
    uart_flush_input(player->uart_num);

    esp_err_t ret = send_command_raw(player, CMD_QUERY_STATUS, 0);
    if (ret != ESP_OK) return ret;

    /* Wait for the module to reply, then read whatever is available. The
     * buffer may contain an echoed command frame followed by the real
     * response, so we search rather than assume offset 0. */
    vTaskDelay(pdMS_TO_TICKS(QUERY_WAIT_MS));
    uint8_t response[RESPONSE_BUF_SIZE];
    int len = uart_read_bytes(player->uart_num, response, sizeof(response), 0);
    log_hex("RX", player->uart_num, response, len);

    if (len < 10) {
        /* Inconclusive read - keep cached state, same as the Python driver. */
        ESP_LOGD(TAG, "status inconclusive uart=%d keep_cached=%d", player->uart_num, player->is_playing);
        note_no_response(player);
        return ESP_OK;
    }

    uint8_t status_high = 0, status_low = 0;
    if (!find_response_frame(response, len, CMD_QUERY_STATUS, &status_high, &status_low)) {
        ESP_LOGD(TAG, "status no valid frame uart=%d keep_cached=%d", player->uart_num, player->is_playing);
        note_no_response(player);
        return ESP_OK;
    }

    note_response_ok(player);
    bool hw_playing = (status_low == STATUS_PLAYING) || (status_high == STATUS_PLAYING);
    ESP_LOGD(TAG, "uart=%d valid frame: status_high=%02X status_low=%02X hw_playing=%d",
             player->uart_num, status_high, status_low, hw_playing);

    if (!hw_playing && player->is_playing) {
        player->pending_stop_confirmations++;
        if (player->pending_stop_confirmations < REQUIRED_STOP_CONFIRMATIONS) {
            /* Not enough consecutive "stopped" readings yet - keep reporting
             * playing so a momentary false-stop doesn't get acted on. */
            return ESP_OK;
        }
    } else {
        player->pending_stop_confirmations = 0;
    }

    player->is_playing = hw_playing;
    if (!hw_playing) {
        player->current_file = 0;
    }

    return ESP_OK;
}

esp_err_t yx5200_reset(yx5200_t *player)
{
    if (!player || !player->initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = send_command(player, CMD_RESET, 0);
    if (ret == ESP_OK) {
        player->is_playing = false;
        player->current_file = 0;
        player->pending_stop_confirmations = 0;
        /* Module firmware needs a short settle period after reset. */
        ESP_LOGD(TAG, "uart=%d reset sent, settling for %dms", player->uart_num, RESET_SETTLE_MS);
        vTaskDelay(pdMS_TO_TICKS(RESET_SETTLE_MS));
        ESP_LOGD(TAG, "uart=%d reset settle done", player->uart_num);
    }
    return ret;
}
