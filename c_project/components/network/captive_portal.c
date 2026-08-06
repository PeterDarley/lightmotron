#include "captive_portal.h"
#include "persistent_dict.h"
#include "wifi_manager.h"
#include "leds.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "captive_portal";

#define AP_SSID "lightmotron-setup"
#define AP_MAX_CONNECTIONS 4
#define DNS_PORT 53
#define HTTP_PORT 80
#define AP_IP "192.168.4.1"
#define DEFAULT_HOSTNAME_FALLBACK "lightmotron"

static TaskHandle_t dns_task_handle = NULL;
static TaskHandle_t http_task_handle = NULL;
static bool portal_running = false;

/* Networks discovered by scan_networks(), cached for the lifetime of the
 * portal session. Populated BEFORE the AP interface comes up, since once
 * the AP is running the radio is busy and a station scan returns nothing
 * (mirrors captive_portal.py's _scan_networks() docstring). */
typedef struct {
    char ssid[33];
    int8_t rssi;
} scanned_network_t;

#define MAX_SCANNED_NETWORKS 20
static scanned_network_t cached_networks[MAX_SCANNED_NETWORKS];
static int cached_network_count = 0;

/**
 * Escape characters that are special in HTML attribute values and content
 * (mirrors captive_portal.py's _html_escape()).
 */
static void html_escape(char *dst, size_t dst_size, const char *src)
{
    size_t di = 0;

    for (size_t si = 0; src[si] != '\0'; si++) {
        const char *rep = NULL;

        switch (src[si]) {
            case '&': rep = "&amp;"; break;
            case '"': rep = "&quot;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            default: break;
        }

        if (rep) {
            size_t rep_len = strlen(rep);
            if (di + rep_len >= dst_size) break;
            memcpy(dst + di, rep, rep_len);
            di += rep_len;
        } else {
            if (di + 1 >= dst_size) break;
            dst[di++] = src[si];
        }
    }

    dst[di] = '\0';
}

/**
 * Decode a URL-encoded string (%XX sequences and '+' -> space), mirroring
 * captive_portal.py's _url_decode().
 */
static void url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t di = 0;

    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_size; si++) {
        char c = src[si];

        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && src[si + 1] != '\0' && src[si + 2] != '\0') {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            char *endptr = NULL;
            long value = strtol(hex, &endptr, 16);
            if (endptr == hex + 2) {
                dst[di++] = (char)value;
                si += 2;
            } else {
                dst[di++] = c;
            }
        } else {
            dst[di++] = c;
        }
    }

    dst[di] = '\0';
}

/**
 * Return the configured mDNS hostname from persistent storage, falling
 * back to DEFAULT_HOSTNAME_FALLBACK when none is stored yet.
 */
static const char *get_configured_hostname(void)
{
    static char hostname[64];

    persistent_dict_t *sys = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *hostname_json = persistent_dict_get(sys, "hostname");

    if (hostname_json && cJSON_IsString(hostname_json) && hostname_json->valuestring[0] != '\0') {
        strncpy(hostname, hostname_json->valuestring, sizeof(hostname) - 1);
    } else {
        strncpy(hostname, DEFAULT_HOSTNAME_FALLBACK, sizeof(hostname) - 1);
    }
    hostname[sizeof(hostname) - 1] = '\0';

    return hostname;
}

/**
 * Scan for available WiFi networks and cache a sorted, deduplicated list
 * (strongest signal first). Must be called BEFORE the AP interface is
 * activated, matching captive_portal.py's _scan_networks().
 */
static void scan_networks(void)
{
    cached_network_count = 0;

    wifi_scan_config_t scan_config = {0};
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Network scan failed: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGI(TAG, "Network scan returned no results");
        return;
    }

    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!records) {
        return;
    }

    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK) {
        free(records);
        return;
    }

    for (int i = 0; i < ap_count; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }

        int existing = -1;
        for (int j = 0; j < cached_network_count; j++) {
            if (strcmp(cached_networks[j].ssid, ssid) == 0) {
                existing = j;
                break;
            }
        }

        if (existing >= 0) {
            /* Keep the entry with the strongest signal for each SSID */
            if (records[i].rssi > cached_networks[existing].rssi) {
                cached_networks[existing].rssi = records[i].rssi;
            }
        } else if (cached_network_count < MAX_SCANNED_NETWORKS) {
            strncpy(cached_networks[cached_network_count].ssid, ssid, sizeof(cached_networks[0].ssid) - 1);
            cached_networks[cached_network_count].ssid[sizeof(cached_networks[0].ssid) - 1] = '\0';
            cached_networks[cached_network_count].rssi = records[i].rssi;
            cached_network_count++;
        }
    }

    free(records);

    /* Sort strongest-first (small N, simple selection sort is fine) */
    for (int i = 0; i < cached_network_count - 1; i++) {
        int strongest = i;
        for (int j = i + 1; j < cached_network_count; j++) {
            if (cached_networks[j].rssi > cached_networks[strongest].rssi) {
                strongest = j;
            }
        }
        if (strongest != i) {
            scanned_network_t tmp = cached_networks[i];
            cached_networks[i] = cached_networks[strongest];
            cached_networks[strongest] = tmp;
        }
    }

    ESP_LOGI(TAG, "Network scan found %d unique SSID(s)", cached_network_count);
}

