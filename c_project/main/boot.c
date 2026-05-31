#include "boot.h"
#include "settings.h"
#include "persistent_dict.h"
#include "wifi_manager.h"
#include "mdns_setup.h"
#include "captive_portal.h"
#include "webserver.h"
#include "routes.h"
#include "leds.h"
#include "animation.h"
#include "lighting.h"
#include "audio_player.h"
#include "sound_manager.h"
#include "billboard.h"
#include "comms.h"
#include "timing.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "boot";

esp_err_t boot_seed_defaults(void)
{
    persistent_dict_t *sys_settings = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    if (!sys_settings) {
        ESP_LOGE(TAG, "Failed to open system settings storage");
        return ESP_FAIL;
    }

    /* Seed hostname */
    if (!persistent_dict_get(sys_settings, "hostname")) {
        cJSON *val = cJSON_CreateString(DEFAULT_HOSTNAME);
        persistent_dict_set(sys_settings, "hostname", val);
    }

    /* Seed theme */
    if (!persistent_dict_get(sys_settings, "theme")) {
        cJSON *val = cJSON_CreateString(DEFAULT_THEME);
        persistent_dict_set(sys_settings, "theme", val);
    }

    /* Seed WiFi */
    if (!persistent_dict_get(sys_settings, "wifi")) {
        cJSON *wifi = cJSON_CreateObject();
        cJSON_AddStringToObject(wifi, "ssid", DEFAULT_WIFI_SSID);
        cJSON_AddStringToObject(wifi, "password", DEFAULT_WIFI_PASSWORD);
        cJSON_AddBoolToObject(wifi, "blink_on_connect", DEFAULT_WIFI_BLINK_ON_CONNECT);
        cJSON_AddBoolToObject(wifi, "print_on_connect", DEFAULT_WIFI_PRINT_ON_CONNECT);
        persistent_dict_set(sys_settings, "wifi", wifi);
    }

    /* Seed pins */
    if (!persistent_dict_get(sys_settings, "pins")) {
        cJSON *pins = cJSON_CreateObject();
        cJSON_AddNumberToObject(pins, "led", DEFAULT_PIN_LED);
        cJSON_AddNumberToObject(pins, "button", DEFAULT_PIN_BUTTON);
        cJSON_AddNumberToObject(pins, "scl", DEFAULT_PIN_SCL);
        cJSON_AddNumberToObject(pins, "sda", DEFAULT_PIN_SDA);
        persistent_dict_set(sys_settings, "pins", pins);
    }

    /* Seed neopixels */
    if (!persistent_dict_get(sys_settings, "neopixels")) {
        cJSON *neopixels = cJSON_CreateArray();
        cJSON *strip = cJSON_CreateObject();
        cJSON_AddNumberToObject(strip, "pin", DEFAULT_NEOPIXEL_PIN);
        cJSON_AddNumberToObject(strip, "num", DEFAULT_NEOPIXEL_NUM);
        cJSON_AddStringToObject(strip, "color_order", DEFAULT_NEOPIXEL_COLOR_ORDER);
        cJSON_AddBoolToObject(strip, "brightness_curve", DEFAULT_NEOPIXEL_BRIGHTNESS_CURVE);
        cJSON_AddItemToArray(neopixels, strip);
        persistent_dict_set(sys_settings, "neopixels", neopixels);
    }

    /* Seed audio_players (empty array) */
    if (!persistent_dict_get(sys_settings, "audio_players")) {
        cJSON *players = cJSON_CreateArray();
        persistent_dict_set(sys_settings, "audio_players", players);
    }

    /* Seed audio flags */
    if (!persistent_dict_get(sys_settings, "audio_reset_on_boot")) {
        cJSON *val = cJSON_CreateBool(DEFAULT_AUDIO_RESET_ON_BOOT);
        persistent_dict_set(sys_settings, "audio_reset_on_boot", val);
    }

    if (!persistent_dict_get(sys_settings, "audio_debug_logging")) {
        cJSON *val = cJSON_CreateBool(DEFAULT_AUDIO_DEBUG_LOGGING);
        persistent_dict_set(sys_settings, "audio_debug_logging", val);
    }

    /* Seed billboard */
    if (!persistent_dict_get(sys_settings, "billboard")) {
        cJSON *billboard = cJSON_CreateObject();
        cJSON_AddNumberToObject(billboard, "mosi", DEFAULT_BILLBOARD_MOSI);
        cJSON_AddNumberToObject(billboard, "clk", DEFAULT_BILLBOARD_CLK);
        cJSON_AddNumberToObject(billboard, "cs", DEFAULT_BILLBOARD_CS);
        cJSON_AddNumberToObject(billboard, "num_modules", DEFAULT_BILLBOARD_NUM_MODULES);
        persistent_dict_set(sys_settings, "billboard", billboard);
    }

    persistent_dict_save(sys_settings);

    /* Seed lighting settings if file doesn't exist */
    persistent_dict_t *lighting = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    if (lighting && !persistent_dict_get(lighting, "models")) {
        cJSON *models = cJSON_CreateObject();
        cJSON *default_model = cJSON_CreateObject();
        cJSON_AddItemToObject(default_model, "scenes", cJSON_CreateObject());
        cJSON_AddItemToObject(default_model, "effects", cJSON_CreateObject());
        cJSON_AddItemToObject(default_model, "filters", cJSON_CreateObject());
        cJSON_AddItemToObject(default_model, "named_ranges", cJSON_CreateObject());
        cJSON_AddItemToObject(default_model, "custom_colors", cJSON_CreateObject());
        cJSON_AddItemToObject(default_model, "soundscapes", cJSON_CreateObject());
        cJSON_AddItemToObject(default_model, "scene_settings", cJSON_CreateObject());
        cJSON_AddItemToObject(models, "Model", default_model);
        persistent_dict_set(lighting, "models", models);

        cJSON *current = cJSON_CreateString("Model");
        persistent_dict_set(lighting, "current_model", current);
        persistent_dict_save(lighting);
    }

    /* Seed sounds if file doesn't exist */
    persistent_dict_t *sounds = persistent_dict_open(STORAGE_SOUNDS_FILE);
    if (sounds && !persistent_dict_get(sounds, "sounds")) {
        cJSON *sounds_obj = cJSON_CreateObject();
        persistent_dict_set(sounds, "sounds", sounds_obj);
        persistent_dict_save(sounds);
    }

    ESP_LOGI(TAG, "Default settings seeded");
    return ESP_OK;
}

