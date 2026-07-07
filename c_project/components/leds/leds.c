#include "leds.h"
#include "json_helpers.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "leds";

#define MAX_STRIPS 4
#define MAX_TOTAL_LEDS 600
#define LED_MAX_CHANNELS 4 /* R, G, B, W */

/*
 * Brightness curve lookup table. Precomputed from the same quadratic curve
 * as LEDs._apply_brightness_curve_to_rgb() in lib/leds.py:
 *   normalized = (value - 1) / 254.0
 *   adjusted   = normalized * normalized
 *   result     = 1 + int(adjusted * 254)     (value == 0 stays 0)
 * This is intentionally NOT a standard 2.2-gamma table — it must match the
 * Python curve exactly, not just look similar.
 */
static const uint8_t brightness_curve_table[256] = {
    0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
    1,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,   3,   4,   4,   4,
    4,   5,   5,   5,   5,   6,   6,   6,   6,   7,   7,   7,   8,   8,   8,   9,
    9,  10,  10,  10,  11,  11,  12,  12,  12,  13,  13,  14,  14,  15,  15,  16,
   16,  17,  17,  18,  18,  19,  19,  20,  20,  21,  21,  22,  23,  23,  24,  24,
   25,  26,  26,  27,  28,  28,  29,  30,  30,  31,  32,  32,  33,  34,  35,  35,
   36,  37,  38,  38,  39,  40,  41,  41,  42,  43,  44,  45,  46,  46,  47,  48,
   49,  50,  51,  52,  53,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
   64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  77,  78,  79,  80,
   81,  82,  83,  84,  86,  87,  88,  89,  90,  91,  93,  94,  95,  96,  98,  99,
  100, 101, 103, 104, 105, 106, 108, 109, 110, 112, 113, 114, 116, 117, 118, 120,
  121, 122, 124, 125, 127, 128, 129, 131, 132, 134, 135, 137, 138, 140, 141, 143,
  144, 146, 147, 149, 150, 152, 153, 155, 156, 158, 160, 161, 163, 164, 166, 168,
  169, 171, 172, 174, 176, 177, 179, 181, 182, 184, 186, 188, 189, 191, 193, 195,
  196, 198, 200, 202, 203, 205, 207, 209, 211, 212, 214, 216, 218, 220, 222, 224,
  225, 227, 229, 231, 233, 235, 237, 239, 241, 243, 245, 247, 249, 251, 253, 255,
};

/* Strip state */
typedef struct {
    int pin;
    int num_leds;
    int start_index; /* Global index offset */
    uint8_t order_indices[LED_MAX_CHANNELS]; /* per-output-byte source channel */
    int bpp; /* bytes per pixel: 3 (RGB-family) or 4 (RGBW-family) */
    rmt_channel_handle_t rmt_channel;
    rmt_encoder_handle_t encoder;
} strip_state_t;

static strip_state_t strips[MAX_STRIPS];
static int num_strips = 0;
static int total_leds = 0;
static rgb_t *pixel_buffer = NULL;
static uint8_t *tx_buffer = NULL; /* Pre-allocated for leds_show() */
static SemaphoreHandle_t led_mutex = NULL;

/* Overall brightness scale, 0.0-1.0. Mirrors LEDs.brightness. */
static float g_brightness = 1.0f;

/*
 * Whether the quadratic brightness curve is applied. Mirrors
 * LEDs._brightness_curve: a SINGLE flag shared by all strips, true if ANY
 * configured strip requested brightness_curve (see
 * LEDs._parse_brightness_curve_setting) -- it is not applied per-strip.
 */
static bool g_brightness_curve = false;

/* Single onboard NeoPixel, entirely separate from the strips[] above. */
static rmt_channel_handle_t onboard_rmt_channel = NULL;
static rmt_encoder_handle_t onboard_encoder = NULL;
static bool onboard_led_ready = false;

