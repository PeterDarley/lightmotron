# Lighting System Internals

This document provides detailed technical specifications for patterns and filters. For user-facing documentation, see the [README](../README.md).

## Web UI View Modules

Web request handlers are being split by logical feature area rather than by file length. For example, named-range handlers now live in `web/views_named_ranges.py`, while `web/views.py` remains the compatibility export module used by route registration.

Static assets served by the built-in web server use content-type-based cache policy. JavaScript and CSS responses are sent with `Cache-Control: no-cache, max-age=0, must-revalidate` so browser UI updates are picked up immediately after deployment, while non-code assets keep a longer cache lifetime.
Static response writes use a send-all loop so larger CSS/JS payloads are not truncated by partial socket writes.
The named-range LED picker template also appends a version query parameter to `led_picker.js` based on file mtime, ensuring the script URL changes when that file changes.
Web server startup now handles transient socket creation `OSError 23` (ENFILE) by retrying briefly and then exiting cleanly (without uncaught thread traceback) if descriptors are still exhausted after retries.
Client disconnect resets (`OSError 104` / `ECONNRESET`) during request reads are treated as normal disconnects and no longer logged as 500 server errors.
Boot now prioritizes `/lib` on `sys.path` before runtime imports, reducing accidental shadowing by stale root-level modules (for example `/audio.py` overshadowing `/lib/audio.py`).
Audio startup can send a UART soft-reset command (DFPlayer/YX5200 command `0x0C`) to each configured module during boot to recover players that occasionally fail after ESP32 reset. This is controlled by `system_settings.audio_reset_on_boot` (default true).
When diagnosing sound stop/play state issues, enable `system_settings.audio_debug_logging` to print UART command/status traces, explicit `/sounds/status` endpoint hit/render traces, and mapped playing-state summaries to serial logs.
Additionally, sound-status diagnostics now emit baseline serial traces (`sounds-status: ...` and `audio: status ...`) even when `audio_debug_logging` is off, so endpoint-hit and low-level status checks remain visible during troubleshooting.
Home scene actions (`/set_scene`) return a server-rendered `scenes/scene_panel.html` fragment and swap `#scene_panel` so active-scene labels and ongoing/immediate button states always reflect server truth, including scene kills triggered by `scene_settings.kills`.
Scene trigger sound playback (`scene_settings.sound`) is handled by the lighting runtime (`Lighting.set_scene` / `Lighting.add_scene`), not by web views, so sounds also play for non-UI scene activations, including the default scene selected at boot via `set_scene(None)`.
Scene-level sound stop lists are also runtime-driven: `scene_settings.stop_sounds_on_start` is applied before scene activation, and `scene_settings.stop_sounds_on_end` is applied when a scene is removed or replaced.
Home sound controls only include sounds where `show_on_home` is true and use adaptive HTMX polling against `/sounds/status`: `every 5s` while any sound is currently playing, and `every 30s` when all sounds are idle.
The Home "Active Scenes" label/list intentionally shows only ongoing scenes; immediate scenes are trigger actions and are not displayed there.

Sound playback supports three advanced features configured per-sound:
- **Looping**: When `loop: true`, a sound automatically restarts when playback ends, repeating indefinitely until stopped manually.
- **Stopping other sounds**: The `stops: [...]` list specifies which other sounds are stopped when this sound starts. Useful for enforcing mutually exclusive playback (e.g., muting background music when an alert plays).
- **Chaining**: When `next_sound: "title"` is set, the specified sound automatically starts when the current sound ends. Enables sequences like intro→main→outro without UI intervention.

Sound looping and chaining are detected by the continuous audio polling system (`_poll_tick()` every 100ms). When a module transitions from playing→stopped, `SoundManager.check_for_ended_sounds()` detects the transition and takes the configured action (restart if loop, or start next_sound if chaining).

## Lighting Module Layout

The lighting runtime is split into focused modules under `lib/lighting/` to
reduce update payloads and improve maintainability:

- `lighting.py`: model/settings orchestration, scene lifecycle, target/color helpers
- `patterns.py`: pattern implementations (`pattern_*`)
- `effects.py`: effect resolution and per-tick execution pipeline
- `filters.py`: filter implementations (`filter_*`) and filter grouping helpers
- `metadata.py`: `PATTERN_METADATA` and `FILTER_METADATA`
The storage page copy button uses the Clipboard API when available and falls back to `document.execCommand('copy')` for non-secure/device-browser contexts.

## Configuration Format

Lighting is defined by **scenes**, each containing one or more **jobs**. Each job assigns a **pattern** to a set of target LEDs, and optionally a list of **filters** that post-process the result.

When a scene entry is renamed in the Setup UI, other entries in the same
scene that reference it via `after` are automatically updated to the new
entry name.
When a scene entry is deleted in the Setup UI, any same-scene `after`
references pointing to that entry are automatically cleared.

