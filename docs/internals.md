# Lighting System Internals

This document provides detailed technical specifications for patterns and filters. For user-facing documentation, see the [README](../README.md).

## Configuration Format

Lighting is defined by **scenes**, each containing one or more **jobs**. Each job assigns a **pattern** to a set of target LEDs, and optionally a list of **filters** that post-process the result.

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

## Models

The system supports multiple named Models. Each Model is a top-level container that holds a complete lighting configuration — for example `scenes`, `effects`, `filters`, `named_ranges`, `custom_colors`, and (optionally) a per-model `sounds` mapping. The persistent storage layout used by the device is::

    "lighting_settings": {
        "models": {
            "ModelName": { ... },
            "OtherModel": { ... }
        },
        "current_model": "ModelName"
    }

On first load the runtime will automatically migrate older single-model installations (where `scenes`, `effects`, etc. lived directly under `lighting_settings`) into a single model named "Model". There's also a helper API available on the `Lighting` singleton: `wrap_current_settings_into_model("Model")` which explicitly performs this wrapping and will raise an error if the settings already use the models container.

At runtime the `Lighting` instance binds `self.settings` to the active model dictionary and all UI and runtime operations (scene start/stop, named-range edits, effect updates, etc.) operate within that active model. When a model contains a `sounds` mapping it is preferred for playback; otherwise the top-level `sounds` mapping is used.

## OTA Updates

Code updates are managed from Setup -> Updates. The updater checks GitHub for changed files, lists additions/modifications/deletions, then applies updates only when explicitly confirmed.

The repository source is configurable in persistent storage under:

```python
"system_settings": {
    "ota": {
        "repo_owner": "PeterDarley",
        "repo_name": "lightmotron"
    }
}
```

### Tracked Files

By default, the following paths are tracked for updates:
- Directories: `lib/`, `web/`, `templates/`, `www/`, `docs/`
- Root files: `boot.py`, `main.py`, `settings.py`, `README.md`, `index.html`, `requirements.txt`

The following paths are explicitly excluded:
- `.git/`, `.github/`, `copilot_working/`, `deployment/`, `external_resources/`
- Upload scripts: `upload.ps1`, `repl.ps1`, `upload.sh`

### Submodule Support

Submodule file tracking is **enabled by default**. The OTA updater uses incremental chunked processing to safely handle large repository trees:

**How it works:**
- Remote tree and local files are saved to newline-delimited snapshot files
- Snapshot files are processed incrementally line-by-line
- Main repository trees are walked iteratively (non-recursive) one tree level at a time
- Submodule trees are walked iteratively (non-recursive) by fetching one tree level at a time
- Each file's SHA is compared individually, then discarded
- Temporary files are cleaned up after processing completes

This approach is **stack-safer** on ESP32 because it avoids recursive operations in hot paths and maintains a smaller working set during processing.

**To disable** submodule tracking (if you want to update submodules manually via git), add to persistent storage:
```python
"system_settings": {
    "ota": {
        "track_submodules": False
    }
}
```

### OTA Debug Logging

To capture progress checkpoints during update checks, enable:

```python
"system_settings": {
    "ota": {
        "debug_logging": True
    }
}
```

When enabled, the OTA engine appends stage markers and `gc.mem_free()`/`gc.mem_alloc()` snapshots to `.ota_debug.log`. This helps identify the last successful phase before a crash or reboot.


## Target Specification

| Value | Meaning |
|---|---|
| `0` | Single LED index |
| `[0, 2, 5]` | Explicit list of indices |
| `"0-7"` | Inclusive range |
| `"all"` | All LEDs |
| `"named:range_name"` | Look up range in `named_ranges` (may reference other named ranges)

Named ranges may include references to other named ranges using the `named:OtherRange`
syntax in member lists (for example: `"engine": [0, "named:wing"]`). The lighting
runtime expands these references recursively when resolving targets. The setup UI
validates named-range edits and will reject circular references to prevent infinite
recursion at runtime.

## Colors

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
Alternates between two colors symmetrically. The on-time and off-time are the same.

| Parameter | Description |
|---|---|
| `duration` | Half-period in ticks. The effect shows `colors[0]` for this many ticks, then `colors[1]` for the same number of ticks. |
| `frequency` | Optional override in blinks per second. If present, it takes precedence over `duration`. |
| `colors[0]` | On color |
| `colors[1]` | Off color |

---

### `pulse`
Like `blink` but with separate on-time and full-cycle timing, allowing asymmetric pulses.

| Parameter | Description |
|---|---|
| `duration` | Number of ticks the on color is shown |
| `period` | Full cycle length in ticks (`duration` + off-time) |
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
| `duration` | Full cycle length in ticks |
| `colors[0]` | Dim/off color |
| `colors[1]` | Bright/on color |

---

### `wave`
A comet or comets sweep across the LEDs. Each peak is set to `colors[1]` for one tick, then fades back to `colors[0]` over `width` LEDs of travel.

| Parameter | Default | Description |
|---|---|---|
| `duration` | 40 | Full sweep length in ticks |
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
| `duration` | 40 | One-way sweep length in ticks |
| `width` | 5 | Fade trail length in LEDs |
| `colors[0]` | — | Background/trail-end color |
| `colors[1]` | — | Peak color |

---

### `phaser_strip`
Two waves start at opposite ends of the target range and converge on a randomly chosen meeting point, both arriving at the same tick. After meeting, the meeting point holds lit while trails fade, then resets.

| Parameter | Default | Description |
|---|---|---|
| `duration` | 40 | Total ticks for the wave convergence phase |
| `width` | 5 | Fade trail length in LEDs |
| `colors[0]` | — | Background/trail-end color |
| `colors[1]` | — | Peak/meeting color |

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

---

### `brightness`
Multiplies each target RGB channel by a constant factor. Each resulting channel value is clamped to the inclusive integer range `0..255`.

| Parameter | Default | Description |
|---|---|---|
| `brightness` | 1.0 | Constant multiplier per channel (`0.5` = half brightness, `2.0` = double brightness) |