/**
 * Build the WiFi setup page HTML, including <option> entries for each
 * scanned network, mirroring captive_portal.py's _build_portal_html().
 * Caller must free() the returned buffer.
 */
static char *build_portal_html(void)
{
    size_t capacity = 4096 + (size_t)cached_network_count * 200;
    char *html = malloc(capacity);
    if (!html) {
        return NULL;
    }

    char safe_hostname[128];
    html_escape(safe_hostname, sizeof(safe_hostname), get_configured_hostname());

    size_t offset = (size_t)snprintf(html, capacity,
        "<!DOCTYPE html><html lang=\"en\"><head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Lightmotron Setup</title>"
        "<style>"
        "*,*::before,*::after{box-sizing:border-box;}"
        "body{font-family:sans-serif;background:#111;color:#eee;display:flex;justify-content:center;padding:2rem;margin:0;}"
        ".card{background:#1e1e1e;border-radius:8px;padding:2rem;max-width:420px;width:100%%;}"
        "h1{margin-top:0;color:#fff;font-size:1.4rem;}"
        "p{color:#aaa;font-size:.9rem;}"
        "label{display:block;margin-top:1rem;font-size:.85rem;color:#aaa;}"
        "select,input{width:100%%;padding:.5rem .6rem;margin-top:.25rem;background:#2c2c2c;color:#fff;border:1px solid #555;border-radius:4px;font-size:1rem;}"
        "button{margin-top:1.5rem;width:100%%;padding:.75rem;background:#0d6efd;color:#fff;border:none;border-radius:4px;font-size:1rem;cursor:pointer;}"
        ".note{margin-top:1.5rem;font-size:.8rem;color:#777;border-top:1px solid #333;padding-top:1rem;}"
        "</style></head><body><div class=\"card\">"
        "<h1>Lightmotron &mdash; WiFi Setup</h1>"
        "<p>Select your WiFi network and enter the password.</p>"
        "<form method=\"POST\" action=\"/save\" onsubmit=\"return onSub()\">"
        "<input type=\"hidden\" name=\"ssid\" id=\"ssid\">"
        "<label>Network</label>"
        "<select id=\"nsel\" onchange=\"onNet(this)\">"
        "<option value=\"\">-- select a network --</option>");

    /* Rescan link: esp_wifi_scan_start() works fine in APSTA mode (this
     * radio isn't limited to STA-only scanning the way the pre-AP scan
     * comment above might suggest -- that comment is about scanning
     * *before* AP mode is even up). It does briefly pause AP beaconing
     * while it hops channels, so an in-progress page load on another
     * client could stall for a second or two -- acceptable for a
     * manually-triggered, infrequent rescan. */
    offset += (size_t)snprintf(html + offset, capacity - offset,
        "<a href=\"/rescan\" style=\"display:block;margin-top:.5rem;font-size:.8rem;"
        "color:#0d6efd;text-decoration:none;\">&#8635; Rescan networks</a>");

    for (int i = 0; i < cached_network_count && offset + 256 < capacity; i++) {
        int signal_pct = 2 * (cached_networks[i].rssi + 100);
        if (signal_pct < 0) signal_pct = 0;
        if (signal_pct > 100) signal_pct = 100;

        char safe_ssid[160];
        html_escape(safe_ssid, sizeof(safe_ssid), cached_networks[i].ssid);

        offset += (size_t)snprintf(html + offset, capacity - offset,
            "<option value=\"%s\">%s (%d%%)</option>", safe_ssid, safe_ssid, signal_pct);
    }

    snprintf(html + offset, capacity - offset,
        "<option value=\"__other__\">Other (enter manually)&hellip;</option>"
        "</select>"
        "<div id=\"other-row\" style=\"display:none\">"
        "<label>Network name (SSID)</label>"
        "<input type=\"text\" id=\"manual\" name=\"manual\" maxlength=\"64\" autocomplete=\"off\">"
        "</div>"
        "<label>Password</label>"
        "<input type=\"password\" name=\"password\" maxlength=\"64\" autocomplete=\"new-password\">"
        "<button type=\"submit\">Save &amp; Connect</button>"
        "</form>"
        "<p class=\"note\">After saving, the device will reboot and join your network. "
        "Try <strong>%s.local</strong>, <strong>%s.lan</strong>, <strong>%s.home</strong>, "
        "or just <strong>%s</strong> in your browser &mdash; whichever one works depends "
        "on your router. Android usually needs the plain hostname or .lan/.home.</p>"
        "</div>"
        "<script>"
        "function onNet(s){"
        "var o=document.getElementById('other-row');"
        "var h=document.getElementById('ssid');"
        "if(s.value==='__other__'){o.style.display='';h.value='';}"
        "else{o.style.display='none';h.value=s.value;}}"
        "function onSub(){"
        "var s=document.getElementById('nsel');"
        "var h=document.getElementById('ssid');"
        "if(s.value==='__other__'){h.value=document.getElementById('manual').value;}"
        "return h.value.trim().length>0;}"
        "(function(){"
        "var s=document.getElementById('nsel');"
        "if(s.options.length>1&&s.options[1].value!=='__other__'){s.selectedIndex=1;onNet(s);}"
        "})();"
        "</script></body></html>",
        safe_hostname, safe_hostname, safe_hostname, safe_hostname);

    return html;
}

