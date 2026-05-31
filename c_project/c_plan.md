# Lightmotron C Port Plan

## Overview

Port the entire MicroPython Lightmotron system to C, targeting the ESP32-S3 via ESP-IDF (Espressif's official C SDK). The C project lives entirely in `www/c_project/` and replicates all behaviors and interfaces exactly.

---

## Target Platform

- **MCU:** ESP32-S3-WROOM-1 (N16R8 — 16MB flash, 8MB PSRAM)
- **SDK:** ESP-IDF v5.x (FreeRTOS-based)
- **Build:** CMake (ESP-IDF standard)

---

## Directory Structure

```
c_project/
├── CMakeLists.txt                  # Top-level ESP-IDF project CMake
├── sdkconfig.defaults              # Default Kconfig settings (PSRAM, flash, WiFi)
├── partitions.csv                  # Custom partition table (large SPIFFS for www/)
├── main/
│   ├── CMakeLists.txt              # Main component CMake
│   ├── main.c                      # Entry point (app_main)
│   ├── boot.c / boot.h            # Startup sequence, seed defaults
│   └── settings.h                  # Default settings constants
├── components/
│   ├── webserver/
│   │   ├── CMakeLists.txt
│   │   ├── webserver.c / .h        # HTTP server (socket, routing, keep-alive)
│   │   ├── template_engine.c / .h  # Custom template renderer
│   │   ├── request_parser.c / .h   # Request line, headers, body, form parsing
│   │   ├── response.c / .h         # Response construction & sending
│   │   ├── static_files.c / .h     # Static file serving with ETag/caching
│   │   └── mime_types.c / .h       # MIME type lookup
│   ├── storage/
│   │   ├── CMakeLists.txt
│   │   ├── persistent_dict.c / .h  # JSON-backed persistent key-value store
│   │   └── json_helpers.c / .h     # cJSON wrappers for typed access
│   ├── lighting/
│   │   ├── CMakeLists.txt
│   │   ├── lighting.c / .h         # Lighting singleton, scene execution
│   │   ├── animation.c / .h        # Animation thread (25ms tick loop)
│   │   ├── patterns.c / .h         # Pattern functions (solid, blink, pulse, wave, etc.)
│   │   ├── filters.c / .h          # Filter functions (brightness, sizzle, spike, etc.)
│   │   ├── effects.c / .h          # Effect resolution & runtime state
│   │   ├── colors.c / .h           # Color resolution (named, hex, RGB)
│   │   ├── named_ranges.c / .h     # Named range resolution
│   │   └── metadata.c / .h         # Scene metadata (kills, sounds, etc.)
│   ├── leds/
│   │   ├── CMakeLists.txt
│   │   ├── leds.c / .h             # NeoPixel driver (RMT peripheral)
│   │   └── led_strip.c / .h        # Multi-strip management, color order
│   ├── audio/
│   │   ├── CMakeLists.txt
│   │   ├── audio_player.c / .h     # AudioPlayer singleton (multi-module mgmt)
│   │   ├── yx5200.c / .h           # YX5200 UART driver (DFPlayer protocol)
│   │   └── sound_manager.c / .h    # SoundManager (play, loop, chain, stop)
│   ├── billboard/
│   │   ├── CMakeLists.txt
│   │   ├── billboard.c / .h        # High-level MAX7219 (text, scroll)
│   │   └── max7219.c / .h          # Low-level SPI framebuffer driver
│   ├── network/
│   │   ├── CMakeLists.txt
│   │   ├── wifi_manager.c / .h     # WiFi STA connection, reconnect
│   │   ├── captive_portal.c / .h   # AP mode + DNS hijack for setup
│   │   ├── mdns_setup.c / .h       # mDNS hostname registration
│   │   └── ota_update.c / .h       # GitHub OTA (chunked HTTP download)
│   ├── web/
│   │   ├── CMakeLists.txt
│   │   ├── routes.c / .h           # Route registration table
│   │   ├── views.c / .h            # All view handlers (GET/POST dispatch)
│   │   ├── views_named_ranges.c/.h # Named range views
│   │   ├── views_scenes.c / .h     # Scene CRUD views
│   │   ├── views_effects.c / .h    # Effect CRUD views
│   │   ├── views_filters.c / .h    # Filter CRUD views
│   │   ├── views_sounds.c / .h     # Sound views
│   │   ├── views_soundscapes.c / .h # Soundscape views
│   │   ├── views_system.c / .h     # System settings, status, reboot
│   │   ├── views_storage.c / .h    # Backup/restore views
│   │   └── context_processors.c/.h # Global context injection (theme)
│   └── util/
│       ├── CMakeLists.txt
│       ├── timing.c / .h           # FreeRTOS timer wrappers
│       ├── comms.c / .h            # Onboard LED blink, WiFi status
│       └── utils.c / .h            # General utilities
├── data/                           # SPIFFS filesystem image (www/, templates/)
│   ├── www/                        # Static web assets (copied from parent project)
│   └── templates/                  # HTML templates (copied from parent project)
└── c_plan.md                       # This file
```

---

## Module-by-Module Port Plan

### Phase 1: Foundation

#### 1.1 Build System & Platform Init
- ESP-IDF CMake project skeleton
- Partition table: factory app + large SPIFFS (for www/templates/data)
- sdkconfig: enable PSRAM, set flash size 16MB, enable WiFi, enable LWIP
- `app_main()` → init NVS, SPIFFS, event loop

#### 1.2 Storage (`components/storage/`)
- **PersistentDict** → C struct wrapping cJSON objects + SPIFFS file I/O
- Lazy loading: file read on first access, flag tracks loaded state
- API:
  ```c
  persistent_dict_t* persistent_dict_open(const char* filename);
  cJSON* persistent_dict_get(persistent_dict_t* pd, const char* key);
  void persistent_dict_set(persistent_dict_t* pd, const char* key, cJSON* value);
  void persistent_dict_save(persistent_dict_t* pd);
  void persistent_dict_delete_key(persistent_dict_t* pd, const char* key);
  ```
- Thread-safe: FreeRTOS mutex per dict instance
- Use cJSON library (bundled with ESP-IDF) for all JSON operations

#### 1.3 Settings (`main/settings.h`)
- Compile-time defaults as `#define` constants
- Runtime settings read from storage with fallback to defaults
- Proxy functions: `settings_get_wifi_ssid()`, `settings_get_neopixel_pin()`, etc.

### Phase 2: Networking

#### 2.1 WiFi Manager (`components/network/wifi_manager.c`)
- ESP-IDF WiFi STA init with event handlers
- Read SSID/password from storage
- Auto-reconnect with exponential backoff
- Status tracking: connected/disconnected/connecting
- Onboard LED blink on connect (GPIO 2)

#### 2.2 Captive Portal (`components/network/captive_portal.c`)
- Fallback when no WiFi credentials or connection fails
- Start SoftAP: SSID "lightmotron-setup"
- DNS server task: respond to ALL queries with AP IP (192.168.4.1)
- Serve setup page on HTTP, accept SSID/password form POST
- Save credentials, reboot

#### 2.3 mDNS (`components/network/mdns_setup.c`)
- Register hostname via ESP-IDF mDNS component
- `<hostname>.local` → device IP

### Phase 3: Web Server

#### 3.1 HTTP Server (`components/webserver/`)
- **Socket layer:** BSD sockets (LWIP), SO_REUSEADDR, listen(1)
- **Thread model:** FreeRTOS task per client (configurable stack 8KB)
- **Keep-alive:** 5s idle timeout, HTTP/1.1 default keep-alive
- **Request parsing:**
  - Read line-by-line from socket (4KB initial buffer)
  - Parse method + path + version
  - Parse headers into linked list
  - Read body by Content-Length
  - URL-decode query string
  - Parse form bodies (URL-encoded + multipart)
- **Routing:**
  - Static route table (array of `{method, path, handler_func}`)
  - Trailing-slash tolerance
  - Fallback to static file serving
- **Response:**
  - `http_response_t` struct: status, headers, body (buffer or file path)
  - Chunked file streaming (4KB)
  - ETag generation: `"<size>-<mtime>"`
  - Cache-Control: no-cache for .js/.css, 30-day for others
- **Max request body:** 512KB

#### 3.2 Template Engine (`components/webserver/template_engine.c`)
- **Processing order:** for-loops → conditionals → variable substitution → includes
- **Variables:** `{{ name }}` → context lookup, dot-notation for nested access
- **For loops:** `{% for x in list %}...{% endfor %}`
  - Support: plain lists, dict.items()/keys()/values(), range(n), tuple unpacking
  - Nested loops with context cloning
- **Conditionals:** `{% if expr %}...{% else %}...{% endif %}`
  - Operators: `==`, `!=`, `in`, `not in`, `and`, `or`, `not`
  - Truthiness: NULL, empty string, empty array, 0 → false
- **Includes:** `{% include 'file' %}` → recursive render
- **Implementation approach:**
  - Template loaded as string from SPIFFS
  - Context as cJSON object (nested dicts/lists)
  - Regex-free: custom parser scanning for `{{`, `{%` delimiters
  - Output built via dynamic string buffer (realloc-based)

#### 3.3 Static File Serving (`components/webserver/static_files.c`)
- Serve from SPIFFS `/www/` partition
- MIME type lookup by extension
- ETag + conditional GET (If-None-Match → 304)
- Stream in 4KB chunks

### Phase 4: Hardware Drivers

#### 4.1 NeoPixel LEDs (`components/leds/`)
- ESP-IDF RMT (Remote Control) peripheral for WS2812B timing
- Multi-strip support: each strip has pin, count, color_order, brightness_curve
- API:
  ```c
  void leds_init(const neopixel_config_t* configs, int num_strips);
  void leds_set_pixel(int strip, int index, uint8_t r, uint8_t g, uint8_t b);
  void leds_show(void);  // Push buffer to hardware
  void leds_clear(void);
  int leds_total_count(void);
  ```
- Color order remapping: GRB, RGB, RGBW
- Brightness curve (gamma correction) option

#### 4.2 MAX7219 Billboard (`components/billboard/`)
- SPI master driver (ESP-IDF SPI)
- 4 chained 8×8 modules = 32×8 framebuffer
- API:
  ```c
  void billboard_init(int mosi_pin, int clk_pin, int cs_pin, int num_modules);
  void billboard_set_brightness(int level);  // 0-15
  void billboard_show_text(const char* text);
  void billboard_scroll_text(const char* text, int delay_ms);
  void billboard_clear(void);
  ```
- Font: 8×8 pixel font embedded as const array

#### 4.3 Audio / YX5200 (`components/audio/`)
- UART driver per module (ESP-IDF UART)
- DFPlayer protocol: 10-byte frames
  ```c
  typedef struct {
      uint8_t start;      // 0x7E
      uint8_t version;    // 0xFF
      uint8_t length;     // 0x06
      uint8_t command;
      uint8_t feedback;   // 0x00 or 0x01
      uint8_t param_h;
      uint8_t param_l;
      uint8_t checksum_h;
      uint8_t checksum_l;
      uint8_t end;        // 0xEF
  } yx5200_frame_t;
  ```
- Commands: play_file, stop, pause, set_volume, query_status, reset
- AudioPlayer: manages up to 3 modules, auto-selects available one
- SoundManager: title→file mapping, looping, chaining, mutual exclusion

### Phase 5: Lighting System

#### 5.1 Animation (`components/lighting/animation.c`)
- FreeRTOS task running at 40Hz (25ms interval)
- Calls `lighting_process_tick()` each frame
- Start/stop/pause/resume with callbacks
- Stop-race protection: callbacks fire after tick loop exits

#### 5.2 Patterns (`components/lighting/patterns.c`)
- Function pointer table:
  ```c
  typedef void (*pattern_fn)(const effect_t* effect, int tick, led_output_t* output, int* output_count);
  ```
- Patterns: solid, blink, pulse, fade_in, breathe, wave, cylon, phaser_strip
- Each writes to output array: `[(led_index, r, g, b), ...]`

#### 5.3 Filters (`components/lighting/filters.c`)
- Function pointer table:
  ```c
  typedef void (*filter_fn)(const filter_config_t* config, led_output_t* target_colors,
                            const rgb_t* current_colors, int count, filter_state_t* state);
  ```
- Filters: null, brightness, sizzle, scintillate, spike, dropout
- Each receives target + current colors, applies diff to current
- Per-effect state struct for spike/dropout timing

#### 5.4 Scene Execution (`components/lighting/lighting.c`)
- Active scenes list (max 8 concurrent)
- Per-tick: iterate scenes → iterate jobs → call pattern → apply filters → update logical_colors
- Cycle tracking: auto-remove scene when all cycle-limited effects complete
- `after` dependency: job waits for predecessor to finish
- Scene metadata: kills, sound triggers, stop_sounds

#### 5.5 Colors & Named Ranges
- Color resolution: named colors table, hex parsing, RGB tuple
- Named ranges: resolve recursive `"named:X"` references to flat index array
- Custom colors: user-defined name→hex mapping stored in settings

### Phase 6: Web Views (All HTTP Endpoints)

#### 6.1 View Architecture
- Each view is a function pair: `view_name_get(request_t*)` + `view_name_post(request_t*)`
- Route table maps path → function pointer
- Standard pattern: parse form data → modify storage → render template or redirect

#### 6.2 Home Views
- `GET /` → render home.html (scene buttons, animation controls, sound buttons)
- `POST /set_scene` → set/add/remove active scene
- `POST /animation` → start/stop animation
- `GET /scenes/panel/status` → HTMX fragment for scene button state

#### 6.3 Setup Views (CRUD for all entities)
- `GET /setup` → render setup dashboard
- Models: create/delete/rename/set-active/copy
- Named ranges: create/edit/delete, subrange management
- Custom colors: add/edit/delete
- Scenes: create/delete/rename/copy, edit entries (add/update/remove jobs)
- Effects: create/delete, edit (pattern, colors, params, filter chain)
- Filters: create/delete, edit (type, params)
- Sounds: create/edit/delete, play/stop
- Soundscapes: create/edit/delete, play/stop
- System settings: neopixels, audio, pins, hostname
- Theme: select/upload/delete
- Updates: OTA check/apply

#### 6.4 Utility Views
- `GET /status` → system info (heap, WiFi, audio health, uptime)
- `GET /storage` → backup (download JSON), `POST /restore` → upload JSON
- `POST /confirm` → generic confirmation dialog handler

### Phase 7: OTA Updates

#### 7.1 OTA (`components/network/ota_update.c`)
- Check GitHub releases API for newer version
- Download firmware binary in chunks
- ESP-IDF OTA API: write to inactive partition, set boot, reboot

---

## Key C Implementation Decisions

### Memory Management
- **PSRAM available (8MB):** Use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` for large allocations (JSON data, template buffers)
- **Internal SRAM:** Reserve for DMA buffers (RMT, SPI, UART), FreeRTOS stacks
- **String buffers:** Dynamic realloc-based string builder for template output
- **JSON:** cJSON library (already in ESP-IDF) for all structured data

### Threading Model (FreeRTOS)
| Task | Core | Priority | Stack |
|------|------|----------|-------|
| Main (app_main) | 0 | 5 | 8KB |
| WiFi (system) | 0 | 23 | 4KB |
| HTTP server accept | 1 | 5 | 4KB |
| HTTP client handler | 1 | 4 | 8KB |
| Animation tick | 0 | 6 | 4KB |
| Audio polling | 0 | 3 | 4KB |
| Billboard scroll | 0 | 2 | 2KB |

### Data Flow
```
Storage (SPIFFS JSON files)
    ↕ cJSON parse/serialize
Settings Cache (in-memory cJSON trees)
    ↕ accessor functions
Lighting/Audio/Billboard subsystems
    ↕ hardware drivers
GPIO/SPI/UART/RMT peripherals
```

### Error Handling
- ESP-IDF `esp_err_t` return codes throughout
- `ESP_ERROR_CHECK()` for fatal init errors
- Graceful degradation for runtime errors (log + continue)
- Watchdog timer: 30s task WDT, reset on hang

### Synchronization
- FreeRTOS mutexes for shared state (storage, lighting, route table)
- Queue-based communication for cross-task events (scene change, sound trigger)
- Atomic operations for simple flags (animation running, WiFi connected)

---

## Implementation Order

1. **Build skeleton** — CMake, partition table, app_main stub
2. **Storage** — persistent_dict + cJSON, settings defaults
3. **WiFi + mDNS** — basic connectivity
4. **HTTP server** — socket accept, request parse, static file serve
5. **Template engine** — variable substitution, loops, conditionals, includes
6. **Route dispatch** — route table, view function dispatch
7. **NeoPixel driver** — RMT-based LED output
8. **Animation** — tick loop task
9. **Patterns + Filters** — all pattern/filter functions
10. **Lighting orchestration** — scene execution, named ranges, colors
11. **Home + Setup views** — full web UI (incremental, endpoint by endpoint)
12. **MAX7219 billboard** — SPI driver + text rendering
13. **Audio** — YX5200 UART driver + SoundManager
14. **Captive portal** — AP + DNS fallback
15. **OTA** — GitHub release check + firmware update
16. **Integration testing** — full system validation

---

## Interface Compatibility

The C port MUST preserve:
- All HTTP routes (same paths, methods, form field names)
- All template files (unchanged — same template syntax processed by C engine)
- All static assets (JS, CSS, fonts — unchanged)
- All JSON storage formats (same keys, same structure)
- All hardware pinouts and protocols
- Same WiFi setup flow (captive portal)
- Same OTA update mechanism (GitHub releases)

The web UI should work identically without ANY template/JS/CSS changes.

---

## Dependencies (ESP-IDF Components)

| Component | Purpose |
|-----------|---------|
| `esp_wifi` | WiFi STA/AP |
| `esp_netif` | Network interface |
| `lwip` | TCP/IP stack |
| `mdns` | mDNS hostname |
| `esp_http_client` | OTA downloads |
| `esp_ota_ops` | Firmware OTA |
| `spiffs` | Filesystem for www/templates/data |
| `nvs_flash` | Non-volatile storage init |
| `driver` (uart) | YX5200 communication |
| `driver` (spi_master) | MAX7219 communication |
| `driver` (rmt) | NeoPixel timing |
| `driver` (gpio) | Button, onboard LED |
| `freertos` | Tasks, mutexes, queues, timers |
| `cJSON` | JSON parsing/generation |
| `esp_timer` | High-resolution timing |
| `esp_psram` | PSRAM initialization |

---

## Estimated Scale

- ~60 HTTP routes across ~50 view handlers
- ~8 pattern functions + ~6 filter functions
- ~15 source files in `components/lighting/`
- ~10 source files in `components/webserver/`
- ~6 source files in `components/audio/`
- Total estimated: ~12,000–18,000 lines of C code
