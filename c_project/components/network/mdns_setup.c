#include "mdns_setup.h"
#include "mdns.h"
#include "esp_log.h"
#include "esp_netif.h"

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

    /* Suppress IPv6 (AAAA) mDNS records for the STA interface. ESP-IDF's
     * mdns component auto-enables IPv6 once the device gets a link-local
     * IPv6 address, which lwIP auto-configures on connect regardless of
     * whether the LAN actually has working IPv6 multicast routing end to
     * end -- most home networks don't. A client that receives both an A and
     * an AAAA record and tries the (non-functional) IPv6 one first can
     * stall or fail to fall back cleanly, which looks like ".local doesn't
     * work" even though the IPv4 path is fine. See MDNS_NOTES.md. Not
     * durable across a later WiFi reconnect (the component's own event
     * hooks can re-enable IPv6 mDNS on a fresh IP_EVENT_GOT_IP6) -- see the
     * notes file for why that's not handled here yet. */
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        ret = mdns_netif_action(sta_netif, MDNS_EVENT_DISABLE_IP6);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "mDNS IPv6 disable failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "mDNS IPv6 disable: STA netif not found");
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