/* WS2812 timing (in RMT ticks at 10MHz resolution) */
#define WS2812_T0H_TICKS 3   /* 0.3us */
#define WS2812_T0L_TICKS 9   /* 0.9us */
#define WS2812_T1H_TICKS 9   /* 0.9us */
#define WS2812_T1L_TICKS 3   /* 0.3us */
#define WS2812_RESET_TICKS 500 /* 50us */

/**
 * RMT encoder for WS2812 LED protocol.
 */
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *primary_data, size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (ws_encoder->state) {
    case 0: /* Send pixel data */
        encoded_symbols += ws_encoder->bytes_encoder->encode(
            ws_encoder->bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws_encoder->state = 1;
            session_state = RMT_ENCODING_RESET;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            *ret_state = (rmt_encode_state_t)RMT_ENCODING_MEM_FULL;
            return encoded_symbols;
        }
        /* fall through */
    case 1: /* Send reset code */
        encoded_symbols += ws_encoder->copy_encoder->encode(
            ws_encoder->copy_encoder, channel, &ws_encoder->reset_code,
            sizeof(ws_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws_encoder->state = RMT_ENCODING_RESET;
            *ret_state = (rmt_encode_state_t)RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            *ret_state = (rmt_encode_state_t)RMT_ENCODING_MEM_FULL;
        }
        break;
    }

    return encoded_symbols;
}

static esp_err_t ws2812_encoder_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws_encoder = __containerof(encoder, ws2812_encoder_t, base);
    ws_encoder->bytes_encoder->reset(ws_encoder->bytes_encoder);
    ws_encoder->copy_encoder->reset(ws_encoder->copy_encoder);
    ws_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t ws2812_encoder_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(ws_encoder->bytes_encoder);
    rmt_del_encoder(ws_encoder->copy_encoder);
    free(ws_encoder);
    return ESP_OK;
}

static esp_err_t create_ws2812_encoder(rmt_encoder_handle_t *ret_encoder)
{
    ws2812_encoder_t *ws_encoder = calloc(1, sizeof(ws2812_encoder_t));
    if (!ws_encoder) {
        return ESP_ERR_NO_MEM;
    }

    ws_encoder->base.encode = ws2812_encode;
    ws_encoder->base.reset = ws2812_encoder_reset;
    ws_encoder->base.del = ws2812_encoder_del;

    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {
            .level0 = 1, .duration0 = WS2812_T0H_TICKS,
            .level1 = 0, .duration1 = WS2812_T0L_TICKS,
        },
        .bit1 = {
            .level0 = 1, .duration0 = WS2812_T1H_TICKS,
            .level1 = 0, .duration1 = WS2812_T1L_TICKS,
        },
        .flags.msb_first = 1,
    };

    esp_err_t ret = rmt_new_bytes_encoder(&bytes_config, &ws_encoder->bytes_encoder);
    if (ret != ESP_OK) {
        free(ws_encoder);
        return ret;
    }

    rmt_copy_encoder_config_t copy_config = {};
    ret = rmt_new_copy_encoder(&copy_config, &ws_encoder->copy_encoder);
    if (ret != ESP_OK) {
        rmt_del_encoder(ws_encoder->bytes_encoder);
        free(ws_encoder);
        return ret;
    }

    ws_encoder->reset_code = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = WS2812_RESET_TICKS,
        .level1 = 0, .duration1 = WS2812_RESET_TICKS,
    };

    *ret_encoder = &ws_encoder->base;
    return ESP_OK;
}

/* Channel index lookup: R=0 G=1 B=2 W=3, matching _CHANNEL_INDEX in
 * lib/leds.py. Returns -1 for any other character. */
static int channel_index(char c)
{
    switch (toupper((unsigned char)c)) {
    case 'R': return 0;
    case 'G': return 1;
    case 'B': return 2;
    case 'W': return 3;
    default: return -1;
    }
}

