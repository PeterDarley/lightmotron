#include "billboard.h"
#include "max7219.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "billboard";

static max7219_t display;
static bool initialized = false;
static volatile bool scroll_stop = false;
static volatile bool scrolling = false;
static TaskHandle_t scroll_task_handle = NULL;

typedef struct {
    char *text;
    int delay_ms;
    int repeat;
} scroll_params_t;

esp_err_t billboard_init(int mosi_pin, int sck_pin, int cs_pin, int num_modules, int brightness)
{
    if (initialized) return ESP_OK;

    esp_err_t ret = max7219_init(&display, mosi_pin, sck_pin, cs_pin, num_modules);
    if (ret != ESP_OK) return ret;

    max7219_set_brightness(&display, brightness);
    initialized = true;

    ESP_LOGI(TAG, "Billboard initialized: %d modules", num_modules);
    return ESP_OK;
}

void billboard_static_text(const char *text, int x, int y)
{
    if (!initialized || !text) return;

    max7219_clear(&display);
    max7219_draw_text(&display, text, x, y);
    max7219_show(&display);
}

static void scroll_task(void *arg)
{
    scroll_params_t *params = (scroll_params_t *)arg;
    scrolling = true;
    scroll_stop = false;

    int text_len = strlen(params->text);
    int char_width = 8;
    int display_width = display.num_modules * 8;
    int total_width = display_width + text_len * char_width + display_width;

    for (int rep = 0; rep < params->repeat; rep++) {
        for (int offset = 0; offset < total_width - display_width + 1; offset++) {
            if (scroll_stop) goto done;

            max7219_clear(&display);
            /* Draw text at position (display_width - offset) */
            int text_x = display_width - offset;
            max7219_draw_text(&display, params->text, text_x, 0);
            max7219_show(&display);

            vTaskDelay(pdMS_TO_TICKS(params->delay_ms));
        }
    }

done:
    scrolling = false;
    free(params->text);
    free(params);
    scroll_task_handle = NULL;
    vTaskDelete(NULL);
}

void billboard_scroll_text(const char *text, int delay_ms, int repeat)
{
    if (!initialized || !text) return;

    /* Stop any existing scroll */
    billboard_stop_scroll();

    scroll_params_t *params = malloc(sizeof(scroll_params_t));
    if (!params) return;

    params->text = strdup(text);
    if (!params->text) {
        free(params);
        return;
    }

    params->delay_ms = delay_ms > 0 ? delay_ms : 60;
    params->repeat = repeat > 0 ? repeat : 1;

    xTaskCreate(scroll_task, "bb_scroll", 2048, params, 3, &scroll_task_handle);
}

void billboard_stop_scroll(void)
{
    if (scrolling) {
        scroll_stop = true;
        /* Wait for task to finish */
        while (scrolling) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void billboard_clear(void)
{
    if (!initialized) return;
    max7219_clear(&display);
}

void billboard_set_brightness(int brightness)
{
    if (!initialized) return;
    max7219_set_brightness(&display, brightness);
}

bool billboard_is_scrolling(void)
{
    return scrolling;
}
