#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "ota_update.h"
#include "wifi_manager.h"
#include "ip_announcement.h"
#include "json_helpers.h"
#include "animation.h"
#include "audio_player.h"
#include "timing.h"
#include "comms.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_spiffs.h>
#include <sdkconfig.h>

#define THEMES_DIR "/spiffs/www/themes"
#define THEMES_FONTS_DIR "/spiffs/www/themes/fonts"

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Reads a single form field's value at `index` (repeated same-named fields
 * arrive as a cJSON array; a lone field arrives as a plain string). Mirrors
 * the indexing used by Python's _as_list()-based parallel-array handling in
 * SystemSettingsView.post(). */
static const char *form_field_at(http_request_t *req, const char *name, int index)
{
    if (!req || !req->form_data) return NULL;
    cJSON *field = cJSON_GetObjectItem(req->form_data, name);
    if (!field) return NULL;
    if (cJSON_IsArray(field)) {
        cJSON *item = cJSON_GetArrayItem(field, index);
        return (item && cJSON_IsString(item)) ? item->valuestring : NULL;
    }
    return (index == 0 && cJSON_IsString(field)) ? field->valuestring : NULL;
}

static int form_field_count(http_request_t *req, const char *name)
{
    if (!req || !req->form_data) return 0;
    cJSON *field = cJSON_GetObjectItem(req->form_data, name);
    if (!field) return 0;
    if (cJSON_IsArray(field)) return cJSON_GetArraySize(field);
    return cJSON_IsString(field) ? 1 : 0;
}

/* ---------------------------------------------------------------------
 * Theme picker
 * ------------------------------------------------------------------- */

/* Mirrors _theme_display_name(): strip ".css", replace '_' with ' '. */
static void theme_display_name(const char *filename, char *out, size_t outsz)
{
    size_t len = strlen(filename);
    if (len >= 4 && strcmp(filename + len - 4, ".css") == 0) len -= 4;
    if (len >= outsz) len = outsz - 1;
    for (size_t i = 0; i < len; i++) out[i] = (filename[i] == '_') ? ' ' : filename[i];
    out[len] = '\0';
}

/* Mirrors _list_theme_files(): sorted ".css" filenames in the themes dir. */
static cJSON *list_theme_files(void)
{
    cJSON *result = cJSON_CreateArray();
    DIR *dir = opendir(THEMES_DIR);
    if (!dir) return result;

    char **names = NULL;
    int count = 0, cap = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 4, ".css") != 0) continue;
        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            names = realloc(names, cap * sizeof(char *));
        }
        names[count++] = strdup(entry->d_name);
    }
    closedir(dir);

    qsort(names, count, sizeof(char *), cmp_str);
    for (int i = 0; i < count; i++) {
        char display[128];
        theme_display_name(names[i], display, sizeof(display));
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateString(names[i]));
        cJSON_AddItemToArray(pair, cJSON_CreateString(display));
        cJSON_AddItemToArray(result, pair);
        free(names[i]);
    }
    free(names);
    return result;
}

static char *get_current_theme(void)
{
    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    char *theme = strdup("");
    if (store) {
        cJSON *v = persistent_dict_get_dup(store, "theme");
        if (v && cJSON_IsString(v) && v->valuestring) {
            free(theme);
            theme = strdup(v->valuestring);
        }
        cJSON_Delete(v);
    }
    return theme;
}

/* Mirrors _theme_response(). */
static http_response_t *theme_response(const char *message, const char *error)
{
    cJSON *ctx = build_global_context();
    cJSON_AddItemToObject(ctx, "theme_files", list_theme_files());
    char *current_theme = get_current_theme();
    cJSON_AddStringToObject(ctx, "current_theme", current_theme);
    free(current_theme);
    cJSON_AddStringToObject(ctx, "message", message ? message : "");
    cJSON_AddStringToObject(ctx, "error", error ? error : "");
    return webserver_render_response("setup/theme.html", ctx);
}

http_response_t *view_theme(http_request_t *req)
{
    if (req->method == HTTP_METHOD_GET) {
        return theme_response(NULL, NULL);
    }

    const char *theme_filename = request_get_form_field(req, "theme");
    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    if (store) {
        persistent_dict_set(store, "theme", cJSON_CreateString(theme_filename ? theme_filename : ""));
        persistent_dict_save(store);
    }

    http_response_t *res = response_create(200, "text/plain", "");
    res->headers = cJSON_CreateObject();
    cJSON_AddStringToObject(res->headers, "HX-Redirect", "/setup");
    return res;
}

