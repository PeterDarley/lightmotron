#include "comms.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "comms";
static int status_led_pin = -1;
static char ip_buf[16] = "0.0.0.0";

esp_err_t comms_init_status_led(int gpio_pin)
{
    status_led_pin = gpio_pin;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        gpio_set_level(gpio_pin, 0);
    }

    return ret;
}

void comms_set_status_led(bool on)
{
    if (status_led_pin < 0) return;
    gpio_set_level(status_led_pin, on ? 1 : 0);
}

void comms_blink_status_led(int count, int on_ms, int off_ms)
{
    if (status_led_pin < 0) return;

    for (int i = 0; i < count; i++) {
        gpio_set_level(status_led_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        gpio_set_level(status_led_pin, 0);
        if (i < count - 1) {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

const char *comms_get_wifi_status(void)
{
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

    if (ret == ESP_OK) {
        return "connected";
    }

    return "disconnected";
}

const char *comms_get_ip_address(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return "0.0.0.0";

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
        return ip_buf;
    }

    return "0.0.0.0";
}