/**
 * Build the "credentials saved" confirmation page.
 */
static char *build_saved_page(void)
{
    /* Sized for the worst case: hostname escaped up to 128 chars, repeated
     * 4 times (.local/.lan/.home/bare) plus the surrounding fixed HTML. */
    static char page[1536];
    char safe_hostname[128];
    html_escape(safe_hostname, sizeof(safe_hostname), get_configured_hostname());

    snprintf(page, sizeof(page),
        "<!DOCTYPE html><html lang=\"en\"><head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Saved</title>"
        "<style>"
        "body{font-family:sans-serif;background:#111;color:#eee;display:flex;justify-content:center;padding:2rem;margin:0;}"
        ".card{background:#1e1e1e;border-radius:8px;padding:2rem;max-width:420px;width:100%%;}"
        "h1{margin-top:0;color:#4caf50;}"
        "p{color:#aaa;}"
        "strong{color:#fff;display:block;margin:0.15rem 0;}"
        "small{color:#777;}"
        "</style></head><body>"
        "<div class=\"card\">"
        "<h1>Credentials Saved</h1>"
        "<p>The device is rebooting and will connect to your WiFi network.</p>"
        "<p>Once connected, try each of these until one works &mdash; which one "
        "depends on your router:</p>"
        "<p>"
        "<strong>http://%s.local/</strong>"
        "<strong>http://%s.lan/</strong>"
        "<strong>http://%s.home/</strong>"
        "<strong>http://%s/</strong>"
        "</p>"
        "<p><small>Android phones typically can't use the .local address; try "
        "the plain hostname or .lan/.home instead.</small></p>"
        "</div></body></html>",
        safe_hostname, safe_hostname, safe_hostname, safe_hostname);

    return page;
}

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
 * Look up a header value (case-insensitive key) within the raw header block
 * [headers_start, headers_end). Returns true and fills out (NUL-terminated)
 * on success, false if the header isn't present.
 */
static bool find_header_value(const char *headers_start, const char *headers_end,
                              const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *line = headers_start;

    while (line < headers_end) {
        const char *line_end = memchr(line, '\n', (size_t)(headers_end - line));
        if (!line_end) {
            line_end = headers_end;
        }

        if ((size_t)(line_end - line) > key_len &&
            strncasecmp(line, key, key_len) == 0 && line[key_len] == ':') {
            const char *val = line + key_len + 1;
            while (val < line_end && (*val == ' ' || *val == '\t')) val++;
            const char *val_end = line_end;
            while (val_end > val && (val_end[-1] == '\r' || val_end[-1] == ' ')) val_end--;

            size_t len = (size_t)(val_end - val);
            if (len >= out_size) {
                len = out_size - 1;
            }
            memcpy(out, val, len);
            out[len] = '\0';
            return true;
        }

        line = line_end + 1;
    }

    return false;
}