http_response_t *view_theme_delete(http_request_t *req)
{
    const char *name = request_get_form_field(req, "theme");

    /* Refuse anything not ending in ".css" (also rejects empty/NULL names). */
    size_t len = name ? strlen(name) : 0;
    if (!name || len < 4 || strcmp(name + len - 4, ".css") != 0) {
        return response_create(400, "text/plain", "Invalid theme filename.");
    }

    if (strstr(name, "/") || strstr(name, "\\") || strstr(name, "..")) {
        return response_create(400, "text/plain", "Invalid theme filename.");
    }

    char *current_theme = get_current_theme();
    bool is_active = (strcmp(name, current_theme) == 0);
    free(current_theme);
    if (is_active) {
        return response_create(400, "text/plain", "Cannot delete the active theme.");
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", THEMES_DIR, name);
    if (remove(path) != 0) {
        return response_create(404, "text/plain", "Theme file not found.");
    }

    return theme_response(NULL, NULL);
}

http_response_t *view_theme_upload(http_request_t *req)
{
    static const char *allowed_ext[] = {".css", ".ttf", ".woff", ".woff2", ".otf", NULL};
    static const char *font_ext[] = {".ttf", ".woff", ".woff2", ".otf", NULL};
    const size_t max_size = 512 * 1024;

    http_file_t *uploaded = NULL;
    for (int i = 0; i < req->file_count; i++) {
        if (strcmp(req->files[i].field_name, "file") == 0) { uploaded = &req->files[i]; break; }
    }
    if (!uploaded) return theme_response(NULL, "No file selected.");

    const char *filename = uploaded->filename;
    if (!filename || !filename[0]) return theme_response(NULL, "No filename provided.");
    if (strstr(filename, "/") || strstr(filename, "\\") || strstr(filename, "..")) {
        return theme_response(NULL, "Invalid filename.");
    }

    char lower[128];
    size_t flen = strlen(filename);
    if (flen >= sizeof(lower)) flen = sizeof(lower) - 1;
    for (size_t i = 0; i < flen; i++) lower[i] = (char)tolower((unsigned char)filename[i]);
    lower[flen] = '\0';

    bool ext_ok = false;
    for (int i = 0; allowed_ext[i]; i++) {
        size_t elen = strlen(allowed_ext[i]);
        if (flen >= elen && strcmp(lower + flen - elen, allowed_ext[i]) == 0) { ext_ok = true; break; }
    }
    if (!ext_ok) return theme_response(NULL, "Only .css, .ttf, .woff, .woff2, and .otf files are allowed.");

    if (uploaded->data_len > max_size) return theme_response(NULL, "File too large (max 512 KB).");

    bool is_font = false;
    for (int i = 0; font_ext[i]; i++) {
        size_t elen = strlen(font_ext[i]);
        if (flen >= elen && strcmp(lower + flen - elen, font_ext[i]) == 0) { is_font = true; break; }
    }
    const char *target_dir = is_font ? THEMES_FONTS_DIR : THEMES_DIR;

    struct stat st;
    if (stat(target_dir, &st) != 0) mkdir(target_dir, 0755);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", target_dir, filename);
    FILE *f = fopen(path, "wb");
    if (!f) return theme_response(NULL, "Write error.");
    fwrite(uploaded->data, 1, uploaded->data_len, f);
    fclose(f);

    char message[192];
    snprintf(message, sizeof(message), "Uploaded %s", filename);
    return theme_response(message, NULL);
}

/* ---------------------------------------------------------------------
 * Hostname
 * ------------------------------------------------------------------- */

static const char *HOSTNAME_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789-";

static bool hostname_chars_valid(const char *s)
{
    for (const char *p = s; *p; p++) {
        if (!strchr(HOSTNAME_CHARS, *p)) return false;
    }
    return true;
}

static void lowercase_trim(const char *in, char *out, size_t outsz)
{
    /* Trim leading/trailing spaces, lowercase the rest. */
    while (*in == ' ') in++;
    size_t len = strlen(in);
    while (len > 0 && in[len - 1] == ' ') len--;
    if (len >= outsz) len = outsz - 1;
    for (size_t i = 0; i < len; i++) out[i] = (char)tolower((unsigned char)in[i]);
    out[len] = '\0';
}

typedef struct {
    char ssid[64];
    char password[64];
} wifi_reconnect_args_t;

static void hostname_reconnect_task(void *arg)
{
    wifi_reconnect_args_t *args = (wifi_reconnect_args_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));
    wifi_manager_connect(args->ssid, args->password);
    free(args);
    vTaskDelete(NULL);
}

/* Mirrors HostnameView._apply_and_restart(): set the mDNS hostname and
 * reconnect WiFi in the background so mDNS advertises the new name. */
static void apply_hostname_and_restart(const char *hostname)
{
    wifi_manager_set_hostname(hostname);

    wifi_reconnect_args_t *args = calloc(1, sizeof(wifi_reconnect_args_t));
    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    if (store) {
        cJSON *wifi = persistent_dict_get_dup(store, "wifi");
        if (wifi) {
            snprintf(args->ssid, sizeof(args->ssid), "%s", json_get_string(wifi, "ssid", ""));
            snprintf(args->password, sizeof(args->password), "%s", json_get_string(wifi, "password", ""));
            cJSON_Delete(wifi);
        }
    }
    xTaskCreate(hostname_reconnect_task, "hostname_reconnect", 3072, args, 3, NULL);
}

/* Mirrors HostnameView._redirect_response(). */
static http_response_t *hostname_redirect_response(const char *hostname)
{
    char body[512];
    snprintf(body, sizeof(body),
        "<div class=\"alert alert-info small py-2\">Hostname updated. Reconnecting network&hellip;</div>"
        "<p class=\"small text-muted\">Redirecting to <strong>http://%s.local/setup</strong> in 5 seconds.</p>"
        "<script>setTimeout(function(){window.location.href=\"http://%s.local/setup\";},5000);</script>",
        hostname, hostname);
    return response_create(200, "text/html", body);
}

static http_response_t *hostname_response(const char *message, const char *error)
{
    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    char *hostname = strdup("");
    if (store) {
        cJSON *v = persistent_dict_get_dup(store, "hostname");
        if (v && cJSON_IsString(v)) { free(hostname); hostname = strdup(v->valuestring); }
        cJSON_Delete(v);
    }

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "hostname", hostname);
    cJSON_AddStringToObject(ctx, "message", message ? message : "");
    cJSON_AddStringToObject(ctx, "error", error ? error : "");
    free(hostname);
    return webserver_render_response("setup/hostname.html", ctx);
}

http_response_t *view_hostname(http_request_t *req)
{
    if (req->method == HTTP_METHOD_GET) {
        return hostname_response(NULL, NULL);
    }

    const char *raw = request_get_form_field(req, "hostname");
    char hostname[64];
    lowercase_trim(raw ? raw : "", hostname, sizeof(hostname));

    if (!hostname[0]) {
        persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
        if (store) {
            persistent_dict_set(store, "hostname", cJSON_CreateString(""));
            persistent_dict_save(store);
        }
        apply_hostname_and_restart("lightmotron");
        return hostname_redirect_response("lightmotron");
    }

    if (strlen(hostname) > 32) return hostname_response(NULL, "Hostname must be 32 characters or fewer.");
    if (hostname[0] == '-' || hostname[strlen(hostname) - 1] == '-') {
        return hostname_response(NULL, "Hostname cannot start or end with a hyphen.");
    }
    if (!hostname_chars_valid(hostname)) {
        return hostname_response(NULL, "Only letters, numbers, and hyphens are allowed.");
    }

    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    if (store) {
        persistent_dict_set(store, "hostname", cJSON_CreateString(hostname));
        persistent_dict_save(store);
    }
    apply_hostname_and_restart(hostname);
    return hostname_redirect_response(hostname);
}

