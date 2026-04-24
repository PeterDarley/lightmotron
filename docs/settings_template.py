"""Reference for the persistent storage JSON structure (storage.json).

All device configuration is stored in a single JSON file (storage.json) on
the ESP32 filesystem, managed by PersistentDict (lib/storage.py).

The top-level keys are:

    system_settings   -- device / network / hardware configuration (see below)
    lighting_settings -- scenes, effects, filters, named ranges, custom colors
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
        ]
    },

    # Sounds configuration.
    # Mapping of sound titles to MP3 file numbers and metadata.
    "sounds": {
        "alert": {
            "file":          1,      # file number (0001.mp3, 0002.mp3, etc)
            "duration_ms":  5400,    # duration in milliseconds
            "high_quality": false    # whether to play on high-quality modules if available
        },
        "fanfare": {
            "file":          2,
            "duration_ms":  12000,
            "high_quality": true
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
