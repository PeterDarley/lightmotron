#include "animation.h"
#include "lighting.h"
#include "leds.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>

static const char *TAG = "animation";

#define MAX_STOP_CALLBACKS 4

static TaskHandle_t animation_task_handle = NULL;
static bool running = false;
static bool paused = false;
static uint32_t tick_counter = 0;
static int frame_interval_ms = 25; /* 40Hz default */
static animation_stop_cb_t stop_callbacks[MAX_STOP_CALLBACKS];
static int stop_callback_count = 0;

static void animation_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Animation task started (interval=%dms)", frame_interval_ms);

    TickType_t last_wake = xTaskGetTickCount();

    while (running) {
        if (!paused) {
            lighting_process_tick(tick_counter);
            leds_show();
            tick_counter++;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(frame_interval_ms));
    }

    /* Fire stop callbacks after loop exits */
    for (int i = 0; i < stop_callback_count; i++) {
        if (stop_callbacks[i]) {
            stop_callbacks[i]();
        }
    }

    ESP_LOGI(TAG, "Animation task stopped");
    animation_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t animation_start(void)
{
    if (running) {
        return ESP_OK;
    }

    running = true;
    paused = false;
    tick_counter = 0;

    xTaskCreatePinnedToCore(animation_task, "animation", 4096, NULL, 6,
                            &animation_task_handle, 0);

    return ESP_OK;
}

esp_err_t animation_stop(void)
{
    if (!running) {
        return ESP_OK;
    }

    running = false;

    /* Wait for task to exit */
    vTaskDelay(pdMS_TO_TICKS(frame_interval_ms * 3));

    return ESP_OK;
}

void animation_pause(void)
{
    paused = true;
}

void animation_resume(void)
{
    paused = false;
}

void animation_reset(void)
{
    paused = true;
    tick_counter = 0;
    lighting_clear_scenes();
    leds_clear();
    leds_show();
    paused = false;
}

bool animation_is_running(void)
{
    return running;
}

bool animation_is_paused(void)
{
    return paused;
}

uint32_t animation_get_tick(void)
{
    return tick_counter;
}

void animation_on_stop(animation_stop_cb_t callback)
{
    if (stop_callback_count < MAX_STOP_CALLBACKS && callback) {
        stop_callbacks[stop_callback_count++] = callback;
    }
}

void animation_set_frame_interval(int ms)
{
    if (ms >= 10 && ms <= 1000) {
        frame_interval_ms = ms;
    }
}