/* ---------------------------------------------------------------------
 * System settings (WiFi, hostname, NeoPixel strips, audio players)
 * ------------------------------------------------------------------- */

static const char *COLOR_ORDERS[] = {"GRB", "RGB", "BGR", "BRG", "RBG", "GBR", "GRBW", "RGBW"};
#define COLOR_ORDER_COUNT (sizeof(COLOR_ORDERS) / sizeof(COLOR_ORDERS[0]))

static bool is_valid_color_order(const char *s)
{
    for (size_t i = 0; i < COLOR_ORDER_COUNT; i++) if (strcasecmp(s, COLOR_ORDERS[i]) == 0) return true;
    return false;
}

/* Mirrors _parse_neopixels_storage(): normalize whatever is stored under
 * "neopixels" into a non-empty list of strip dicts. */
static cJSON *parse_neopixels_storage(cJSON *raw)
{
    cJSON *result = cJSON_CreateArray();

    if (raw && cJSON_IsArray(raw) && cJSON_GetArraySize(raw) > 0) {
        for (cJSON *item = raw->child; item; item = item->next) {
            if (!cJSON_IsObject(item)) continue;
            if (!cJSON_GetObjectItem(item, "pin") || !cJSON_GetObjectItem(item, "num")) continue;
            cJSON *strip = cJSON_CreateObject();
            cJSON_AddNumberToObject(strip, "pin", json_get_int(item, "pin", 4));
            cJSON_AddNumberToObject(strip, "num", json_get_int(item, "num", 30));
            char order[8];
            snprintf(order, sizeof(order), "%s", json_get_string(item, "color_order", "GRB"));
            for (char *p = order; *p; p++) *p = (char)toupper((unsigned char)*p);
            cJSON_AddStringToObject(strip, "color_order", order);
            cJSON_AddBoolToObject(strip, "brightness_curve", json_get_bool(item, "brightness_curve", true));
            cJSON_AddItemToArray(result, strip);
        }
        if (cJSON_GetArraySize(result) > 0) return result;
    }

    if (raw && cJSON_IsObject(raw)) {
        cJSON *strip = cJSON_CreateObject();
        cJSON_AddNumberToObject(strip, "pin", json_get_int(raw, "pin", 4));
        cJSON_AddNumberToObject(strip, "num", json_get_int(raw, "num", 144));
        char order[8];
        snprintf(order, sizeof(order), "%s", json_get_string(raw, "color_order", "GRB"));
        for (char *p = order; *p; p++) *p = (char)toupper((unsigned char)*p);
        cJSON_AddStringToObject(strip, "color_order", order);
        cJSON_AddBoolToObject(strip, "brightness_curve", json_get_bool(raw, "brightness_curve", true));
        cJSON_AddItemToArray(result, strip);
        return result;
    }

    cJSON *strip = cJSON_CreateObject();
    cJSON_AddNumberToObject(strip, "pin", 4);
    cJSON_AddNumberToObject(strip, "num", 144);
    cJSON_AddStringToObject(strip, "color_order", "GRB");
    cJSON_AddBoolToObject(strip, "brightness_curve", true);
    cJSON_AddItemToArray(result, strip);
    return result;
}

/* Mirrors _system_settings_context(). */
static void add_system_settings_context(cJSON *ctx)
{
    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *wifi = NULL, *neopixels = NULL, *audio_players = NULL;
    char *hostname = strdup("");

    if (store) {
        cJSON *w = persistent_dict_get_dup(store, "wifi");
        if (w) wifi = w;
        cJSON *h = persistent_dict_get_dup(store, "hostname");
        if (h && cJSON_IsString(h)) { free(hostname); hostname = strdup(h->valuestring); }
        cJSON_Delete(h);
        neopixels = persistent_dict_get_dup(store, "neopixels");
        audio_players = persistent_dict_get_dup(store, "audio_players");
    }

    cJSON_AddStringToObject(ctx, "ss_wifi_ssid", json_get_string(wifi, "ssid", ""));
    cJSON_AddStringToObject(ctx, "ss_hostname", hostname);
    free(hostname);
    if (wifi) cJSON_Delete(wifi);

    cJSON_AddItemToObject(ctx, "ss_strips", parse_neopixels_storage(neopixels));
    if (neopixels) cJSON_Delete(neopixels);

    cJSON *color_orders = cJSON_CreateArray();
    for (size_t i = 0; i < COLOR_ORDER_COUNT; i++) cJSON_AddItemToArray(color_orders, cJSON_CreateString(COLOR_ORDERS[i]));
    cJSON *color_orders_json_arr = cJSON_Duplicate(color_orders, 1);
    cJSON_AddItemToObject(ctx, "ss_color_orders", color_orders);
    char *color_orders_json = cJSON_PrintUnformatted(color_orders_json_arr);
    cJSON_Delete(color_orders_json_arr);
    cJSON_AddStringToObject(ctx, "ss_color_orders_json", color_orders_json ? color_orders_json : "[]");
    if (color_orders_json) cJSON_free(color_orders_json);

    if (audio_players && cJSON_IsArray(audio_players)) {
        cJSON_AddItemToObject(ctx, "ss_audio_players", audio_players);
    } else {
        if (audio_players) cJSON_Delete(audio_players);
        cJSON_AddItemToObject(ctx, "ss_audio_players", cJSON_CreateArray());
    }
}