/*
 * Parse a color-order string (e.g. "GRB", "RGBW", "BRG") into per-output
 * -byte source-channel indices, matching LEDs._reorder()/order_indices in
 * lib/leds.py. Any 3- or 4-character permutation of R/G/B/W is accepted;
 * anything else (wrong length or invalid character) falls back to "GRB",
 * exactly like the Python side.
 */
static void parse_color_order(const char *str, uint8_t *order_indices, int *bpp)
{
    int len = str ? (int)strlen(str) : 0;
    bool valid = (len == 3 || len == 4);

    if (valid) {
        for (int i = 0; i < len; i++) {
            int idx = channel_index(str[i]);
            if (idx < 0) {
                valid = false;
                break;
            }
            order_indices[i] = (uint8_t)idx;
        }
    }

    if (!valid) {
        static const uint8_t grb[3] = {1, 0, 2}; /* G, R, B */
        memcpy(order_indices, grb, sizeof(grb));
        len = 3;
    }

    *bpp = len;
}

esp_err_t leds_init_from_config(const cJSON *neopixels_array)
{
    if (!neopixels_array || !cJSON_IsArray(neopixels_array)) {
        return ESP_ERR_INVALID_ARG;
    }

    int count = cJSON_GetArraySize(neopixels_array);
    if (count > MAX_STRIPS) count = MAX_STRIPS;

    strip_config_t configs[MAX_STRIPS];
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(neopixels_array, i);
        configs[i].pin = json_get_int(item, "pin", 4);
        configs[i].num_leds = json_get_int(item, "num", 144);
        const char *order = json_get_string(item, "color_order", "GRB");
        strncpy(configs[i].color_order, order ? order : "GRB", LED_COLOR_ORDER_MAXLEN - 1);
        configs[i].color_order[LED_COLOR_ORDER_MAXLEN - 1] = '\0';
        configs[i].brightness_curve = json_get_bool(item, "brightness_curve", true);
    }

    return leds_init(configs, count);
}

esp_err_t leds_init(const strip_config_t *configs, int count)
{
    if (!configs || count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    led_mutex = xSemaphoreCreateMutex();
    num_strips = count > MAX_STRIPS ? MAX_STRIPS : count;
    total_leds = 0;
    g_brightness_curve = false;

    for (int i = 0; i < num_strips; i++) {
        strips[i].pin = configs[i].pin;
        strips[i].num_leds = configs[i].num_leds;
        parse_color_order(configs[i].color_order, strips[i].order_indices, &strips[i].bpp);
        strips[i].start_index = total_leds;
        total_leds += configs[i].num_leds;

        /* Matches LEDs._parse_brightness_curve_setting(): the curve is a
         * single global toggle, enabled if ANY strip requests it. */
        if (configs[i].brightness_curve) {
            g_brightness_curve = true;
        }
    }

    if (total_leds > MAX_TOTAL_LEDS) {
        total_leds = MAX_TOTAL_LEDS;
    }

    /* Allocate pixel buffer */
    pixel_buffer = calloc(total_leds, sizeof(rgb_t));
    if (!pixel_buffer) {
        ESP_LOGE(TAG, "Failed to allocate pixel buffer");
        return ESP_ERR_NO_MEM;
    }

    /* Find largest strip for tx buffer size */
    int max_strip_leds = 0;
    for (int i = 0; i < num_strips; i++) {
        if (strips[i].num_leds > max_strip_leds) {
            max_strip_leds = strips[i].num_leds;
        }
    }

    /* 4 bytes/pixel to accommodate RGBW-family strips (bpp==4). */
    tx_buffer = malloc(max_strip_leds * LED_MAX_CHANNELS);
    if (!tx_buffer) {
        ESP_LOGE(TAG, "Failed to allocate tx buffer");
        free(pixel_buffer);
        pixel_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Initialize RMT channels */
    for (int i = 0; i < num_strips; i++) {
        rmt_tx_channel_config_t tx_config = {
            .gpio_num = strips[i].pin,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10000000, /* 10MHz = 0.1us per tick */
            .mem_block_symbols = 64,
            .trans_queue_depth = 4,
        };

        esp_err_t ret = rmt_new_tx_channel(&tx_config, &strips[i].rmt_channel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create RMT channel for strip %d: %s",
                     i, esp_err_to_name(ret));
            return ret;
        }

        ret = create_ws2812_encoder(&strips[i].encoder);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create encoder for strip %d", i);
            return ret;
        }

        rmt_enable(strips[i].rmt_channel);
    }

    ESP_LOGI(TAG, "LEDs initialized: %d strips, %d total LEDs", num_strips, total_leds);
    return ESP_OK;
}

