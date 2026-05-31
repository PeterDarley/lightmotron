#include "timing.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <stdbool.h>

uint32_t timing_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint64_t timing_micros(void)
{
    return esp_timer_get_time();
}

bool timing_elapsed(uint32_t start_ms, uint32_t elapsed_ms)
{
    uint32_t now = timing_millis();
    return (now - start_ms) >= elapsed_ms;
}

uint32_t timing_uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}