http_response_t *view_system_settings(http_request_t *req)
{
    if (req->method == HTTP_METHOD_GET) {
        cJSON *ctx = build_global_context();
        add_system_settings_context(ctx);
        return webserver_render_response("setup/system_settings.html", ctx);
    }

    /* --- WiFi --- */
    const char *ssid_in = request_get_form_field(req, "wifi_ssid");
    char ssid[65];
    snprintf(ssid, sizeof(ssid), "%s", ssid_in ? ssid_in : "");

    const char *password_in = request_get_form_field(req, "wifi_password");
    char password[128] = "";
    if (password_in && password_in[0]) {
        snprintf(password, sizeof(password), "%s", password_in);
    } else {
        persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
        cJSON *wifi = store ? persistent_dict_get_dup(store, "wifi") : NULL;
        snprintf(password, sizeof(password), "%s", json_get_string(wifi, "password", ""));
        if (wifi) cJSON_Delete(wifi);
    }

    /* --- Hostname --- */
    const char *hostname_in = request_get_form_field(req, "hostname");
    char hostname[64];
    lowercase_trim(hostname_in ? hostname_in : "", hostname, sizeof(hostname));
    const char *error = NULL;
    if (hostname[0] && (strlen(hostname) > 32 || hostname[0] == '-' || hostname[strlen(hostname) - 1] == '-' ||
                        !hostname_chars_valid(hostname))) {
        error = "Hostname: only letters, numbers, hyphens; max 32 chars; no leading/trailing hyphens.";
    }

    /* --- NeoPixel strips (parallel arrays from repeated fields) --- */
    int n_strips = form_field_count(req, "strip_pin");
    if (n_strips < 1) n_strips = 1;
    cJSON *strips = cJSON_CreateArray();
    for (int i = 0; i < n_strips; i++) {
        const char *pin_s = form_field_at(req, "strip_pin", i);
        const char *num_s = form_field_at(req, "strip_num", i);
        const char *order_s = form_field_at(req, "strip_order", i);
        const char *bc_s = form_field_at(req, "strip_brightness_curve", i);

        int pin_val = pin_s ? atoi(pin_s) : 4;
        int num_val = num_s ? atoi(num_s) : 30;
        if (num_val < 1) num_val = 30;
        char order_val[8];
        snprintf(order_val, sizeof(order_val), "%s", order_s ? order_s : "GRB");
        for (char *p = order_val; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (!is_valid_color_order(order_val)) snprintf(order_val, sizeof(order_val), "GRB");
        bool bc_val = bc_s && strcmp(bc_s, "1") == 0;

        cJSON *strip = cJSON_CreateObject();
        cJSON_AddNumberToObject(strip, "pin", pin_val);
        cJSON_AddNumberToObject(strip, "num", num_val);
        cJSON_AddStringToObject(strip, "color_order", order_val);
        cJSON_AddBoolToObject(strip, "brightness_curve", bc_val);
        cJSON_AddItemToArray(strips, strip);
    }
    if (cJSON_GetArraySize(strips) == 0) {
        cJSON *strip = cJSON_CreateObject();
        cJSON_AddNumberToObject(strip, "pin", 4);
        cJSON_AddNumberToObject(strip, "num", 144);
        cJSON_AddStringToObject(strip, "color_order", "GRB");
        cJSON_AddBoolToObject(strip, "brightness_curve", true);
        cJSON_AddItemToArray(strips, strip);
    }

    /* --- Audio players (parallel arrays; incomplete rows skipped) --- */
    int n_audio = form_field_count(req, "audio_uart");
    int n_tx = form_field_count(req, "audio_tx_pin");
    int n_rx = form_field_count(req, "audio_rx_pin");
    if (n_tx > n_audio) n_audio = n_tx;
    if (n_rx > n_audio) n_audio = n_rx;

    cJSON *audio_players = cJSON_CreateArray();
    for (int i = 0; i < n_audio; i++) {
        const char *uart_s = form_field_at(req, "audio_uart", i);
        const char *tx_s = form_field_at(req, "audio_tx_pin", i);
        const char *rx_s = form_field_at(req, "audio_rx_pin", i);
        const char *hq_s = form_field_at(req, "audio_high_quality", i);

        int uart_val = (uart_s && uart_s[0]) ? atoi(uart_s) : 0;
        int tx_val = (tx_s && tx_s[0]) ? atoi(tx_s) : 0;
        int rx_val = (rx_s && rx_s[0]) ? atoi(rx_s) : 0;
        bool hq_val = hq_s && strcmp(hq_s, "1") == 0;

        if (uart_val >= 0 && tx_val > 0 && rx_val > 0) {
            cJSON *player = cJSON_CreateObject();
            cJSON_AddNumberToObject(player, "uart", uart_val);
            cJSON_AddNumberToObject(player, "tx_pin", tx_val);
            cJSON_AddNumberToObject(player, "rx_pin", rx_val);
            cJSON_AddBoolToObject(player, "high_quality", hq_val);
            cJSON_AddItemToArray(audio_players, player);
        }
    }

    if (error) {
        cJSON_Delete(strips);
        cJSON_Delete(audio_players);
        cJSON *ctx = build_global_context();
        add_system_settings_context(ctx);
        cJSON_AddStringToObject(ctx, "error", error);
        return webserver_render_response("setup/system_settings.html", ctx);
    }

    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    if (store) {
        cJSON *wifi = cJSON_CreateObject();
        cJSON_AddStringToObject(wifi, "ssid", ssid);
        cJSON_AddStringToObject(wifi, "password", password);
        cJSON_AddBoolToObject(wifi, "blink_on_connect", true);
        cJSON_AddBoolToObject(wifi, "print_on_connect", true);
        persistent_dict_set(store, "wifi", wifi);
        persistent_dict_set(store, "hostname", cJSON_CreateString(hostname));
        persistent_dict_set(store, "neopixels", strips);
        persistent_dict_set(store, "audio_players", audio_players);
        persistent_dict_save(store);
    } else {
        cJSON_Delete(strips);
        cJSON_Delete(audio_players);
    }

    cJSON *ctx = build_global_context();
    add_system_settings_context(ctx);
    cJSON_AddStringToObject(ctx, "message", "Settings saved.");
    return webserver_render_response("setup/system_settings.html", ctx);
}

http_response_t *view_system_settings_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    add_system_settings_context(ctx);
    return webserver_render_response("setup/system_settings_summary.html", ctx);
}

/* Mirrors SystemSettingsIPAnnouncedView: marks the current IP as already
 * announced (via sound/notification) so the announcement doesn't repeat.
 * Registered as POST /system_settings/ip_announced in routes.c. */