void leds_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index < 0 || index >= total_leds) {
        return;
    }

    xSemaphoreTake(led_mutex, portMAX_DELAY);
    pixel_buffer[index].r = r;
    pixel_buffer[index].g = g;
    pixel_buffer[index].b = b;
    xSemaphoreGive(led_mutex);
}

void leds_set_pixel_rgb(int index, rgb_t color)
{
    leds_set_pixel(index, color.r, color.g, color.b);
}

rgb_t leds_get_pixel(int index)
{
    rgb_t black = {0, 0, 0};
    if (index < 0 || index >= total_leds) {
        return black;
    }

    xSemaphoreTake(led_mutex, portMAX_DELAY);
    rgb_t result = pixel_buffer[index];
    xSemaphoreGive(led_mutex);
    return result;
}

void leds_fill(uint8_t r, uint8_t g, uint8_t b)
{
    if (!pixel_buffer) return;

    xSemaphoreTake(led_mutex, portMAX_DELAY);
    for (int i = 0; i < total_leds; i++) {
        pixel_buffer[i].r = r;
        pixel_buffer[i].g = g;
        pixel_buffer[i].b = b;
    }
    xSemaphoreGive(led_mutex);
}

void leds_range(int start, int end, uint8_t r, uint8_t g, uint8_t b)
{
    if (!pixel_buffer) return;

    if (start < 0) start = 0;
    if (end > total_leds) end = total_leds;

    xSemaphoreTake(led_mutex, portMAX_DELAY);
    for (int i = start; i < end; i++) {
        pixel_buffer[i].r = r;
        pixel_buffer[i].g = g;
        pixel_buffer[i].b = b;
    }
    xSemaphoreGive(led_mutex);
}

void leds_identify(const int *indexes, int count)
{
    if (!pixel_buffer) return;

    xSemaphoreTake(led_mutex, portMAX_DELAY);
    for (int i = 0; i < total_leds; i++) {
        pixel_buffer[i].r = 0;
        pixel_buffer[i].g = 0;
        pixel_buffer[i].b = 0;
    }
    for (int i = 0; indexes && i < count; i++) {
        int idx = indexes[i];
        if (idx >= 0 && idx < total_leds) {
            pixel_buffer[idx].r = 255;
            pixel_buffer[idx].g = 255;
            pixel_buffer[idx].b = 255;
        }
    }
    xSemaphoreGive(led_mutex);

    leds_show();
}

rgb_t leds_wheel(int pos)
{
    pos = ((pos % 256) + 256) % 256; /* Match Python's `pos % 256` for negatives too */

    if (pos < 85) {
        rgb_t c = {(uint8_t)(255 - pos * 3), (uint8_t)(pos * 3), 0};
        return c;
    }
    if (pos < 170) {
        pos -= 85;
        rgb_t c = {0, (uint8_t)(255 - pos * 3), (uint8_t)(pos * 3)};
        return c;
    }
    pos -= 170;
    rgb_t c = {(uint8_t)(pos * 3), 0, (uint8_t)(255 - pos * 3)};
    return c;
}

