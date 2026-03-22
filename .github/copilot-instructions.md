# Copilot Instructions

This file is the single source-of-truth for notes the user asks Copilot to "note".

Guideline:
- When the user says "note ..." or asks you to "note something", add it in this file.

## Notes

### ESP32 Board Pinout
The file `/docs/esp32_pinout.png` shows the pinout for the ESP32 dev board in use (ESP32-WROOM-32).

### MAX7219 4-module LED matrix display wiring
Uses **SPI** (not I2C). Pin mapping (set in `settings.BILLBOARD`):

| MAX7219 pin | ESP32 GPIO | Wire function |
|---|---|---|
| DIN  | GPIO 23 | SPI MOSI (data in) |
| CLK  | GPIO 18 | SPI SCK (clock) |
| CS   | GPIO 5  | Chip select |
| VCC  | 5 V     | Power (must be 5 V, not 3.3 V) |
| GND  | GND     | Common ground |

### Docstrings
All classes, functions, and methods should include a descriptive docstring. Docstrings MUST be followed by a single blank line.

### Variable names
Prefer long, descriptive variable names over short or abbreviated ones (e.g. `route_handler` not `rh`, `templates_dir` not `tdir`, `client_socket` not `cl`).

### Blank lines after indented blocks
Indented blocks (if/for/while/try/with/class/def bodies) should generally be followed by a blank line for readability.