http_response_t *view_system_settings_ip_announced(http_request_t *req)
{
    const char *ip = wifi_manager_get_ip();
    if (ip && ip[0] && strcmp(ip, "N/A") != 0) {
        ip_announcement_mark_announced(ip);
    }
    return response_create(200, "text/plain", "");
}

/* ---------------------------------------------------------------------
 * Reboot
 * ------------------------------------------------------------------- */

static void delayed_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

http_response_t *view_system_reboot(http_request_t *req)
{
    /* Queue the reboot on a background task so this response actually makes
     * it back to the client first - mirrors SystemRebootView's _thread-based
     * delayed reset. Calling esp_restart() synchronously here (as the
     * previous implementation did) would tear down the connection before
     * the response could be sent. */
    xTaskCreate(delayed_reboot_task, "delayed_reboot", 2048, NULL, 5, NULL);
    return response_create(200, "text/html",
        "<div class=\"alert alert-warning small py-2 mb-0\">"
        "Rebooting now. Reconnect to the device in a few seconds.</div>");
}

http_response_t *view_system_reboot_confirm(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    return webserver_render_response("setup/system_reboot_confirm.html", ctx);
}

/* ---------------------------------------------------------------------
 * OTA updates
 *
 * NOTE: lib/views.py's UpdatesView ports a MicroPython-specific per-file OTA
 * updater (OTAUpdater walks a GitHub repo tree and rewrites individual .py/
 * template files on the interpreter's filesystem). That model doesn't apply
 * to this compiled firmware, which instead flashes a whole firmware binary
 * to the inactive OTA partition (ota_update.h: ota_check_for_update() /
 * ota_apply_update()). Only the repository-settings portion (repo_owner/
 * repo_name persistence) is portable and is ported below; check/apply keep
 * their existing binary-OTA semantics rather than attempting a literal
 * per-file port.
 * ------------------------------------------------------------------- */

static bool validate_repo_part(const char *value)
{
    if (!value || !value[0]) return false;
    size_t len = strlen(value);
    if (len > 64) return false;
    if (value[0] == '.' || value[len - 1] == '.') return false;
    for (size_t i = 0; i < len; i++) {
        char c = value[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

static void get_repo_settings(char *owner, size_t owner_len, char *name, size_t name_len)
{
    snprintf(owner, owner_len, "PeterDarley");
    snprintf(name, name_len, "lightmotron");

    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    if (!store) return;
    cJSON *ota = persistent_dict_get_dup(store, "ota");
    if (!ota) return;
    const char *o = json_get_string(ota, "repo_owner", NULL);
    const char *n = json_get_string(ota, "repo_name", NULL);
    if (o && o[0]) snprintf(owner, owner_len, "%s", o);
    if (n && n[0]) snprintf(name, name_len, "%s", n);
    cJSON_Delete(ota);
}

static cJSON *build_updates_context(const char *message, const char *error, ota_update_info_t *info)
{
    cJSON *ctx = build_global_context();
    char repo_owner[64], repo_name[64];
    get_repo_settings(repo_owner, sizeof(repo_owner), repo_name, sizeof(repo_name));
    cJSON_AddStringToObject(ctx, "repo_owner", repo_owner);
    cJSON_AddStringToObject(ctx, "repo_name", repo_name);
    cJSON_AddStringToObject(ctx, "message", message ? message : "");
    cJSON_AddStringToObject(ctx, "error", error ? error : "");
    if (info) {
        cJSON_AddBoolToObject(ctx, "update_available", info->update_available);
        cJSON_AddStringToObject(ctx, "current_version", info->current_version);
        cJSON_AddStringToObject(ctx, "latest_version", info->latest_version);
    }
    return ctx;
}

http_response_t *view_updates(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");
    if (!action) action = "check";

    if (strcmp(action, "save_repo") == 0) {
        const char *repo_owner = request_get_form_field(req, "repo_owner");
        const char *repo_name = request_get_form_field(req, "repo_name");

        if (!validate_repo_part(repo_owner) || !validate_repo_part(repo_name)) {
            cJSON *ctx = build_updates_context(
                NULL, "Repository must use only letters, numbers, dash, underscore, or period.", NULL);
            return webserver_render_response("setup/updates.html", ctx);
        }

        persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
        if (store) {
            cJSON *ota = cJSON_CreateObject();
            cJSON_AddStringToObject(ota, "repo_owner", repo_owner);
            cJSON_AddStringToObject(ota, "repo_name", repo_name);
            persistent_dict_set(store, "ota", ota);
            persistent_dict_save(store);
        }

        char message[160];
        snprintf(message, sizeof(message), "Repository updated to %s/%s", repo_owner, repo_name);
        return webserver_render_response("setup/updates.html", build_updates_context(message, NULL, NULL));
    }

    ota_update_info_t info = {0};

    if (strcmp(action, "install") == 0 || strcmp(action, "apply") == 0) {
        ota_check_for_update(&info);
        if (info.update_available) {
            ota_apply_update(info.download_url); /* Reboots the device on success. */
            return webserver_render_response("setup/updates.html",
                           build_updates_context(NULL, "Update apply failed.", &info));
        }
        return webserver_render_response("setup/updates.html",
                       build_updates_context("No update available to apply.", NULL, &info));
    }

    ota_check_for_update(&info);
    const char *message = info.update_available ? "Update available." : "No updates available.";
    return webserver_render_response("setup/updates.html", build_updates_context(message, NULL, &info));
}

http_response_t *view_updates_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    ota_update_info_t info = {0};
    ota_check_for_update(&info);
    char repo_owner[64], repo_name[64];
    get_repo_settings(repo_owner, sizeof(repo_owner), repo_name, sizeof(repo_name));
    cJSON_AddStringToObject(ctx, "repo_owner", repo_owner);
    cJSON_AddStringToObject(ctx, "repo_name", repo_name);
    cJSON_AddBoolToObject(ctx, "update_available", info.update_available);
    cJSON_AddStringToObject(ctx, "current_version", info.current_version);
    cJSON_AddStringToObject(ctx, "latest_version", info.latest_version);
    return webserver_render_response("setup/updates_summary.html", ctx);
}

/* ---------------------------------------------------------------------
 * Custom colors
 * ------------------------------------------------------------------- */

/* Parses a "#RRGGBB" or "RRGGBB" hex string into 0-255 r/g/b components.
 * Returns false on malformed input, mirroring CustomColorsView.post()'s
 * silent ValueError/IndexError catch around int(hex_color[...], 16). */
static bool parse_hex_color(const char *value, int *r, int *g, int *b)
{
    if (!value) return false;
    if (value[0] == '#') value++;
    if (strlen(value) < 6) return false;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)value[i])) return false;
    }
    char byte[3] = {0};
    memcpy(byte, value, 2);     *r = (int)strtol(byte, NULL, 16);
    memcpy(byte, value + 2, 2); *g = (int)strtol(byte, NULL, 16);
    memcpy(byte, value + 4, 2); *b = (int)strtol(byte, NULL, 16);
    return true;
}

