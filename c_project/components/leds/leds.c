#include "leds.h"
#include "json_helpers.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "leds";

#define MAX_STRIPS 4
#define MAX_TOTAL_LEDS 600

/* Gamma correction table (2.2 gamma) */
static const uint8_t gamma_table[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,
    1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
    3,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,
    7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  11,  11,  11,  12,  12,
   13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,  20,  21,
   21,  22,  22,  23,  24,  24,  25,  26,  26,  27,  28,  28,  29,  30,  30,  31,
   32,  33,  33,  34,  35,  36,  36,  37,  38,  39,  40,  40,  41,  42,  43,  44,
   45,  46,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
   60,  61,  62,  63,  64,  65,  67,  68,  69,  70,  71,  72,  73,  75,  76,  77,
   78,  80,  81,  82,  83,  85,  86,  87,  89,  90,  91,  93,  94,  95,  97,  98,
  100, 101, 103, 104, 106, 107, 109, 110, 112, 113, 115, 116, 118, 120, 121, 123,
  125, 126, 128, 130, 131, 133, 135, 137, 138, 140, 142, 144, 146, 147, 149, 151,
  153, 155, 157, 159, 161, 163, 165, 167, 169, 171, 173, 175, 177, 179, 181, 183,
  185, 187, 190, 192, 194, 196, 198, 201, 203, 205, 207, 210, 212, 214, 217, 219,
  221, 224, 226, 229, 231, 233, 236, 238, 241, 243, 246, 248, 251, 253, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

/* Strip state */
typedef struct {
    int pin;
    int num_leds;
    int start_index; /* Global index offset */
    color_order_t color_order;
    bool brightness_curve;
    rmt_channel_handle_t rmt_channel;
    rmt_encoder_handle_t encoder;
} strip_state_t;

static strip_state_t strips[MAX_STRIPS];
static int num_strips = 0;
static int total_leds = 0;
static rgb_t *pixel_buffer = NULL;
static uint8_t *tx_buffer = NULL; /* Pre-allocated for leds_show() */
static SemaphoreHandle_t led_mutex = NULL;

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

static color_order_t parse_color_order(const char *str)
{
    if (!str) return COLOR_ORDER_GRB;
    if (strcasecmp(str, "RGB") == 0) return COLOR_ORDER_RGB;
    if (strcasecmp(str, "RGBW") == 0) return COLOR_ORDER_RGBW;
    return COLOR_ORDER_GRB;
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
        configs[i].color_order = parse_color_order(json_get_string(item, "color_order", "GRB"));
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

    for (int i = 0; i < num_strips; i++) {
        strips[i].pin = configs[i].pin;
        strips[i].num_leds = configs[i].num_leds;
        strips[i].color_order = configs[i].color_order;
        strips[i].brightness_curve = configs[i].brightness_curve;
        strips[i].start_index = total_leds;
        total_leds += configs[i].num_leds;
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

    tx_buffer = malloc(max_strip_leds * 3);
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

esp_err_t leds_show(void)
{
    xSemaphoreTake(led_mutex, portMAX_DELAY);

    for (int s = 0; s < num_strips; s++) {
        int num = strips[s].num_leds;
        int start = strips[s].start_index;

        /* Build transmission buffer with color order */
        uint8_t *tx_buf = tx_buffer;

        for (int i = 0; i < num; i++) {
            rgb_t pixel = pixel_buffer[start + i];
            uint8_t r = pixel.r, g = pixel.g, b = pixel.b;

            /* Apply gamma correction */
            if (strips[s].brightness_curve) {
                r = gamma_table[r];
                g = gamma_table[g];
                b = gamma_table[b];
            }

            /* Apply color order */
            switch (strips[s].color_order) {
            case COLOR_ORDER_RGB:
                tx_buf[i * 3 + 0] = r;
                tx_buf[i * 3 + 1] = g;
                tx_buf[i * 3 + 2] = b;
                break;
            case COLOR_ORDER_GRB:
            default:
                tx_buf[i * 3 + 0] = g;
                tx_buf[i * 3 + 1] = r;
                tx_buf[i * 3 + 2] = b;
                break;
            }
        }

        /* Transmit */
        rmt_transmit_config_t tx_config = {
            .loop_count = 0,
        };
        rmt_transmit(strips[s].rmt_channel, strips[s].encoder, tx_buf, num * 3, &tx_config);
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

uint8_t leds_gamma_correct(uint8_t value)
{
    return gamma_table[value];
}