esp_err_t boot_init(void)
{
    esp_err_t ret;

    /* Initialize NVS */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* Seed random number generator */
    srand((unsigned int)esp_log_timestamp());

    /* Initialize SPIFFS */
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = STORAGE_MOUNT_POINT,
        .partition_label = "storage",
        .max_files = 10,
        .format_if_mount_failed = true,
    };
    ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SPIFFS mounted");

    /* Seed defaults */
    ret = boot_seed_defaults();
    if (ret != ESP_OK) {
        return ret;
    }

    /* Read WiFi credentials */
    persistent_dict_t *sys_settings = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    cJSON *wifi_obj = persistent_dict_get(sys_settings, "wifi");
    const char *ssid = "";
    const char *password = "";
    if (wifi_obj) {
        cJSON *ssid_json = cJSON_GetObjectItem(wifi_obj, "ssid");
        cJSON *pass_json = cJSON_GetObjectItem(wifi_obj, "password");
        if (ssid_json && ssid_json->valuestring) {
            ssid = ssid_json->valuestring;
        }
        if (pass_json && pass_json->valuestring) {
            password = pass_json->valuestring;
        }
    }

    /* Attempt WiFi connection */
    bool wifi_connected = false;
    if (strlen(ssid) > 0) {
        ret = wifi_manager_init();
        if (ret == ESP_OK) {
            ret = wifi_manager_connect(ssid, password);
            if (ret == ESP_OK) {
                wifi_connected = true;
                ESP_LOGI(TAG, "WiFi connected");

                /* Set up mDNS */
                cJSON *hostname_json = persistent_dict_get(sys_settings, "hostname");
                const char *hostname = DEFAULT_HOSTNAME;
                if (hostname_json && hostname_json->valuestring) {
                    hostname = hostname_json->valuestring;
                }
                mdns_setup_init(hostname);

                /* Blink LED on connect if configured */
                cJSON *blink = cJSON_GetObjectItem(wifi_obj, "blink_on_connect");
                if (blink && cJSON_IsTrue(blink)) {
                    comms_blink_status_led(3, 100, 100);
                }
            }
        }
    }

    /* If WiFi failed, start captive portal */
    if (!wifi_connected) {
        ESP_LOGW(TAG, "WiFi not connected, starting captive portal");
        captive_portal_start();
        return ESP_OK; /* Captive portal handles everything from here */
    }

    /* Register web routes BEFORE starting server */
    routes_register_all();

    /* Start web server */
    ret = webserver_start(DEFAULT_HTTP_PORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return ret;
    }
    ESP_LOGI(TAG, "Web server started on port %d", DEFAULT_HTTP_PORT);

    /* Initialize LEDs */
    cJSON *neopixels = persistent_dict_get(sys_settings, "neopixels");
    if (neopixels && cJSON_IsArray(neopixels)) {
        leds_init_from_config(neopixels);
        ESP_LOGI(TAG, "LEDs initialized");
    }

    /* Initialize billboard */
    cJSON *billboard_cfg = persistent_dict_get(sys_settings, "billboard");
    if (billboard_cfg) {
        int mosi = cJSON_GetObjectItem(billboard_cfg, "mosi")->valueint;
        int clk = cJSON_GetObjectItem(billboard_cfg, "clk")->valueint;
        int cs = cJSON_GetObjectItem(billboard_cfg, "cs")->valueint;
        int num = cJSON_GetObjectItem(billboard_cfg, "num_modules")->valueint;
        int brightness = 7; /* default mid brightness */
        cJSON *brt = cJSON_GetObjectItem(billboard_cfg, "brightness");
        if (brt && cJSON_IsNumber(brt)) brightness = brt->valueint;
        billboard_init(mosi, clk, cs, num, brightness);
        ESP_LOGI(TAG, "Billboard initialized");
    }

    /* Initialize audio */
    cJSON *audio_players = persistent_dict_get(sys_settings, "audio_players");
    if (audio_players && cJSON_GetArraySize(audio_players) > 0) {
        audio_player_init_from_config(audio_players);
        cJSON *reset_on_boot = persistent_dict_get(sys_settings, "audio_reset_on_boot");
        if (reset_on_boot && cJSON_IsTrue(reset_on_boot)) {
            audio_player_reset_all();
        }
        cJSON *health = audio_player_check_health();
        if (health) cJSON_Delete(health);
        sound_manager_init();
        sound_manager_start_polling();
        ESP_LOGI(TAG, "Audio initialized");
    }

    /* Initialize lighting system */
    lighting_init();
    ESP_LOGI(TAG, "Lighting system initialized");

    /* Start animation */
    animation_start();
    ESP_LOGI(TAG, "Animation started");

    ESP_LOGI(TAG, "Boot complete");
    return ESP_OK;
}
