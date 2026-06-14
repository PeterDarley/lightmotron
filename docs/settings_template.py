"""Reference for the persistent storage JSON structure (storage.json).

All device configuration is stored in a single JSON file (storage.json) on
the ESP32 filesystem, managed by PersistentDict (lib/storage.py).

The top-level keys are:

    system_settings   -- device / network / hardware configuration (see below)
    lighting_settings -- lighting configuration. New installations store multiple
    "Models" under this key; the structure is::

        "lighting_settings": {
            "models": {
                "ModelName": {
                    "default_scene": ...,
                    "scenes": { ... },
                    "named_ranges": { ... },
                    "effects": { ... },
                    "filters": { ... },
                    "custom_colors": { ... },
                    "scene_settings": { ... }
                },
                "OtherModel": { ... }
            },
            "current_model": "ModelName"
        }

    Older single-model installations stored the scenes/effects/etc directly
    at the top-level under ``lighting_settings``; on first load the system will
    migrate that legacy layout into a single model named "Model".
    sounds            -- sound titles and MP3 file mappings for audio playback
    ui_settings       -- (legacy key, migrated to system_settings)

Below is the full reference structure for ``system_settings``.  All keys are
optional; hardcoded defaults in settings.py are used when a key is absent.

{
    "system_settings": {

        # WiFi station credentials.
        # Collected on first boot via the captive portal (lightmotron-setup AP).
        # Editable on the Setup page -> System Settings.
        "wifi": {
            "ssid": "my_network",
            "password": "my_password",
            "blink_on_connect": true,   # blink onboard LED when connected
            "print_on_connect": true    # print IP address to serial console
        },

        # mDNS hostname.  Device is accessible at <hostname>.local.
        # Default: "lightmotron"
        "hostname": "lightmotron",

        # Active CSS theme filename (e.g. "dark_red.css").
        # Empty string = default Bootstrap styling.
        "theme": "",

        # OTA update source repository.
        # Used by Setup -> Updates when checking/applying code updates.
        "ota": {
            "repo_owner": "PeterDarley",
            "repo_name": "lightmotron",
            "track_submodules": True,
            "debug_logging": False
        },

        # ESP32 board configuration.
        "board": {
            "cpu_frequency": 240000000  # Hz: 80000000 | 160000000 | 240000000
        },

        # GPIO pin assignments for built-in peripherals.
        "pins": {
            "led":    2,    # onboard status LED
            "button": 0,    # BOOT button
            "scl":    22,   # I2C clock
            "sda":    21    # I2C data
        },

        # MAX7219 4-module LED matrix display (SPI).
        "billboard": {
            "mosi":       23,   # DIN  -- SPI MOSI
            "sck":        18,   # CLK  -- SPI SCK
            "cs":          5,   # CS   -- chip select
            "num":         4,   # number of chained MAX7219 modules
            "brightness":  5    # 0-15
        },

        # NeoPixel LED strip configuration (one or more strips).
        # Each entry supports independent pin, pixel count, color order,
        # and brightness-curve setting.
        "neopixels": [
            {
                "pin":              4,      # GPIO pin connected to DIN
                "num":            144,      # total number of pixels on this strip
                "color_order":   "GRB",    # RGB byte order expected by strip hardware
                "brightness_curve": true     # quarter-sine brightness adjustment
            }
        ],

        # Audio player configuration (YX5200/DFPlayer MP3 modules).
        # Up to 3 modules can be configured on separate UARTs.
        # Each module connects to its own RX/TX pin pair.
        "audio_players": [
            {
                "uart":         1,          # UART number (0, 1, or 2)
                "tx_pin":      10,          # GPIO pin connected to RX of YX5200
                "rx_pin":      11,          # GPIO pin connected to TX of YX5200
                "high_quality": false       # whether to prefer this module for HQ sounds
            }
            # ... up to 3 total entries
        ],

        # If true, send DFPlayer/YX5200 soft-reset over UART at boot.
        # Helps recover modules that occasionally fail to initialise after ESP32 reboot.
        "audio_reset_on_boot": true,

        # If true, print detailed audio state diagnostics to serial output.
        # Useful when investigating early stop detection or module state drift.
        "audio_debug_logging": false,

        # Stored IP address for IP announcement detection.
        # When the device boots and this IP differs from the current assigned IP,
        # the system will announce the new IP address via audio (if audio players
        # are configured).  This prevents repeated announcements of the same IP.
        # Can be cleared via "Mark Current IP as Announced" button in System Settings.
        "stored_ip_address": ""
    },

    # Sounds configuration.
    # Mapping of sound titles to MP3 file numbers and playback metadata.
    "sounds": {
        "alert": {
            "file":          1,      # file number (0001.mp3, 0002.mp3, etc)
            "high_quality": false,   # whether to play on high-quality modules if available
            "show_on_home": true,    # whether to show this sound button on the Home page
            "loop": false,           # whether to loop/repeat when playback ends
            "stops": [],             # list of sound titles to stop when this starts
            "next_sound": None       # sound title to play when this ends (chaining)
        },
        "fanfare": {
            "file":          2,
            "high_quality": true,
            "show_on_home": true,
            "loop": false,
            "stops": [],
            "next_sound": None
        }
        # ... more sounds
    }
}

Notes
-----
* WiFi credentials are excluded from backup downloads and redacted on the
  Storage page.  They can only be changed via the System Settings card on
  the Setup page or by re-running the captive portal (lightmotron-setup AP).
* Changes to hardware pins, hostname, or WiFi credentials take effect on
  the next reboot.
* The captive portal is started automatically on first boot (no credentials
  stored) or if the configured WiFi network cannot be reached.
* Audio players must have SD cards formatted with /MP3/ folders containing
  numbered MP3 files (0001.mp3, 0002.mp3, etc).  All cards must have identical
  file structures.
* When playing a sound with high_quality=true, the system will prefer to play
  it on a module marked as high_quality if one is available.
* OTA updates can target a different GitHub repository by changing
    ``system_settings.ota.repo_owner`` and ``system_settings.ota.repo_name``
    from the Setup page Updates card.
* OTA update diagnostics can be enabled by setting
    ``system_settings.ota.debug_logging`` to ``true``. Diagnostic entries are
    written to ``.ota_debug.log`` during update checks.
* Named ranges (``named_ranges``) may include references to other named ranges
    using the ``"named:OtherRange"`` syntax in addition to explicit LED indices
    or ranges. The web setup UI and runtime will expand these references when
    resolving targets. Circular references are rejected by the UI and will not
    be saved.
* Scene-level metadata lives under
    ``lighting_settings.models.<model_name>.scene_settings.<scene_name>`` and may
    include:
    ``kills`` (scenes to remove on start), ``sound`` (sound to play on start),
    ``stop_sounds_on_start`` (sounds to stop before activation), and
    ``stop_sounds_on_end`` (sounds to stop when the scene is removed/replaced).
* Soundscapes live under
    ``lighting_settings.models.<model_name>.soundscapes.<soundscape_name>`` as a
    mapping of entry names to entry objects, for example::

        "soundscapes": {
            "Tripple Moo": {
                "entry1": {
                    "sound": "Moo",
                    "repeat_enabled": true,
                    "repeat": 0
                }
            }
        }

  Repeat semantics are: ``repeat_enabled=false`` means no repeat,
  ``repeat_enabled=true`` with ``repeat=0`` means infinite repeat, and
  ``repeat_enabled=true`` with positive ``repeat`` means repeat that many
  additional times.