/* Mirrors _rename_color_refs(): updates every effect's "colors" list,
 * replacing "custom:<old>" entries with "custom:<new>" so effects keep
 * pointing at a renamed custom color. */
static void rename_color_refs(cJSON *model, const char *old_name, const char *new_name)
{
    char old_ref[128], new_ref[128];
    snprintf(old_ref, sizeof(old_ref), "custom:%s", old_name);
    snprintf(new_ref, sizeof(new_ref), "custom:%s", new_name);

    cJSON *effects = model ? cJSON_GetObjectItem(model, "effects") : NULL;
    if (!effects) return;
    for (cJSON *effect = effects->child; effect; effect = effect->next) {
        cJSON *colors_list = cJSON_GetObjectItem(effect, "colors");
        if (!colors_list || !cJSON_IsArray(colors_list)) continue;
        int idx = 0;
        for (cJSON *item = colors_list->child; item; item = item->next, idx++) {
            if (cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, old_ref) == 0) {
                cJSON_ReplaceItemInArray(colors_list, idx, cJSON_CreateString(new_ref));
            }
        }
    }
}

/* Builds the sorted [[name, [r, g, b]], ...] list consumed by
 * `{% for color_name, color_value in custom_colors %}` plus color_value's
 * .0/.1/.2 dot-indexing in the template. Mirrors the custom_colors_list
 * built by _custom_colors_response(). Storage under "custom_colors" is a
 * name -> [r, g, b] array map (see colors.c's color_resolve_custom()), not
 * a hex string. */
static cJSON *build_custom_colors_list(cJSON *colors)
{
    cJSON *result = cJSON_CreateArray();
    if (!colors) return result;

    int total = cJSON_GetArraySize(colors);
    const char **names = total > 0 ? calloc(total, sizeof(char *)) : NULL;
    int count = 0;
    if (names) {
        for (cJSON *item = colors->child; item; item = item->next) names[count++] = item->string;
        qsort(names, count, sizeof(char *), cmp_str);
    }

    for (int i = 0; i < count; i++) {
        cJSON *value = cJSON_GetObjectItem(colors, names[i]);
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateString(names[i]));
        cJSON_AddItemToArray(pair, cJSON_Duplicate(value, 1));
        cJSON_AddItemToArray(result, pair);
    }
    free(names);
    return result;
}

/* Mirrors _custom_colors_response(). */
static http_response_t *custom_colors_response(cJSON *colors, const char *edit_name, const char *edit_hex)
{
    cJSON *ctx = build_global_context();
    cJSON_AddItemToObject(ctx, "custom_colors", build_custom_colors_list(colors));
    cJSON_AddStringToObject(ctx, "page_title", "Custom Colors");
    cJSON_AddStringToObject(ctx, "edit_name", edit_name ? edit_name : "");
    cJSON_AddStringToObject(ctx, "edit_hex", edit_hex ? edit_hex : "");
    return webserver_render_response("setup/custom_colors.html", ctx);
}

http_response_t *view_custom_colors(http_request_t *req)
{
    cJSON *model = lighting_get_settings();
    if (!model) return response_create(500, "text/plain", "Error");

    cJSON *colors = cJSON_GetObjectItem(model, "custom_colors");
    if (!colors) { colors = cJSON_CreateObject(); cJSON_AddItemToObject(model, "custom_colors", colors); }

    if (req->method == HTTP_METHOD_GET) {
        return custom_colors_response(colors, NULL, NULL);
    }

    /* POST: action=add|update|delete|edit_form|cancel. Mirrors
     * CustomColorsView.post(). Form fields are color_name/color_value
     * (color_value is a "#RRGGBB" hex string from an <input type="color">),
     * not name/color. */
    const char *action = request_get_form_field(req, "action");
    if (!action) action = "add";
    const char *color_name = request_get_form_field(req, "color_name");
    const char *color_value = request_get_form_field(req, "color_value");
    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);

    if (strcmp(action, "delete") == 0 && color_name && color_name[0] &&
        cJSON_GetObjectItem(colors, color_name)) {
        cJSON_DeleteItemFromObject(colors, color_name);
        persistent_dict_save(store);
    } else if ((strcmp(action, "add") == 0 || strcmp(action, "update") == 0) &&
               color_name && color_name[0] && color_value && color_value[0]) {
        int r, g, b;
        if (parse_hex_color(color_value, &r, &g, &b)) {
            const char *old_color_name = request_get_form_field(req, "old_color_name");
            if (strcmp(action, "update") == 0 && old_color_name && old_color_name[0] &&
                strcmp(old_color_name, color_name) != 0) {
                if (cJSON_GetObjectItem(colors, old_color_name)) {
                    cJSON_DeleteItemFromObject(colors, old_color_name);
                }
                rename_color_refs(model, old_color_name, color_name);
            }
            cJSON *rgb = cJSON_CreateArray();
            cJSON_AddItemToArray(rgb, cJSON_CreateNumber(r));
            cJSON_AddItemToArray(rgb, cJSON_CreateNumber(g));
            cJSON_AddItemToArray(rgb, cJSON_CreateNumber(b));
            cJSON_DeleteItemFromObject(colors, color_name);
            cJSON_AddItemToObject(colors, color_name, rgb);
            persistent_dict_save(store);
        }
    } else if (strcmp(action, "edit_form") == 0 && color_name && color_name[0]) {
        cJSON *existing = cJSON_GetObjectItem(colors, color_name);
        if (existing && cJSON_IsArray(existing) && cJSON_GetArraySize(existing) >= 3) {
            char hex[8];
            snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                     cJSON_GetArrayItem(existing, 0)->valueint & 0xFF,
                     cJSON_GetArrayItem(existing, 1)->valueint & 0xFF,
                     cJSON_GetArrayItem(existing, 2)->valueint & 0xFF);
            return custom_colors_response(colors, color_name, hex);
        }
    } else if (strcmp(action, "cancel") == 0) {
        return custom_colors_response(colors, NULL, NULL);
    }

    return custom_colors_response(colors, NULL, NULL);
}

