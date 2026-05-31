#include "boot.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Lightmotron starting...");

    esp_err_t ret = boot_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Boot failed: %s", esp_err_to_name(ret));
    }
}
