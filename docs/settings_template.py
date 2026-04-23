"""Reference for the persistent storage JSON structure (storage.json).

All device configuration is stored in a single JSON file (storage.json) on
the ESP32 filesystem, managed by PersistentDict (lib/storage.py).

The top-level keys are:

    system_settings   -- device / network / hardware configuration (see below)
    lighting_settings -- scenes, effects, filters, named ranges, custom colors
    ui_settings       -- (legacy key, migrated to system_settings)

Below is the full reference structure for ``system_settings``.  All keys are
optional; hardcoded defaults in settings.py are used when a key is absent.

{
    "system_settings": {

        # WiFi station credentials.
        # Collected on first boot via the captive portal (lightmotron-setup AP).
        # Editable on the Setup page → Initial Settings.
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

        # NeoPixel LED strip.
        "neopixels": {
            "pin":              4,      # GPIO pin connected to DIN
            "num":            144,      # total number of pixels
            "brightness_curve": true    # quarter-sine brightness adjustment
        }
    }
}

Notes
-----
* WiFi credentials are excluded from backup downloads and redacted on the
  Storage page.  They can only be changed via the Initial Settings card on
  the Setup page or by re-running the captive portal (lightmotron-setup AP).
* Changes to hardware pins, hostname, or WiFi credentials take effect on
  the next reboot.
* The captive portal is started automatically on first boot (no credentials
  stored) or if the configured WiFi network cannot be reached.
"""