http_response_t *view_custom_colors_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    cJSON *colors = model ? cJSON_GetObjectItem(model, "custom_colors") : NULL;
    cJSON_AddItemToObject(ctx, "custom_colors", build_custom_colors_list(colors));
    return webserver_render_response("setup/custom_colors_summary.html", ctx);
}

/* view_storage / view_status / view_setup / view_confirm — moved here from
 * views_home.c (system-level pages, matching how views.h already groups
 * them separately from the home views). */

/* Mirrors _fmt_bytes(): human-readable byte count as "N.N MB"/"N.N KB"/"N B". */
static void fmt_bytes(size_t num_bytes, char *out, size_t outsz)
{
    if (num_bytes >= 1024 * 1024) {
        snprintf(out, outsz, "%.1f MB", num_bytes / (1024.0 * 1024.0));
    } else if (num_bytes >= 1024) {
        snprintf(out, outsz, "%.1f KB", num_bytes / 1024.0);
    } else {
        snprintf(out, outsz, "%u B", (unsigned)num_bytes);
    }
}

/* Redacts the WiFi password the same way _redact_system_settings() does for
 * StorageView: keep ssid, replace only "password" with "***". */
static cJSON *redact_system_settings(cJSON *system_settings)
{
    cJSON *copy = cJSON_Duplicate(system_settings, 1);
    if (!copy) return NULL;
    cJSON *wifi = cJSON_GetObjectItem(copy, "wifi");
    if (wifi && cJSON_GetObjectItem(wifi, "password")) {
        cJSON_ReplaceItemInObject(wifi, "password", cJSON_CreateString("***"));
    }
    return copy;
}

http_response_t *view_storage(http_request_t *req)
{
    (void)req;
    cJSON *ctx = build_global_context();

    /* Mirrors StorageView: pretty-printed dump of persistent storage, with
     * the WiFi password redacted. The C port splits Python's single
     * "storage.json" into system_settings.json + lighting_settings.json, so
     * the two are combined under those same keys for display parity. */
    cJSON *combined = cJSON_CreateObject();

    persistent_dict_t *sys_store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *sys_all = sys_store ? persistent_dict_get_all(sys_store) : NULL;
    if (sys_all) {
        cJSON *redacted = redact_system_settings(sys_all);
        cJSON_AddItemToObject(combined, "system_settings", redacted ? redacted : cJSON_CreateObject());
    }

    persistent_dict_t *lighting_store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    cJSON *lighting_all = lighting_store ? persistent_dict_get_all(lighting_store) : NULL;
    if (lighting_all) {
        cJSON_AddItemToObject(combined, "lighting_settings", cJSON_Duplicate(lighting_all, 1));
    }

    char *pretty = cJSON_Print(combined);
    cJSON_Delete(combined);
    cJSON_AddStringToObject(ctx, "storage_json", pretty ? pretty : "{}");
    cJSON_AddStringToObject(ctx, "page_title", "Storage");
    if (pretty) cJSON_free(pretty);

    return webserver_render_response("storage.html", ctx);
}

/* Builds the audio_health list + summary shown on the status page, combining
 * configured audio_players (system_settings) with a live health probe.
 * Mirrors the audio_health_list/audio_health_summary construction in
 * StatusView. */
static void add_audio_health_context(cJSON *ctx)
{
    persistent_dict_t *store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *configured = store ? persistent_dict_get_dup(store, "audio_players") : NULL;
    int configured_count = (configured && cJSON_IsArray(configured)) ? cJSON_GetArraySize(configured) : 0;

    cJSON *health = configured_count > 0 ? audio_player_check_health() : NULL;

    cJSON *audio_health = cJSON_CreateArray();
    int healthy = 0;
    for (int i = 0; i < configured_count; i++) {
        cJSON *cfg = cJSON_GetArrayItem(configured, i);
        cJSON *info = NULL;
        if (health) {
            for (cJSON *item = health->child; item; item = item->next) {
                if (json_get_int(item, "module", -1) == i) { info = item; break; }
            }
        }

        int uart = json_get_int(cfg, "uart", -1);
        if (uart < 0 && info) uart = json_get_int(info, "uart", -1);
        bool ok = info ? json_get_bool(info, "ok", false) : false;
        if (ok) healthy++;

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "index", i);
        cJSON_AddNumberToObject(entry, "uart", uart);
        cJSON_AddBoolToObject(entry, "ok", ok);
        if (info && json_get_bool(info, "playing", false)) {
            cJSON *state = cJSON_CreateArray();
            cJSON_AddItemToArray(state, cJSON_CreateNumber(json_get_int(info, "current_file", 0)));
            cJSON_AddItemToArray(state, cJSON_CreateBool(false)); /* HQ flag not tracked per-module in health */
            cJSON_AddItemToObject(entry, "state", state);
        } else {
            cJSON_AddNullToObject(entry, "state");
        }
        cJSON_AddItemToArray(audio_health, entry);
    }
    if (health) cJSON_Delete(health);
    if (configured) cJSON_Delete(configured);

    cJSON_AddItemToObject(ctx, "audio_health", audio_health);

    char summary[64];
    if (configured_count == 0) {
        snprintf(summary, sizeof(summary), "No audio modules configured");
    } else {
        snprintf(summary, sizeof(summary), "%d/%d responsive", healthy, configured_count);
    }
    cJSON_AddStringToObject(ctx, "audio_health_summary", summary);
}

