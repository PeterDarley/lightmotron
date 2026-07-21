idf_component_register(
    SRCS "wifi_manager.c" "captive_portal.c" "mdns_setup.c" "ota_update.c" "ip_announcement.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_http_client esp_https_ota mdns lwip esp_event app_update storage audio
)
