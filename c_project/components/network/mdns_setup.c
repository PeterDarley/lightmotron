#include "mdns_setup.h"
#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "mdns_setup";

esp_err_t mdns_setup_init(const char *hostname)
{
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mdns_hostname_set(hostname);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mdns_instance_name_set("Lightmotron LED Controller");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance name set failed");
    }

    /* Advertise HTTP service */
    ret = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS service add failed");
    }

    ESP_LOGI(TAG, "mDNS initialized: %s.local", hostname);
    return ESP_OK;
}

esp_err_t mdns_setup_set_hostname(const char *hostname)
{
    esp_err_t ret = mdns_hostname_set(hostname);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "mDNS hostname updated: %s.local", hostname);
    }
    return ret;
}
