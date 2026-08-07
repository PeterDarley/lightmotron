# Lighting System Internals

Implementation reference for the `c_project` firmware (ESP-IDF/C on
ESP32-S3). This documents *how the system works*, not how to configure it —
for that, see the [Programming Guide](programming.md). For a repo-wide
architecture overview and links to the rest of the developer docs, start at
the [Developer Guide](developer.md).

## Web/HTTP Layer

Request handling is split by feature area, one C file per area, under
`c_project/components/web/`: `views_home.c` (home page, scene panel,
animation), `views_models.c`, `views_scenes.c`, `views_effects.c`,
`views_filters.c`, `views_sounds.c`, `views_soundscapes.c`,
`views_storage.c` (storage viewer, backup/restore), `views_system.c`
(setup, status, theme, hostname, system settings, reboot, OTA, custom
colors), and `views_named_ranges.c`. Route registration lives in
`components/web/routes.c`; global template context (theme, hostname) is
injected by `context_processors.c`.

The HTTP server itself (`components/webserver/`) is a custom
socket-based server with its own minimal template engine
(`template_engine.c`) — not a third-party framework. It supports variable
interpolation (`{{ x }}`, `{{ x.key }}`), `{% for %}`/`{% if %}` blocks, and
`{% include %}`.

All web assets (`templates/`, `www/`) are read into RAM once at boot
(`asset_cache.c`) rather than read from flash per-request. This isn't just
a performance optimization — a per-request flash read briefly disables the
flash cache on both CPU cores, and doing that on every page load previously
caused hard-to-diagnose UART interrupt-watchdog crashes when it collided
with the audio driver's own interrupts (see `AUDIO_UART_CRASH_NOTES.md` for
the full investigation).

## Storage Subsystem

All persistent configuration lives in JSON files on a LittleFS partition
(`components/storage/persistent_dict.c`), mounted separately from the
`webassets` partition specifically so that flashing new firmware/web
assets never touches user settings. Key behaviors:

* **Lazy loading** — a file's contents are only read from flash on first
  access, not at `persistent_dict_open()` time.
* **In-memory caching with an explicit dirty flag** — `persistent_dict_get()`
  returns a live pointer into the in-memory cJSON tree, not a copy.
  Mutating that tree directly (rather than going through
  `persistent_dict_set()`/`persistent_dict_delete_key()`) does **not**
  mark the store dirty, so `persistent_dict_save()` silently no-ops and
  the change is lost on reboot. Code that needs to mutate a live-referenced
  tree in place must call `persistent_dict_mark_dirty()` before saving.
* **Atomicity** — writes go straight to the target file (no
  temp-file-then-rename dance); LittleFS itself guarantees a file's
  contents are committed atomically on close/sync, so a power loss
  mid-write reverts to the previous committed state rather than leaving a
  torn file.
* **Thread safety** — each `persistent_dict_t` has its own FreeRTOS mutex.

## Lighting Subsystem

`components/lighting/` implements the animation runtime:

* `lighting.c` — scene lifecycle (activate/remove), model/settings binding
* `animation.c` — the tick loop (FreeRTOS task, 25ms interval)
* `patterns.c` — pattern implementations (`pattern_*`)
* `effects.c` — effect resolution and per-tick execution
* `filters.c` — filter implementations (`filter_*`)
* `colors.c` — named/hex/RGB color resolution
* `named_ranges.c` — named-range target resolution (including composite
  ranges that reference other named ranges)
* `metadata.c` — scene metadata (kills, trigger sound, stop lists)

### Configuration Format

Lighting is defined by **scenes**, each containing one or more **jobs**.
Each job assigns a **pattern** to a set of target LEDs, plus an optional
list of **filters** that post-process the pattern's output:

```json
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

For the full set of pattern/filter names and their parameters, see the
[Programming Guide](programming.md) — the reference there is the same one
users see in the Setup UI.

Filters are applied sequentially, each receiving the previous filter's
output. Every filter receives both the pattern's **target color** for this
tick and the LED's **current color** from the previous tick; differences
are calculated against the target but applied to the current color. This
makes filter order not affect the final result — chaining `[a, b]` produces
the same output as `[b, a]`.

Scene auto-completion applies only when every effect in a scene has an
explicit `cycles` limit and all of them have finished; a scene with any
infinite-repeat effect is treated as ongoing and never auto-removed. The
Home page's "ongoing vs. immediate" grouping uses this same rule.

### Models

The system supports multiple named **Models** — each a complete,
self-contained lighting configuration. Storage layout:

```json
"lighting_settings": {
    "models": {
        "ModelName": { "scenes": {}, "effects": {}, "filters": {}, "named_ranges": {}, "custom_colors": {} },
        "OtherModel": { ... }
    },
    "current_model": "ModelName"
}
```

At runtime, the active model's dictionary is what every scene/effect/
named-range operation reads and writes. When a model defines its own
`sounds` mapping it's preferred for playback; otherwise the top-level
`sounds` mapping is used.

## Audio Subsystem

`components/audio/` has three layers: `yx5200.c` (per-module UART driver,
DFPlayer-compatible command/response protocol), `audio_player.c`
(multi-module selection: picks a free/high-quality module for a play
request), and `sound_manager.c` (loop/chain/stop-list logic, driven by
periodic status polling rather than hardware push notifications).

A module's responsiveness is tracked defensively, since these modules are
known to occasionally miss a status-query response without actually being
gone: a module is marked unresponsive only after several consecutive
inconclusive reads, and — separately — a *sticky* "has this module ever
answered" flag means a module that's proven itself real is periodically
re-probed rather than being written off permanently the first time it goes
quiet. See `yx5200_recover_if_due()`/`yx5200_could_recover()` and
`AUDIO_UART_CRASH_NOTES.md` for the reasoning and the failure mode this
was built to avoid (a real module getting mistaken for absent hardware and
never being tried again for the rest of the boot session).

## LED Driver

`components/leds/leds.c` drives NeoPixel strips via the RMT peripheral.
Each configured strip has its own color-order mapping (e.g. GRB vs RGB) —
the pattern/filter/effect pipeline above always works in a
hardware-independent logical RGB representation; reordering into the
physical channel order happens only at the final transmit step.

## Networking

* `wifi_manager.c` — station-mode connect/reconnect
* `captive_portal.c` — fallback AP + DNS hijack for first-time WiFi setup,
  including a live network rescan without tearing down the portal
* `mdns_setup.c` — `<hostname>.local` registration
* `ota_update.c` — firmware updates

### OTA Updates

This is one of the few subsystems that's **architecturally different**
from the original MicroPython implementation, not just a line-for-line
port. MicroPython ran interpreted `.py` source directly off the
filesystem, so its updater could sync individual changed files from a git
tree. A compiled ESP-IDF firmware image can't be patched file-by-file —
the only sound equivalent is flashing a whole new firmware binary through
ESP-IDF's OTA partition mechanism (`ota_0`/`ota_1`, see `partitions.csv`).

So the C build's updater instead: queries the configured GitHub
repository's **latest release** via the GitHub API, looks for a `.bin`
asset, compares its version against the running firmware's build version,
and — when the user confirms via Setup → Updates — downloads and flashes
it via `esp_https_ota()`. `build_c_release.ps1` (see the
[Building Guide](building.md)) is what produces the `.bin` a release
should have attached.

The repository is configurable in persistent storage:

```json
"system_settings": {
    "ota": {
        "repo_owner": "PeterDarley",
        "repo_name": "lightmotron"
    }
}
```

## See Also

* [Programming Guide](programming.md) — user-facing configuration
  reference (patterns, filters, colors, scenes, sounds)
* [Building Guide](building.md) — build/flash instructions
* [Theming Guide](theming.md) — CSS theming system
* [Hardware Reference](hardware.md) — wiring
* [Storage Format Reference](settings_template.py) — full JSON schema