http_response_t *view_status(http_request_t *req)
{
    cJSON *ctx = build_global_context();

    /* Heap */
    size_t free_heap = esp_get_free_heap_size();
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t alloc_heap = (total_heap > free_heap) ? (total_heap - free_heap) : 0;
    int mem_pct = total_heap ? (int)((uint64_t)alloc_heap * 100 / total_heap) : 0;
    char mem_free_str[32], mem_alloc_str[32], mem_total_str[32];
    fmt_bytes(free_heap, mem_free_str, sizeof(mem_free_str));
    fmt_bytes(alloc_heap, mem_alloc_str, sizeof(mem_alloc_str));
    fmt_bytes(total_heap, mem_total_str, sizeof(mem_total_str));
    cJSON_AddStringToObject(ctx, "mem_free", mem_free_str);
    cJSON_AddStringToObject(ctx, "mem_alloc", mem_alloc_str);
    cJSON_AddStringToObject(ctx, "mem_total", mem_total_str);
    cJSON_AddNumberToObject(ctx, "mem_pct", mem_pct);

    /* Storage (SPIFFS partition usage) */
    size_t spiffs_total = 0, spiffs_used = 0;
    char storage_total_str[32] = "N/A", storage_free_str[32] = "N/A", storage_used_str[32] = "N/A";
    int storage_pct = 0;
    if (esp_spiffs_info("storage", &spiffs_total, &spiffs_used) == ESP_OK) {
        size_t spiffs_free = (spiffs_total > spiffs_used) ? (spiffs_total - spiffs_used) : 0;
        fmt_bytes(spiffs_total, storage_total_str, sizeof(storage_total_str));
        fmt_bytes(spiffs_free, storage_free_str, sizeof(storage_free_str));
        fmt_bytes(spiffs_used, storage_used_str, sizeof(storage_used_str));
        storage_pct = spiffs_total ? (int)((uint64_t)spiffs_used * 100 / spiffs_total) : 0;
    }
    cJSON_AddStringToObject(ctx, "storage_total", storage_total_str);
    cJSON_AddStringToObject(ctx, "storage_free", storage_free_str);
    cJSON_AddStringToObject(ctx, "storage_used", storage_used_str);
    cJSON_AddNumberToObject(ctx, "storage_pct", storage_pct);

    /* WiFi / hostname */
    bool wifi_connected = wifi_manager_is_connected();
    cJSON_AddStringToObject(ctx, "wifi_connected", wifi_connected ? "Yes" : "No");
    cJSON_AddStringToObject(ctx, "ip_address", wifi_connected ? wifi_manager_get_ip() : "N/A");

    persistent_dict_t *sys_store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *sys_all = sys_store ? persistent_dict_get_dup(sys_store, "hostname") : NULL;
    const char *hostname = (sys_all && cJSON_IsString(sys_all) && sys_all->valuestring[0])
                                ? sys_all->valuestring
                                : "lightmotron";
    cJSON_AddStringToObject(ctx, "hostname", hostname);
    if (sys_all) cJSON_Delete(sys_all);

    cJSON *wifi_cfg = sys_store ? persistent_dict_get_dup(sys_store, "wifi") : NULL;
    cJSON_AddStringToObject(ctx, "wifi_ssid", json_get_string(wifi_cfg, "ssid", ""));
    if (wifi_cfg) cJSON_Delete(wifi_cfg);

    /* CPU / runtime */
    cJSON_AddNumberToObject(ctx, "cpu_freq_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddStringToObject(ctx, "reset_cause", comms_reset_reason_name());

    uint32_t uptime = timing_uptime_seconds();
    char uptime_str[32];
    snprintf(uptime_str, sizeof(uptime_str), "%uh %um %us",
             (unsigned)(uptime / 3600),
             (unsigned)((uptime % 3600) / 60),
             (unsigned)(uptime % 60));
    cJSON_AddStringToObject(ctx, "uptime", uptime_str);
    cJSON_AddBoolToObject(ctx, "animation_running", animation_is_running());
    cJSON_AddNumberToObject(ctx, "tick_number", animation_get_tick());

    /* Scene / model */
    char active_names[MAX_ACTIVE_SCENES][64];
    int active_count = lighting_get_active_scenes(active_names, MAX_ACTIVE_SCENES);
    cJSON_AddStringToObject(ctx, "current_scene", active_count > 0 ? active_names[active_count - 1] : "");
    cJSON_AddStringToObject(ctx, "active_model", lighting_get_current_model());

    /* Restore-result banner (redirected from /restore?...) */
    const char *restore_status = request_get_query_param(req, "restore");
    const char *restore_message = "";
    const char *restore_class = "";
    if (restore_status && strcmp(restore_status, "ok") == 0) {
        restore_message = "Storage restored successfully.";
        restore_class = "success";
    } else if (restore_status && strcmp(restore_status, "invalid") == 0) {
        restore_message = "Restore failed: invalid JSON data.";
        restore_class = "danger";
    }
    cJSON_AddStringToObject(ctx, "restore_message", restore_message);
    cJSON_AddStringToObject(ctx, "restore_class", restore_class);

    add_audio_health_context(ctx);

    cJSON_AddStringToObject(ctx, "page_title", "Status");

    return webserver_render_response("status.html", ctx);
}

http_response_t *view_setup(http_request_t *req)
{
    (void)req;
    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "current_model", lighting_get_current_model());
    return webserver_render_response("setup.html", ctx);
}

http_response_t *view_confirm(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");
    const char *target = request_get_form_field(req, "target");

    cJSON *ctx = build_global_context();
    if (action) cJSON_AddStringToObject(ctx, "action", action);
    if (target) cJSON_AddStringToObject(ctx, "target", target);

    return webserver_render_response("setup/confirm_delete.html", ctx);
}