/**
 * Parse URL-encoded form body for ssid, password and manual fields.
 */
static void parse_form_data(const char *body, char *ssid, size_t ssid_len,
                            char *password, size_t pass_len,
                            char *manual, size_t manual_len)
{
    ssid[0] = '\0';
    password[0] = '\0';
    manual[0] = '\0';

    const char *ptr = body;
    while (ptr && *ptr) {
        const char *amp = strchr(ptr, '&');
        size_t pair_len = amp ? (size_t)(amp - ptr) : strlen(ptr);

        char pair_buf[256];
        if (pair_len >= sizeof(pair_buf)) {
            pair_len = sizeof(pair_buf) - 1;
        }
        memcpy(pair_buf, ptr, pair_len);
        pair_buf[pair_len] = '\0';

        char *eq = strchr(pair_buf, '=');
        if (eq) {
            *eq = '\0';
            const char *key = pair_buf;
            const char *raw_value = eq + 1;

            char decoded_value[128];
            url_decode(decoded_value, sizeof(decoded_value), raw_value);

            if (strcmp(key, "ssid") == 0) {
                strncpy(ssid, decoded_value, ssid_len - 1);
                ssid[ssid_len - 1] = '\0';
            } else if (strcmp(key, "password") == 0) {
                strncpy(password, decoded_value, pass_len - 1);
                password[pass_len - 1] = '\0';
            } else if (strcmp(key, "manual") == 0) {
                strncpy(manual, decoded_value, manual_len - 1);
                manual[manual_len - 1] = '\0';
            }
        }

        ptr = amp ? amp + 1 : NULL;
    }
}

/**
 * Save WiFi credentials into system_settings.wifi, preserving any other
 * keys already stored there (mirrors captive_portal.py's merge via
 * dict(sys_settings.get("wifi", {}))).
 */
static void save_wifi_credentials(const char *ssid, const char *password)
{
    persistent_dict_t *sys = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *wifi = persistent_dict_get_dup(sys, "wifi");
    if (!wifi) {
        wifi = cJSON_CreateObject();
    }

    cJSON_DeleteItemFromObject(wifi, "ssid");
    cJSON_AddStringToObject(wifi, "ssid", ssid);
    cJSON_DeleteItemFromObject(wifi, "password");
    cJSON_AddStringToObject(wifi, "password", password);

    if (!cJSON_HasObjectItem(wifi, "blink_on_connect")) {
        cJSON_AddBoolToObject(wifi, "blink_on_connect", true);
    }
    if (!cJSON_HasObjectItem(wifi, "print_on_connect")) {
        cJSON_AddBoolToObject(wifi, "print_on_connect", true);
    }

    persistent_dict_set(sys, "wifi", wifi);
    persistent_dict_save(sys);
}

