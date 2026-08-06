#include "boot.h"
#include "settings.h"
#include "persistent_dict.h"
#include "wifi_manager.h"
#include "mdns_setup.h"
#include "captive_portal.h"
#include "webserver.h"
#include "asset_cache.h"
#include "routes.h"
#include "leds.h"
#include "animation.h"
#include "lighting.h"
#include "audio_player.h"
#include "sound_manager.h"
#include "billboard.h"
#include "comms.h"
#include "timing.h"
#include "ip_announcement.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

    /* Seed stored_ip_address (tracks the last-announced IP; mirrors
     * settings.py's _SYSTEM_SETTINGS_DEFAULTS["stored_ip_address"]) */
    if (!persistent_dict_get(sys_settings, "stored_ip_address")) {
        cJSON *val = cJSON_CreateString(DEFAULT_STORED_IP_ADDRESS);
        persistent_dict_set(sys_settings, "stored_ip_address", val);
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

/* Argument passed to ip_flash_task(): a private copy of the IP address
 * string, since the background task outlives the boot_init() stack frame. */
typedef struct {
    char ip[16];
} ip_flash_task_arg_t;

/**
 * Background task: flash the onboard LED to report the last octet of the
 * device's IP address, three times over. Mirrors boot.py's
 * _flash_ip_last_octet()/_run_ip_flash_sequence(): the last octet is split
 * into hundreds/tens/ones digits, flashed red/green/blue respectively (1s
 * on, 1s off per flash, digits with value 0 skipped), with a 2s gap between
 * each of the 3 repeats.
 */
static void ip_flash_task(void *pvParameters)
{
    ip_flash_task_arg_t *arg = (ip_flash_task_arg_t *)pvParameters;

    esp_err_t ret = onboard_led_init(-1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Onboard LED init failed, skipping IP flash sequence");
        free(arg);
        vTaskDelete(NULL);
        return;
    }

    const char *last_dot = strrchr(arg->ip, '.');
    int last_octet = last_dot ? atoi(last_dot + 1) : 0;
    int hundreds = last_octet / 100;
    int tens = (last_octet % 100) / 10;
    int ones = last_octet % 10;

    struct {
        int count;
        uint8_t r, g, b;
    } digit_colors[3] = {
        { hundreds, 255, 0, 0 },
        { tens, 0, 255, 0 },
        { ones, 0, 0, 255 },
    };

    for (int flash_set = 0; flash_set < 3; flash_set++) {
        for (int i = 0; i < 3; i++) {
            if (digit_colors[i].count == 0) {
                continue;
            }
            for (int n = 0; n < digit_colors[i].count; n++) {
                onboard_led_set(digit_colors[i].r, digit_colors[i].g, digit_colors[i].b);
                vTaskDelay(pdMS_TO_TICKS(1000));
                onboard_led_off();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        if (flash_set < 2) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    free(arg);
    vTaskDelete(NULL);
}

/**
 * Spawn ip_flash_task() as a background FreeRTOS task so it doesn't block
 * the rest of boot. Mirrors boot.py's _thread.start_new_thread(...) call.
 */
static void start_ip_flash_sequence(const char *ip_address)
{
    ip_flash_task_arg_t *arg = malloc(sizeof(ip_flash_task_arg_t));
    if (!arg) {
        ESP_LOGW(TAG, "Failed to allocate IP flash task argument");
        return;
    }

    strncpy(arg->ip, ip_address, sizeof(arg->ip) - 1);
    arg->ip[sizeof(arg->ip) - 1] = '\0';

    const char *last_dot = strrchr(arg->ip, '.');
    ESP_LOGI(TAG, "Flashing IP last octet: %s", last_dot ? last_dot + 1 : arg->ip);

    if (xTaskCreate(ip_flash_task, "ip_flash", 2560, arg, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create IP flash task");
        free(arg);
    }
}

esp_err_t boot_init(void)
{
    esp_err_t ret;

    /* Log why the MCU restarted — distinguishes a true power-on from
     * brownout/watchdog/panic/soft resets when diagnosing unexpected
     * reboots or hangs (mirrors boot.py's reset-cause print). */
    ESP_LOGI(TAG, "Reset cause: %s", comms_reset_reason_name());

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

    /* Initialize LittleFS - two separate partitions (see partitions.csv):
     * "webassets" (www/templates, rebuilt and rewritten on every
     * `idf.py flash`) and "data" (user settings, never touched by
     * flashing). Keeping them separate means a normal code/asset deploy
     * can never wipe scenes/effects/colors/WiFi credentials/etc.
     *
     * Migrated from SPIFFS 2026-08-05 (see LITTLEFS_MIGRATION_NOTES.md) --
     * SPIFFS's garbage-collection pauses were repeatedly tripping the
     * interrupt watchdog during ordinary settings writes. base_path/
     * partition_label are unchanged so nothing else in the app needed to
     * change; only the on-flash format did. */
    esp_vfs_littlefs_conf_t webassets_conf = {
        .base_path = STORAGE_MOUNT_POINT,
        .partition_label = "webassets",
        .format_if_mount_failed = true,
    };
    ret = esp_vfs_littlefs_register(&webassets_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount webassets LittleFS (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "webassets LittleFS mounted");

    esp_vfs_littlefs_conf_t data_conf = {
        .base_path = DATA_MOUNT_POINT,
        .partition_label = "data",
        .format_if_mount_failed = true,
    };
    ret = esp_vfs_littlefs_register(&data_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount data LittleFS (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "data LittleFS mounted");

    /* Force LittleFS's janitorial work (metadata-pair compaction, orphan
     * cleanup, block-allocator population -- see lfs_fs_gc() in
     * managed_components/joltwallet__littlefs) here at boot, before
     * anything else touches either partition, instead of letting it
     * happen implicitly inside whatever write triggers it first. This is
     * the same lfs_dir_splittingcompact work that's been tripping the
     * interrupt watchdog during ordinary settings saves (see
     * LITTLEFS_MIGRATION_NOTES.md) -- running it here pays down any
     * accumulated compaction debt at boot, when nothing else is
     * contending for CPU/flash and a stall is easy to diagnose, instead
     * of mid-request. esp_littlefs doesn't expose lfs_fs_gc() itself, so
     * esp_littlefs_gc() (added to the vendored component alongside this
     * change) is a thin wrapper around it. Not fatal if it fails --
     * that just means the debt carries over to the next write, same as
     * before this existed. */
    uint32_t gc_start = esp_log_timestamp();
    ret = esp_littlefs_gc("data");
    ESP_LOGI(TAG, "data LittleFS gc: %s (%ums)", esp_err_to_name(ret),
             (unsigned)(esp_log_timestamp() - gc_start));

    gc_start = esp_log_timestamp();
    ret = esp_littlefs_gc("webassets");
    ESP_LOGI(TAG, "webassets LittleFS gc: %s (%ums)", esp_err_to_name(ret),
             (unsigned)(esp_log_timestamp() - gc_start));

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

    /* Resolve hostname (falls back to "lightmotron" if unset/empty), matching
     * boot.py's `_stored_hostname if _stored_hostname else "lightmotron"`. */
    cJSON *hostname_json = persistent_dict_get(sys_settings, "hostname");
    const char *hostname = DEFAULT_HOSTNAME;
    if (hostname_json && hostname_json->valuestring && strlen(hostname_json->valuestring) > 0) {
        hostname = hostname_json->valuestring;
    }

    /* Attempt WiFi connection. Mirrors boot.py's
     * `if not _wifi_ssid or not _wifi_password:` gate -- both must be
     * present or we skip straight to the captive portal below. */
    bool wifi_connected = false;
    if (strlen(ssid) > 0 && strlen(password) > 0) {
        ret = wifi_manager_init();
        if (ret == ESP_OK) {
            /* Set the DHCP/mDNS hostname before connecting, mirroring
             * boot.py's network.hostname() call which happens before
             * WIFIManager connects so it's used for the whole session. */
            wifi_manager_set_hostname(hostname);

            ret = wifi_manager_connect(ssid, password);
            if (ret == ESP_OK) {
                wifi_connected = true;
                ESP_LOGI(TAG, "WiFi connected");

                /* Set up mDNS */
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

    /* Print hostname/IP info, mirroring boot.py's print() calls */
    const char *active_ip = wifi_manager_get_ip();
    ESP_LOGI(TAG, "Hostname: %s", hostname);
    ESP_LOGI(TAG, "Home URL (mDNS): http://%s.local/", hostname);
    ESP_LOGI(TAG, "Home URL (IP):   http://%s/", active_ip);

    /* Flash the onboard LED with the IP's last octet in the background,
     * mirroring boot.py's _run_ip_flash_sequence() thread. */
    start_ip_flash_sequence(active_ip);

    /* Load all web assets into RAM before the server starts, so requests are
     * served from memory and never read flash (see asset_cache.h). Done here,
     * before audio init below, so the caching reads themselves don't overlap
     * any UART activity. */
    asset_cache_init();
    ESP_LOGI(TAG, "Free internal RAM after asset cache: %u bytes (largest block %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    /* Register web routes now (doesn't accept connections yet -- just
     * builds the route table), but hold off on webserver_start() until
     * every subsystem a page/handler can touch (LEDs, audio, lighting) is
     * initialized below. Starting the server earlier left a real window
     * where an incoming request's client_task could call into lighting.c
     * before lighting_init() had created lighting_mutex -- e.g.
     * lighting_get_active_scenes() taking a still-NULL mutex, asserting. */
    routes_register_all();

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

    /* Check if IP needs to be announced (mirrors boot.py's
     * check_and_announce_ip() call). Must come after audio init above --
     * this plays a sound through the configured module(s), which don't
     * exist yet any earlier in boot; running it before init meant the very
     * first announcement after every boot silently failed with "No audio
     * modules configured" even when a module was configured. */
    ip_announcement_check_and_announce();

    /* Initialize lighting system */
    lighting_init();
    ESP_LOGI(TAG, "Lighting system initialized");

    /* Start web server -- now that LEDs/billboard/audio/lighting are all
     * initialized, it's safe for a request to reach any page/handler. */
    ret = webserver_start(DEFAULT_HTTP_PORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return ret;
    }
    ESP_LOGI(TAG, "Web server started on port %d", DEFAULT_HTTP_PORT);

    /* Activate whichever scene(s) are marked active_on_boot, or fall back
     * to an arbitrary single scene if none are marked -- matches
     * Lighting.__init__()'s boot-scene activation in lib/lighting/lighting.py. */
    lighting_activate_boot_scenes();

    /* Start animation */
    animation_start();
    ESP_LOGI(TAG, "Animation started");

    ESP_LOGI(TAG, "Boot complete");
    return ESP_OK;
}
