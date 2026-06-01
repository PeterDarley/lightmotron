#include "captive_portal.h"
#include "persistent_dict.h"
#include "wifi_manager.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "captive_portal";

#define AP_SSID "lightmotron-setup"
#define AP_MAX_CONNECTIONS 4
#define DNS_PORT 53
#define HTTP_PORT 80
#define AP_IP "192.168.4.1"

static TaskHandle_t dns_task_handle = NULL;
static TaskHandle_t http_task_handle = NULL;
static bool portal_running = false;

/* Simple HTML setup page */
static const char *setup_page =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Lightmotron Setup</title>"
    "<style>body{font-family:sans-serif;margin:40px auto;max-width:400px;padding:0 20px;}"
    "input{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;}"
    "button{width:100%;padding:12px;background:#007bff;color:white;border:none;cursor:pointer;}"
    "</style></head><body>"
    "<h1>Lightmotron WiFi Setup</h1>"
    "<form method='POST' action='/save'>"
    "<label>WiFi Network (SSID):</label><input name='ssid' required>"
    "<label>Password:</label><input name='password' type='password'>"
    "<button type='submit'>Connect</button>"
    "</form></body></html>";

static const char *success_page =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Success</title></head><body>"
    "<h1>WiFi credentials saved!</h1><p>The device will now restart and connect to your network.</p>"
    "</body></html>";

/**
 * DNS server task — responds to ALL queries with the AP IP address.
 */
static void dns_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);

    uint8_t buffer[512];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (portal_running) {
        int len = recvfrom(sock, buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&client_addr, &client_len);
        if (len < 12 || len > (int)(sizeof(buffer) - 16)) {
            continue;
        }

        /* Build DNS response: copy query, set response flags, add answer */
        uint8_t response[512];
        memcpy(response, buffer, len);

        /* Set QR bit (response), AA bit (authoritative) */
        response[2] = 0x81;
        response[3] = 0x80;
        /* Set answer count to 1 */
        response[6] = 0x00;
        response[7] = 0x01;

        /* Append answer: pointer to name in query, type A, class IN, TTL 60, 4 bytes, AP IP */
        int answer_offset = len;
        response[answer_offset++] = 0xC0; /* Name pointer */
        response[answer_offset++] = 0x0C; /* Offset to name in query */
        response[answer_offset++] = 0x00; /* Type A */
        response[answer_offset++] = 0x01;
        response[answer_offset++] = 0x00; /* Class IN */
        response[answer_offset++] = 0x01;
        response[answer_offset++] = 0x00; /* TTL (60 seconds) */
        response[answer_offset++] = 0x00;
        response[answer_offset++] = 0x00;
        response[answer_offset++] = 0x3C;
        response[answer_offset++] = 0x00; /* Data length (4) */
        response[answer_offset++] = 0x04;
        response[answer_offset++] = 192;  /* 192.168.4.1 */
        response[answer_offset++] = 168;
        response[answer_offset++] = 4;
        response[answer_offset++] = 1;

        sendto(sock, response, answer_offset, 0,
               (struct sockaddr *)&client_addr, client_len);
    }

    close(sock);
    vTaskDelete(NULL);
}

/**
 * Parse URL-encoded form body for ssid and password fields.
 */
static void parse_form_data(const char *body, char *ssid, size_t ssid_len,
                            char *password, size_t pass_len)
{
    ssid[0] = '\0';
    password[0] = '\0';

    const char *ptr = body;
    while (ptr && *ptr) {
        const char *eq = strchr(ptr, '=');
        if (!eq) {
            break;
        }

        const char *amp = strchr(eq, '&');
        size_t key_len = eq - ptr;
        size_t val_len = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);

        if (key_len == 4 && strncmp(ptr, "ssid", 4) == 0) {
            size_t copy_len = val_len < ssid_len - 1 ? val_len : ssid_len - 1;
            strncpy(ssid, eq + 1, copy_len);
            ssid[copy_len] = '\0';
        } else if (key_len == 8 && strncmp(ptr, "password", 8) == 0) {
            size_t copy_len = val_len < pass_len - 1 ? val_len : pass_len - 1;
            strncpy(password, eq + 1, copy_len);
            password[copy_len] = '\0';
        }

        ptr = amp ? amp + 1 : NULL;
    }
}

/**
 * HTTP server task for the captive portal.
 */
static void http_task(void *pvParameters)
{
    int server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "HTTP socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(HTTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "HTTP bind failed");
        close(server_sock);
        vTaskDelete(NULL);
        return;
    }

    listen(server_sock, 2);
    ESP_LOGI(TAG, "Captive portal HTTP server started on port %d", HTTP_PORT);

    char recv_buf[1024];

    while (portal_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            continue;
        }

        int received = recv(client_sock, recv_buf, sizeof(recv_buf) - 1, 0);
        if (received <= 0) {
            close(client_sock);
            continue;
        }
        recv_buf[received] = '\0';

        /* Check if this is a POST to /save */
        if (strncmp(recv_buf, "POST /save", 10) == 0) {
            /* Find the body (after \r\n\r\n) */
            char *body = strstr(recv_buf, "\r\n\r\n");
            if (body) {
                body += 4;
                char ssid[64], password[64];
                parse_form_data(body, ssid, sizeof(ssid), password, sizeof(password));

                if (strlen(ssid) > 0) {
                    /* Save credentials */
                    persistent_dict_t *sys = persistent_dict_open(
                        "/spiffs/data/system_settings.json");
                    cJSON *wifi = cJSON_CreateObject();
                    cJSON_AddStringToObject(wifi, "ssid", ssid);
                    cJSON_AddStringToObject(wifi, "password", password);
                    cJSON_AddBoolToObject(wifi, "blink_on_connect", true);
                    cJSON_AddBoolToObject(wifi, "print_on_connect", true);
                    persistent_dict_set(sys, "wifi", wifi);
                    persistent_dict_save(sys);

                    /* Send success page */
                    char response[512];
                    int resp_len = snprintf(response, sizeof(response),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                        (int)strlen(success_page), success_page);
                    send(client_sock, response, resp_len, 0);
                    close(client_sock);

                    /* Reboot after short delay */
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    esp_restart();
                }
            }
        }

        /* Default: serve setup page (handles all GET requests — captive portal redirect) */
        char response[2048];
        int resp_len = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
            (int)strlen(setup_page), setup_page);
        send(client_sock, response, resp_len, 0);
        close(client_sock);
    }

    close(server_sock);
    vTaskDelete(NULL);
}

esp_err_t captive_portal_start(void)
{
    ESP_LOGI(TAG, "Starting captive portal");

    /* Initialize WiFi in AP mode */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .max_connection = AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: %s", AP_SSID);

    portal_running = true;

    /* Start DNS hijack task */
    xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, &dns_task_handle);

    /* Start HTTP task */
    xTaskCreate(http_task, "portal_http", 4096, NULL, 5, &http_task_handle);

    return ESP_OK;
}

esp_err_t captive_portal_stop(void)
{
    portal_running = false;
    vTaskDelay(pdMS_TO_TICKS(500));

    if (dns_task_handle) {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }
    if (http_task_handle) {
        vTaskDelete(http_task_handle);
        http_task_handle = NULL;
    }

    esp_wifi_stop();
    return ESP_OK;
}
