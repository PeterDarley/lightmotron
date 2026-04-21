# Lightmotron

A MicroPython-based lighting controller for plastic model kits, running on an ESP32. Uses NeoPixel LED strips to produce dynamic lighting effects, and exposes a web interface for control over WiFi.

## Features

* NeoPixel LED strip control for model lighting effects
* MAX7219 4-module scrolling LED matrix display
* Web server for browser-based control
* WiFi connectivity

## Hardware

### Microcontroller
* ESP32-S3-WROOM-1 (N16R8 variant — 16MB flash, 8MB octal PSRAM)

### Lighting
* NeoPixel LED strip (GPIO 32)
* MAX7219 4-module scrolling LED matrix display (SPI: MOSI GPIO 23, SCK GPIO 18, CS GPIO 5)

### Audio
* Up to 3× WWZMDiB YX5200 MP3 player modules (DFPlayer-compatible, serial UART)
* 1× PAM8403 stereo amplifier module (analog volume via onboard pot)
* 1–3× 3W 8Ω mini speakers
* Passive mix network: one 1kΩ resistor per player on DAC_R → PAM8403 input
* 1kΩ resistor on each ESP32-S3 TX → YX5200 RX line

## Setup

This repo uses a git submodule for shared MicroPython libraries. After cloning, run:

```bash
git submodule init
git submodule update
```

## Uploading to the Device

```powershell
.\upload.ps1
```

To hard reset the device after uploading:

```powershell
python tools\reset_device.py COM3
```

## Lighting System

Lighting is defined by **scenes**, each containing one or more **jobs**. Each job assigns a **pattern** to a set of target LEDs, and optionally a list of **filters** that post-process the result.

### Scene Configuration

```python
"scenes": {
    "My Scene": {
        "job_name": {
            "pattern": "solid",
            "target": "0-5",
            "colors": ["red", "black"],
            "filters": [{"filter": "scintillate", "heat": 8}]
        }
    }
}
```

### Target Specification

| Value | Meaning |
|---|---|
| `0` | Single LED index |
| `[0, 2, 5]` | Explicit list of indices |
| `"0-7"` | Inclusive range |
| `"all"` | All LEDs |

### Colors

Colors can be named strings or RGB tuples (`(255, 128, 0)`). Most patterns take two colors: `colors[0]` is the primary/on color and `colors[1]` is the secondary/off color.

| Name | RGB | Description |
|---|---|---|
| `"white"` | (255, 255, 255) | Full-brightness white |
| `"warm_white"` | (255, 220, 160) | Warm incandescent white |
| `"cool_white"` | (180, 210, 255) | Cool daylight white |
| `"dim_white"` | (64, 64, 64) | Low-level ambient white |
| `"silver"` | (180, 180, 200) | Slightly cool silver-grey |
| `"grey"` | (128, 128, 128) | Mid grey |
| `"black"` | (0, 0, 0) | Off |
| `"red"` | (255, 0, 0) | Pure red |
| `"dark_red"` | (128, 0, 0) | Deep red |
| `"orange"` | (255, 100, 0) | Orange |
| `"amber"` | (255, 160, 0) | Amber / warm orange |
| `"gold"` | (255, 200, 0) | Bright gold |
| `"yellow"` | (255, 255, 0) | Yellow |
| `"green"` | (0, 255, 0) | Pure green |
| `"dark_green"` | (0, 128, 0) | Deep green |
| `"lime"` | (128, 255, 0) | Yellow-green |
| `"teal"` | (0, 180, 128) | Blue-green teal |
| `"cyan"` | (0, 255, 255) | Cyan |
| `"ice_blue"` | (80, 160, 255) | Light icy blue |
| `"blue"` | (0, 0, 255) | Pure blue |
| `"dark_blue"` | (0, 0, 128) | Deep blue |
| `"indigo"` | (60, 0, 180) | Deep indigo |
| `"violet"` | (180, 0, 255) | Bright violet |
| `"purple"` | (128, 0, 128) | Mid purple |
| `"magenta"` | (255, 0, 255) | Magenta |
| `"pink"` | (255, 80, 150) | Hot pink |
| `"fire"` | (255, 40, 0) | Deep orange-red flame |
| `"plasma"` | (0, 200, 255) | Sci-fi plasma blue |
| `"engine_glow"` | (100, 40, 255) | Purple engine exhaust glow |