/**
 * HTTP server task for the captive portal.
 *
 * Serves the setup page on GET / (and /index.html), redirects any other
 * GET path to / (so OS captive-portal detection probes land on the form,
 * mirroring captive_portal.py's _handle_request()), saves credentials on
 * POST /save, and reboots once credentials are stored.
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

    char recv_buf[2048];

    while (portal_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            continue;
        }
        char client_ip[16];
        strncpy(client_ip, inet_ntoa(client_addr.sin_addr), sizeof(client_ip) - 1);
        client_ip[sizeof(client_ip) - 1] = '\0';
        uint32_t request_start_ms = esp_log_timestamp();

        /* Bound how long the read loop below can block waiting for the rest
         * of a POST body -- a client that stalls mid-upload would otherwise
         * hang this task (and thus the whole captive portal) indefinitely. */
        struct timeval recv_timeout = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        int received = recv(client_sock, recv_buf, sizeof(recv_buf) - 1, 0);
        if (received <= 0) {
            ESP_LOGI(TAG, "%s (no request received, recv=%d)", client_ip, received);
            close(client_sock);
            continue;
        }
        recv_buf[received] = '\0';

        char method[8] = "";
        char path[128] = "";
        sscanf(recv_buf, "%7s %127s", method, path);

        char *query = strchr(path, '?');
        if (query) {
            *query = '\0';
        }

        bool credentials_saved = false;
        int response_status = 0; /* 0 = no response branch matched/logged below */

        if (strcmp(method, "POST") == 0 && strcmp(path, "/save") == 0) {
            char *headers_end = strstr(recv_buf, "\r\n\r\n");
            char ssid[64] = "", password[64] = "", manual[64] = "";

            if (headers_end) {
                char *body_start = headers_end + 4;
                size_t header_total = (size_t)(body_start - recv_buf);
                size_t body_already = (size_t)received > header_total
                    ? (size_t)received - header_total : 0;

                char content_length_str[16];
                size_t content_length = 0;
                if (find_header_value(recv_buf, headers_end, "Content-Length",
                                       content_length_str, sizeof(content_length_str))) {
                    content_length = (size_t)atoi(content_length_str);
                }

                /* The form only ever holds ssid/password/manual (<=64 bytes
                 * each) plus URL-encoding overhead -- reject anything
                 * implausibly large rather than risk a slow/hostile client
                 * tying up the only captive-portal HTTP task. */
                static char body_buf[1024];
                if (content_length > 0 && content_length < sizeof(body_buf)) {
                    size_t total_body = body_already > content_length ? content_length : body_already;
                    memcpy(body_buf, body_start, total_body);

                    /* A single recv() (above) frequently does NOT capture the
                     * whole request on real devices -- phone captive-portal
                     * browsers commonly send headers and body as separate
                     * TCP segments, or pause for "100 Continue" before
                     * sending the body at all. Keep reading until the full
                     * Content-Length has arrived instead of silently treating
                     * whatever happened to be in the first packet as the
                     * complete form (previously this could parse an empty or
                     * truncated body as a blank SSID). Mirrors the loop
                     * request_parser.c already uses for the main webserver,
                     * which doesn't have this bug for the same reason. */
                    while (total_body < content_length) {
                        int chunk = recv(client_sock, body_buf + total_body,
                                         content_length - total_body, 0);
                        if (chunk <= 0) {
                            break;
                        }
                        total_body += (size_t)chunk;
                    }
                    body_buf[total_body] = '\0';

                    if (total_body < content_length) {
                        ESP_LOGW(TAG, "POST /save body incomplete: got %u of %u bytes",
                                 (unsigned)total_body, (unsigned)content_length);
                    }

                    parse_form_data(body_buf, ssid, sizeof(ssid), password, sizeof(password), manual, sizeof(manual));
                } else {
                    /* No usable Content-Length -- fall back to whatever
                     * arrived after the headers in the initial read. */
                    parse_form_data(body_start, ssid, sizeof(ssid), password, sizeof(password), manual, sizeof(manual));
                }
            }

            /* Fallback: if the hidden field wasn't populated, use the manual entry */
            if (ssid[0] == '\0' || strcmp(ssid, "__other__") == 0) {
                strncpy(ssid, manual, sizeof(ssid) - 1);
                ssid[sizeof(ssid) - 1] = '\0';
            }

            if (strlen(ssid) > 0) {
                ESP_LOGI(TAG, "%s POST /save: saving credentials for SSID '%s'...", client_ip, ssid);
                save_wifi_credentials(ssid, password);
                ESP_LOGI(TAG, "%s POST /save: credentials saved, building response...", client_ip);

                char *saved_page = build_saved_page();
                char response[1536];
                int resp_len = snprintf(response, sizeof(response),
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                    (int)strlen(saved_page), saved_page);
                send(client_sock, response, resp_len, 0);
                credentials_saved = true;
                response_status = 200;
            } else {
                const char *bad_request = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
                send(client_sock, bad_request, strlen(bad_request), 0);
                response_status = 400;
            }

        } else if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
            char *html = build_portal_html();
            if (html) {
                char *response = malloc(strlen(html) + 256);
                if (response) {
                    int resp_len = snprintf(response, strlen(html) + 256,
                        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                        (int)strlen(html), html);
                    send(client_sock, response, resp_len, 0);
                    free(response);
                    response_status = 200;
                }
                free(html);
            }

        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/rescan") == 0) {
            /* scan_networks() resets cached_network_count and repopulates it
             * from a fresh scan -- safe to re-run from here since the AP is
             * already up (see the rescan-link comment in build_portal_html()
             * for why this works despite the pre-AP-only note above it). */
            scan_networks();
            char response[160];
            int resp_len = snprintf(response, sizeof(response),
                "HTTP/1.1 302 Found\r\nLocation: http://" AP_IP "/\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n");
            send(client_sock, response, resp_len, 0);
            response_status = 302;

        } else if (strcmp(method, "GET") == 0) {
            /* Redirect all other GET paths (OS captive-portal probes) to / */
            char response[160];
            int resp_len = snprintf(response, sizeof(response),
                "HTTP/1.1 302 Found\r\nLocation: http://" AP_IP "/\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n");
            send(client_sock, response, resp_len, 0);
            response_status = 302;

        } else {
            const char *bad_request = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            send(client_sock, bad_request, strlen(bad_request), 0);
            response_status = 400;
        }

        ESP_LOGI(TAG, "%s %s %s -> %d (%ums)", client_ip, method, path, response_status,
                 (unsigned)(esp_log_timestamp() - request_start_ms));

        close(client_sock);

        if (credentials_saved) {
            ESP_LOGI(TAG, "Credentials saved, rebooting...");
            onboard_led_off();
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }

    close(server_sock);
    vTaskDelete(NULL);
}

