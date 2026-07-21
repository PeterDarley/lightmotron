#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

/* Default system settings constants */

#define DEFAULT_HOSTNAME "lightmotron"
#define DEFAULT_THEME ""
#define DEFAULT_STORED_IP_ADDRESS ""

/* WiFi defaults */
#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASSWORD ""
#define DEFAULT_WIFI_BLINK_ON_CONNECT true
#define DEFAULT_WIFI_PRINT_ON_CONNECT true

/* GPIO pin defaults */
#define DEFAULT_PIN_LED 2
#define DEFAULT_PIN_BUTTON 0
#define DEFAULT_PIN_SCL 22
#define DEFAULT_PIN_SDA 21

/* NeoPixel defaults */
#define DEFAULT_NEOPIXEL_PIN 4
#define DEFAULT_NEOPIXEL_NUM 144
#define DEFAULT_NEOPIXEL_COLOR_ORDER "GRB"
#define DEFAULT_NEOPIXEL_BRIGHTNESS_CURVE true

/* Billboard (MAX7219) SPI defaults */
#define DEFAULT_BILLBOARD_MOSI 23
#define DEFAULT_BILLBOARD_CLK 18
#define DEFAULT_BILLBOARD_CS 5
#define DEFAULT_BILLBOARD_NUM_MODULES 4

/* Audio defaults */
#define DEFAULT_AUDIO_RESET_ON_BOOT true
#define DEFAULT_AUDIO_DEBUG_LOGGING false
#define MAX_AUDIO_PLAYERS 3

/* Animation defaults */
#define DEFAULT_FRAME_INTERVAL_MS 25

/* Web server defaults */
#define DEFAULT_HTTP_PORT 80
#define MAX_REQUEST_BODY_SIZE (512 * 1024)
#define HTTP_RECV_BUFFER_SIZE 4096
#define STATIC_FILE_CHUNK_SIZE 4096
#define KEEPALIVE_TIMEOUT_S 5
#define MAX_CONCURRENT_CLIENTS 4

/* Storage paths */
#define STORAGE_MOUNT_POINT "/spiffs"
/* STORAGE_SYSTEM_SETTINGS_FILE / STORAGE_LIGHTING_SETTINGS_FILE / STORAGE_SOUNDS_FILE
 * live in components/storage/include/persistent_dict.h (the canonical source of
 * truth reachable by every component that depends on storage, not just main). */
#define TEMPLATES_DIR "/spiffs/templates"
#define WWW_DIR "/spiffs/www"

/* Named color count */
#define MAX_NAMED_COLORS 32
#define MAX_SCENES 16
#define MAX_JOBS_PER_SCENE 16
#define MAX_EFFECTS 32
#define MAX_FILTERS 16
#define MAX_NAMED_RANGES 32
#define MAX_LEDS 300
#define MAX_STRIPS 4

#endif /* SETTINGS_H */