---

## Patterns

### `solid`
Sets all target LEDs to a fixed color. No animation.

| Parameter | Description |
|---|---|
| `colors[0]` | The color to display |

---

### `blink`
Alternates between two colors at a given frequency.

| Parameter | Description |
|---|---|
| `frequency` | Blinks per second (cycles at 40Hz tick rate) |
| `colors[0]` | On color |
| `colors[1]` | Off color |

---

### `pulse`
Like `blink` but with a separate on-duration and off-interval, allowing asymmetric pulses.

| Parameter | Description |
|---|---|
| `frequency` | Pulses per second |
| `duration` | Number of ticks the on color is shown |
| `colors[0]` | On color |
| `colors[1]` | Off color |

---

### `fade_in`
Linearly interpolates from `colors[0]` to `colors[1]` over a set duration, then holds.

| Parameter | Description |
|---|---|
| `duration` | Number of ticks for the full fade |
| `colors[0]` | Start color |
| `colors[1]` | End color |

---

### `breathe`
Uses a sine wave to smoothly oscillate between two colors, creating a breathing effect.

| Parameter | Description |
|---|---|
| `frequency` | Breath cycles per second |
| `colors[0]` | Dim/off color |
| `colors[1]` | Bright/on color |

---

### `sizzle`
Randomly walks each channel around a base color using biased random steps. Gives a fire/sizzle appearance.

| Parameter | Default | Description |
|---|---|---|
| `frequency` | 40 | Updates per second |
| `variation` | 50 | Controls how strongly distance from target biases the random walk — smaller values mean stronger correction |
| `heat` | 10 | Maximum channel step per update (1 to `heat`) |
| `colors[0]` | — | Target base color |

---

### `wave`
A comet or comets sweep across the LEDs. Each peak is set to `colors[1]` for one tick, then fades back to `colors[0]` over `width` LEDs of travel.

| Parameter | Default | Description |
|---|---|---|
| `frequency` | 1 | Sweeps per second |
| `width` | 5 | Fade trail length in LEDs |
| `number` | 1 | Number of simultaneous peaks, evenly spaced |
| `reverse` | `false` | If true, sweeps from last LED to first |
| `colors[0]` | — | Background/trail-end color |
| `colors[1]` | — | Peak color |

---

### `cylon`
Like `wave` but the peak bounces back and forth (forward sweep then reverse sweep).

| Parameter | Default | Description |
|---|---|---|
| `frequency` | 1 | One-way sweeps per second |
| `width` | 5 | Fade trail length in LEDs |
| `colors[0]` | — | Background/trail-end color |
| `colors[1]` | — | Peak color |

---

## Filters

Filters are applied after the pattern has computed its LED list for the tick. Multiple filters can be chained in the `filters` list.

```python
"filters": [
    {"filter": "scintillate", "frequency": 20, "heat": 5}
]
```

---

### `null`
Passes the LED list through unchanged. Useful for testing or as a placeholder.

---

### `sizzle`
Computes a single random deviation from the first LED's current position toward its target color, then applies that same deviation uniformly to all LEDs. Creates a coordinated group flicker.

| Parameter | Default | Description |
|---|---|---|
| `frequency` | 40 | Updates per second |
| `variation` | 50 | Bias strength toward target (lower = stronger pull) |
| `heat` | 10 | Maximum step size per channel per update |

---

### `scintillate`
Like `sizzle` but each LED is adjusted independently, creating a sparkling/twinkling effect where individual LEDs vary in different directions.

| Parameter | Default | Description |
|---|---|---|
| `frequency` | 40 | Updates per second |
| `variation` | 50 | Bias strength toward target per LED |
| `heat` | 10 | Maximum step size per channel per update |