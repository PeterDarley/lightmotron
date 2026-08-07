# Hardware & Wiring Reference

This page covers wiring for the audio hardware. For NeoPixel LED strip
wiring, see the [NeoPixel Wiring Diagram](neopixel-wiring.md).

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

## MAX7219 Scrolling LED Matrix (Billboard) — not currently supported

An earlier hardware configuration included a MAX7219-based scrolling LED
matrix display. The driver code is still in the tree
(`c_project/components/billboard/`) for reference, but it's **not wired
up or activated** in the current firmware — `main/boot.c` deliberately
never calls into it, and no default settings are seeded for it. Don't wire
one up expecting it to work.

If you're reviving this feature, the driver's default pin assumptions were
DIN→GPIO23 (SPI MOSI), CLK→GPIO18 (SPI SCK), CS→GPIO5, running at 5V logic
(needs a level shifter on the signal lines from the ESP32-S3's 3.3V GPIOs).
Note that GPIO5 in particular is a common choice for other things (an
audio module's UART RX, for instance) — check for conflicts before reusing
that default.