esp_err_t leds_show(void)
{
    xSemaphoreTake(led_mutex, portMAX_DELAY);

    for (int s = 0; s < num_strips; s++) {
        int num = strips[s].num_leds;
        int start = strips[s].start_index;
        int bpp = strips[s].bpp;

        /* Build transmission buffer with brightness curve + color order */
        uint8_t *tx_buf = tx_buffer;

        for (int i = 0; i < num; i++) {
            rgb_t pixel = pixel_buffer[start + i];
            uint8_t r = pixel.r, g = pixel.g, b = pixel.b;

            /* Apply the quadratic brightness curve (global flag; see
             * g_brightness_curve). Matches LEDs._scale()'s curve step. */
            if (g_brightness_curve) {
                r = brightness_curve_table[r];
                g = brightness_curve_table[g];
                b = brightness_curve_table[b];
            }

            /* Apply overall brightness scale. Matches LEDs._scale()'s
             * `if brightness >= 0.999: return color` short-circuit. */
            if (g_brightness < 0.999f) {
                r = (uint8_t)(r * g_brightness);
                g = (uint8_t)(g * g_brightness);
                b = (uint8_t)(b * g_brightness);
            }

            /* Apply per-strip color order (any R/G/B/W permutation). */
            uint8_t source[LED_MAX_CHANNELS] = {r, g, b, 0};
            for (int k = 0; k < bpp; k++) {
                tx_buf[i * bpp + k] = source[strips[s].order_indices[k]];
            }
        }

        /* Transmit */
        rmt_transmit_config_t tx_config = {
            .loop_count = 0,
        };
        rmt_transmit(strips[s].rmt_channel, strips[s].encoder, tx_buf, num * bpp, &tx_config);
        rmt_tx_wait_all_done(strips[s].rmt_channel, portMAX_DELAY);
    }

    xSemaphoreGive(led_mutex);
    return ESP_OK;
}

void leds_clear(void)
{
    xSemaphoreTake(led_mutex, portMAX_DELAY);
    memset(pixel_buffer, 0, total_leds * sizeof(rgb_t));
    xSemaphoreGive(led_mutex);
}

int leds_total_count(void)
{
    return total_leds;
}

int leds_strip_count(void)
{
    return num_strips;
}

void leds_set_brightness(float brightness)
{
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    g_brightness = brightness;
}

float leds_get_brightness(void)
{
    return g_brightness;
}

uint8_t leds_apply_brightness_curve(uint8_t value)
{
    return brightness_curve_table[value];
}

/* ------------------------------------------------------------------ */
/* OnboardLED equivalent: single onboard NeoPixel, addressed directly, */
/* bypassing the configured strips[] entirely (mirrors OnboardLED in    */
/* lib/leds.py, used e.g. for boot-time IP-address flashing).           */
/* ------------------------------------------------------------------ */

esp_err_t onboard_led_init(int pin)
{
    if (pin < 0) {
        pin = DEFAULT_ONBOARD_NEOPIXEL_PIN;
    }

    rmt_tx_channel_config_t tx_config = {
        .gpio_num = pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000, /* 10MHz = 0.1us per tick */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_config, &onboard_rmt_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OnboardLED: failed to create RMT channel: %s", esp_err_to_name(ret));
        onboard_led_ready = false;
        return ret;
    }

    ret = create_ws2812_encoder(&onboard_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OnboardLED: failed to create encoder");
        onboard_led_ready = false;
        return ret;
    }

    rmt_enable(onboard_rmt_channel);
    onboard_led_ready = true;
    return ESP_OK;
}

void onboard_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!onboard_led_ready) return;

    /* No brightness/gamma scaling here -- matches OnboardLED.set(), which
     * writes raw channel values straight to the pixel. Default NeoPixel
     * byte order is GRB. */
    uint8_t tx_buf[3] = {g, r, b};

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    rmt_transmit(onboard_rmt_channel, onboard_encoder, tx_buf, sizeof(tx_buf), &tx_config);
    rmt_tx_wait_all_done(onboard_rmt_channel, portMAX_DELAY);
}

void onboard_led_off(void)
{
    onboard_led_set(0, 0, 0);
}