Filters are applied sequentially: each filter receives the output of the
previous filter. On scene restart (`set_scene`), transient filter runtime
state is cleared so timing-based filters (for example `dropout`/`spike`)
start from a clean phase.
Timing-based filters are evaluated using effect-local ticks (relative to each
effect's own start tick), not the global animation tick.

Scene auto-completion only applies when every effect in the scene has an
explicit `cycles` limit and all such effects have finished. If a scene
contains any effect without `cycles` (infinite repeat), that scene is treated
as ongoing and is not auto-removed.
The home page scene grouping uses this same rule: scenes with at least one
infinite effect are shown as ongoing.

## Audio Debug Logging

Audio module logs include the configured UART and pin mapping so wiring can be
verified directly from boot/runtime output.

- `AudioPlayer: configured modules:` entries include `uart`, `tx_pin`, and `rx_pin`.
- Health checks include `uart`, `tx`, and `rx` in each module line.
- YX5200 command logs include `UART N (tx=X, rx=Y)` on send, response, no-response,
  and write/read error messages.

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

For hash comparison consistency on Windows checkouts, local text files are normalized from CRLF to LF before computing git-style blob SHAs. This includes common git dotfiles such as `.gitignore`.

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
- Directories: `lib/`, `web/`, `templates/`, `www/`
- Root files: `boot.py`, `main.py`

The following paths are explicitly excluded:
- `.git/`, `.github/`, `copilot_working/`, `deployment/`, `external_resources/`
- Upload scripts: `upload.ps1`, `repl.ps1`, `upload.sh`
- Non-device files: `index.html`, `requirements.txt`, `lib/licence`

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

In the effect editor UI, the displayed filter list is intentionally reversed relative to execution order: the top filter runs last.

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
| `variation_percent` | 20 | Maximum random channel deviation as a percentage of each channel's current target value |
| `heat` | 10 | Reserved for compatibility (not currently used by sizzle/scintillate math) |

---

### `scintillate`
Like `sizzle` but each LED is adjusted independently, creating a sparkling/twinkling effect where individual LEDs vary in different directions.

| Parameter | Default | Description |
|---|---|---|
| `frequency` | 40 | Updates per second |
| `variation_percent` | 20 | Maximum random channel deviation as a percentage of each channel's current target value |
| `heat` | 10 | Reserved for compatibility (not currently used by sizzle/scintillate math) |

When editing legacy `sizzle` or `scintillate` filters that still store
`variation` as 0..255 levels, the UI converts it to percentage using
`variation / 255 * 100`. Saving stores only `variation_percent`.

---

### `brightness`
Multiplies each target RGB channel by a constant factor. Each resulting channel value is clamped to the inclusive integer range `0..255`.

| Parameter | Default | Description |
|---|---|---|
| `brightness` | 1.0 | Constant multiplier per channel (`0.5` = half brightness, `2.0` = double brightness) |

---

### `spike`
Periodically overrides LEDs with a spike color.

Each spike lasts `duration` ± `heat` ticks. After a spike ends, the next spike
is scheduled `period` ± `variation` ticks later. This keeps spikes separated by
the configured period window instead of clustering when spikes are long.
The first spike after an effect starts is scheduled from effect start using
`period` (plus any subrange phase offset), without initial `variation` jitter.

In the Setup UI filter editor, `duration` and `period` are entered with
minutes/seconds/ticks controls and stored as total ticks.

| Parameter | Default | Description |
|---|---|---|
| `color` | `white` | Spike color (name or RGB tuple) |
| `duration` | 5 | Spike length in ticks |
| `period` | 40 | Delay before next spike (after previous spike ends) |
| `variation` | 0 | Random ± tick offset applied to each period |
| `heat` | 0 | Random ± tick offset applied to each spike duration |
| `scope` | `all` | Grouping mode: `all`, `subranges`, or `leds` |

---

### `dropout`
Identical to `spike` but the override color is always black `(0, 0, 0)`.

For `scope='subranges'`, if the target is an aggregate named range (a named
range whose value is a list of component targets), each component is treated as
its own group. This avoids accidental merging of adjacent component LEDs.
Like `spike`, the first dropout after effect start uses `period` from the
effect start time and does not apply initial `variation` jitter.
Runtime scheduling state for `spike`/`dropout` is scoped per effect instance,
so reusing the same named filter in different effects does not carry timing
state across those effect boundaries.

| Parameter | Default | Description |
|---|---|---|
| `duration` | 5 | Dropout length in ticks |
| `period` | 40 | Delay before next dropout (after previous dropout ends) |
| `variation` | 0 | Random ± tick offset applied to each period |
| `heat` | 0 | Random ± tick offset applied to each dropout duration |
| `scope` | `all` | Grouping mode: `all`, `subranges`, or `leds` |