esp_err_t captive_portal_start(void)
{
    ESP_LOGI(TAG, "Starting captive portal");

    /* Initialize WiFi with both AP and STA netifs -- STA is needed so we can
     * scan for nearby networks before the AP radio takes over.
     *
     * This runs as a fallback after boot_init()'s own STA-connect attempt
     * already failed -- wifi_manager_init() already created the default
     * event loop and initialized WiFi earlier in this same boot, so this
     * is NOT a clean slate. esp_event_loop_create_default()/esp_wifi_init()
     * both fail with an "already done" error in that case; ESP_ERROR_CHECK
     * on either would abort() the whole firmware here, turning an ordinary
     * "WiFi failed to connect" (wrong password, router down, moved
     * networks, ...) into a permanent boot-crash-loop with no way back in
     * to fix it -- exactly the case this fallback exists to handle. */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t event_loop_ret = esp_event_loop_create_default();
    if (event_loop_ret != ESP_OK && event_loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(event_loop_ret);
    }

    /* esp_netif_create_default_wifi_sta()/_ap() assert() internally on a
     * duplicate if_key rather than returning an error -- unlike the two
     * calls above, there's no error code to catch after the fact, so the
     * existing netif must be checked for *before* calling. wifi_manager_init()
     * already created "WIFI_STA_DEF" for the connect attempt that just
     * failed; "WIFI_AP_DEF" is never created elsewhere, but check the same
     * way for symmetry/future-proofing. */
    if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
        esp_netif_create_default_wifi_ap();
    }
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t wifi_init_ret = esp_wifi_init(&cfg);
    /* Confirmed via device log: this IDF version returns the generic
     * ESP_ERR_INVALID_STATE here ("WiFi is initialized by esp_wifi_init"),
     * not ESP_ERR_WIFI_INIT_STATE as originally guessed. */
    if (wifi_init_ret != ESP_OK && wifi_init_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(wifi_init_ret);
    }

    /* Scan BEFORE the AP comes up -- once the AP is running the radio is
     * busy and a station scan returns no results (see scan_networks()). */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* wifi_manager's connection-timeout path (wifi_manager.c) marks its own
     * state FAILED but never calls esp_wifi_disconnect() -- the STA can
     * still be internally busy from that unresolved connection attempt,
     * which is a known way for esp_wifi_scan_start() to silently return
     * zero results. Force a clean, idle STA state before scanning. */
    esp_wifi_disconnect();
    scan_networks();

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .max_connection = AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    /* Disable AP power-save so the radio doesn't periodically suspend and
     * drop connected clients (mirrors captive_portal.py's
     * ap_interface.config(pm=network.WIFI_PS_NONE)). */
    esp_err_t ps_ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not disable AP power-save: %s", esp_err_to_name(ps_ret));
    }

    ESP_LOGI(TAG, "AP started: %s", AP_SSID);
    ESP_LOGI(TAG, "Portal at http://" AP_IP "/");

    /* Indicate captive-portal mode with a purple onboard LED, mirroring
     * captive_portal.py's start(). */
    onboard_led_init(-1);
    onboard_led_set(80, 0, 80);

    portal_running = true;

    /* Start DNS hijack task */
    xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, &dns_task_handle);

    /* Start HTTP task */
    /* 16384, not the original 6144: save_wifi_credentials() ->
     * persistent_dict_save() -> json_write_file() now runs LittleFS's
     * write path (lfs_dir_commit/lfs_dir_relocatingcommit/lfs_bd_sync/...),
     * which is meaningfully deeper than SPIFFS's was -- overflowed the
     * previous size (canary-triggered panic while saving WiFi credentials
     * from the captive portal). See LITTLEFS_MIGRATION_NOTES.md. */
    xTaskCreate(http_task, "portal_http", 16384, NULL, 5, &http_task_handle);

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
