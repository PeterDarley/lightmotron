#include "wifi_manager.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_CONNECT_TIMEOUT_MS 10000

static EventGroupHandle_t wifi_event_group = NULL;
static wifi_state_t current_state = WIFI_STATE_DISCONNECTED;
static char ip_address[16] = "";
static bool initialized = false;
static bool wifi_started = false;
static esp_netif_t *sta_netif = NULL;
static int retry_count = 0;
#define MAX_RETRY 5

/**
 * Scan visible networks and return the exact-case SSID that matches
 * *target_ssid* case-insensitively (mirrors comms.py's _resolve_ssid()).
 *
 * The STA interface must already be started (radio powered on) before this
 * is called. If no match is found, *resolved* is left equal to *target_ssid*
 * so the connection attempt still proceeds.
 */
static void resolve_ssid(const char *target_ssid, char *resolved, size_t resolved_len)
{
    strncpy(resolved, target_ssid, resolved_len - 1);
    resolved[resolved_len - 1] = '\0';

    wifi_scan_config_t scan_config = {0};
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SSID scan failed (%s), using stored value", esp_err_to_name(ret));
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        return;
    }

    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!records) {
        return;
    }

    if (esp_wifi_scan_get_ap_records(&ap_count, records) == ESP_OK) {
        for (int i = 0; i < ap_count; i++) {
            if (strcasecmp((const char *)records[i].ssid, target_ssid) == 0) {
                strncpy(resolved, (const char *)records[i].ssid, resolved_len - 1);
                resolved[resolved_len - 1] = '\0';
                break;
            }
        }
    }

    free(records);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < MAX_RETRY) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retry connection (%d/%d)", retry_count, MAX_RETRY);
        } else {
            current_state = WIFI_STATE_FAILED;
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(ip_address, sizeof(ip_address), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", ip_address);
        current_state = WIFI_STATE_CONNECTED;
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_set_hostname(const char *hostname)
{
    if (!sta_netif || !hostname) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_netif_set_hostname(sta_netif, hostname);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Hostname set to '%s'", hostname);
    } else {
        ESP_LOGW(TAG, "Failed to set hostname: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Power on the radio (without a config) so we can scan for the exact
     * broadcast SSID before connecting, mirroring comms.py's
     * sta_if.active(True) followed by _resolve_ssid()/scan(). */
    if (!wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    }

    char resolved_ssid[33];
    resolve_ssid(ssid, resolved_ssid, sizeof(resolved_ssid));
    if (strcmp(resolved_ssid, ssid) != 0) {
        ESP_LOGI(TAG, "Resolved SSID '%s' -> '%s'", ssid, resolved_ssid);
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, resolved_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    retry_count = 0;
    current_state = WIFI_STATE_CONNECTING;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_LOGI(TAG, "Connecting to '%s'...", resolved_ssid);
    esp_wifi_connect();

    /* Wait for connection or failure */
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "Connection failed after %d retries", MAX_RETRY);
        return ESP_FAIL;
    }

    /* Timeout */
    ESP_LOGW(TAG, "Connection timeout");
    current_state = WIFI_STATE_FAILED;
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_disconnect(void)
{
    esp_wifi_disconnect();
    current_state = WIFI_STATE_DISCONNECTED;
    ip_address[0] = '\0';
    return ESP_OK;
}

wifi_state_t wifi_manager_get_state(void)
{
    return current_state;
}

const char *wifi_manager_get_ip(void)
{
    return ip_address;
}

int8_t wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

bool wifi_manager_is_connected(void)
{
    return current_state == WIFI_STATE_CONNECTED;
}
