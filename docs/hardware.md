# Hardware & Wiring Reference

This page covers wiring for the audio and matrix-display hardware. For
NeoPixel LED strip wiring, see the [NeoPixel Wiring Diagram](neopixel-wiring.md).

## Microcontroller

YD-ESP32-S3 board (ESP32-S3-WROOM-1 N16R8 module — 16MB flash, 8MB octal
PSRAM).

## Audio

* Up to 3× WWZMDiB YX5200 MP3 player modules (DFPlayer-compatible, serial
  UART — one hardware UART per module; the ESP32-S3 has 3, enough for 3
  simultaneous players)
* 1× PAM8403 stereo amplifier module (analog volume via its onboard
  potentiometer; software volume is controlled separately via YX5200
  serial commands)
* 1–3× 3W 8Ω mini speakers

Wiring notes:

* Each YX5200's UART RX line needs a 1kΩ resistor between it and the
  ESP32-S3's TX pin.
* To mix multiple YX5200 modules' DAC output into the single PAM8403 input,
  use a passive resistor network: one 1kΩ resistor per player, each from
  that player's `DAC_R` output into the shared PAM8403 input line.
* UART pin assignments (TX/RX per module) are configurable per-module on
  the Setup page → System Settings; there's no fixed default, since it
  depends on how many modules you're wiring and which UARTs you assign
  them to.

## MAX7219 Scrolling LED Matrix (Billboard)

Uses SPI (not I2C).

| MAX7219 pin | ESP32 GPIO | Wire function |
|---|---|---|
| DIN | GPIO 23 | SPI MOSI (data in) |
| CLK | GPIO 18 | SPI SCK (clock) |
| CS | GPIO 5 | Chip select |
| VCC | 5V | Power (must be 5V, not 3.3V) |
| GND | GND | Common ground |

Since the MAX7219 module runs its SPI lines at 5V logic and the ESP32-S3's
GPIOs are 3.3V, a logic level converter is needed on CLK, DIN, and CS (a
4-channel 3.3V↔5V converter module works well here). VCC and GND connect
directly — only the signal lines need level shifting.

Pin numbers and module count are configurable on the Setup page → System
Settings; the table above reflects a typical wiring, not a hard-coded
requirement.
