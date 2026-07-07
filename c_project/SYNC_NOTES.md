# C Project Sync Audit — Progress Notes

Started: 2026-07-06. Goal: bring `c_project/` (ESP-IDF C port) up to date with the
current state of the Python/MicroPython implementation, per explicit user request
(overriding the normal "c_project does not need to stay in sync" default noted in
`.github/copilot-instructions.md` for this one-time full audit).

This file is the restart point. If this session is interrupted, read this file first,
then resume at the first `[ ]` (not started) or `[~]` (in progress) item below.

## Methodology

For each file pair: read both sides, identify (a) features/fields/routes present in
Python but missing/stale in C, (b) behavior differences, (c) anything in C that's
obsolete (Python removed it). Fix the C side to match. Record findings here even for
pairs found already in sync, so we don't re-audit them.

## File pair mapping & status

Legend: `[ ]` not started · `[~]` in progress · `[x]` audited+fixed · `[=]` audited, already in sync

### Storage
- [x] `lib/storage.py` (157L) <-> `components/storage/persistent_dict.c/.h`, `json_helpers.c/.h`

### Boot / entry point / settings
- [ ] `boot.py` (163L), `main.py` (9L), `settings.py` (214L) <-> `main/boot.c/.h`, `main/main.c`, `main/settings.h`

### Networking
- [ ] `lib/comms.py` (413L, WIFIManager) <-> `components/network/wifi_manager.c/.h`
- [ ] `lib/captive_portal.py` (621L) <-> `components/network/captive_portal.c/.h`
- [ ] `lib/ip_announcement.py` (223L) <-> nothing found in C yet — check if in scope / needs a new file
- [ ] `lib/ota_update.py` (1101L) <-> `components/network/ota_update.c/.h` (C is only 64L — likely very stale/stub)
- [ ] mDNS piece of `boot.py` <-> `components/network/mdns_setup.c/.h`

### Webserver core
- [ ] `lib/webserver.py` (1439L) <-> `components/webserver/webserver.c/.h`, `template_engine.c/.h`, `request_parser.c/.h`, `static_files.c/.h`, `mime_types.c/.h`, `response.h` (no .c — check)

### LEDs
- [x] `lib/leds.py` (469L, incl. OnboardLED) <-> `components/leds/leds.c/.h`

### Audio
- [ ] `lib/audio.py` (623L) <-> `components/audio/audio_player.c/.h`, `yx5200.c/.h`
- [ ] `lib/sounds.py` (709L) <-> `components/audio/sound_manager.c/.h`

### Billboard
- [x] `lib/billboard.py` (214L) <-> `components/billboard/billboard.c/.h`
- [x] `lib/max7219.py` (89L) <-> `components/billboard/max7219.c/.h`

### Lighting core
- [ ] `lib/animation.py` (142L) <-> `components/lighting/animation.c/.h` — NOTE: just fixed a stuck-`running`-flag bug here on the Python side this session (thread spawn failure rollback); make sure C port gets equivalent treatment or already doesn't have the bug (C threading model differs).
- [ ] `lib/lighting/colors.py` (36L) <-> `components/lighting/colors.c/.h`
- [ ] `lib/lighting/effects.py` (292L) <-> `components/lighting/effects.c/.h`
- [ ] `lib/lighting/filters.py` (270L) <-> `components/lighting/filters.c/.h`
- [ ] `lib/lighting/lighting.py` (561L) <-> `components/lighting/lighting.c/.h`
- [ ] `lib/lighting/metadata.py` (79L) <-> `components/lighting/metadata.c/.h`
- [ ] `lib/lighting/patterns.py` (333L) <-> `components/lighting/patterns.c/.h`
- [ ] named ranges logic (part of `lib/lighting/lighting.py`?) <-> `components/lighting/named_ranges.c/.h`

### Web views
- [ ] `web/routes.py` (78L) <-> `components/web/routes.c/.h` — includes the two fresh routes added this session: `/filters/color_select`
- [ ] `web/context_processors.py` (76L) <-> `components/web/context_processors.c`
- [ ] `web/views.py` (3648L, monolithic — covers home/scenes/effects/filters/sounds/soundscapes/system/models/storage) <-> split C files: `views_home.c`, `views_scenes.c`, `views_effects.c`, `views_filters.c` (NOTE: just changed this session — Spike filter color selector — needs porting), `views_sounds.c`, `views_soundscapes.c`, `views_system.c`, `views_models.c`. NOTE: plan mentions `views_storage.c` for backup/restore but it doesn't exist in the file listing — check if backup/restore is ported at all.
- [ ] `web/views_named_ranges.py` (405L) <-> `components/web/views_named_ranges.c`

### Util
- [x] `lib/timing.py` (127L) <-> `components/util/timing.c/.h` — see finding below, left as known gap
- [x] `lib/utils.py` (7L) <-> `components/util/utils.c/.h` (new files, ported)
- [x] `lib/control.py` (58L) <-> confirmed dead code, no port needed

## Session-specific recent Python changes to prioritize porting

These are known-fresh diffs from this conversation session, already confirmed absent
from C until audited above:
1. `lib/animation.py`: `start()` now rolls back `running`/`stopped` flags if
   `_thread.start_new_thread()` raises, instead of leaving `running` stuck `True`
   forever with no backing thread.
2. Spike filter "Color" param: web UI now uses a Standard/Custom dropdown + color
   picker (matching the effect color selector) instead of a plain text box.
   Touches `templates/setup/filter_edit.html`, `templates/setup/color_select.html`,
   `web/views.py` (`_filter_edit_context`, `FilterEditView.post`, `ColorSelectView`),
   `web/routes.py` (new `/filters/color_select` route).

## Findings log

(Append findings here as each pair is audited, even if "no changes needed".)

### Storage (done)
- `persistent_dict.c`/`json_helpers.c` already correctly implement lazy-load-on-first-access
  and cover every operation actually called from Python (`get`/`set`/`__contains__`/`store`).
- FIXED: `ensure_loaded()` was missing Python's `isinstance(data, dict)` guard — Python's
  `_ensure_loaded()` discards successfully-parsed-but-non-dict JSON (e.g. a top-level array)
  and falls back to `{}`. Added a `cJSON_IsObject()` check with matching fallback.
- Known divergence (left alone, safe): `persistent_dict_save()` skips writing when `!dirty`;
  Python's `store()` always writes. Every real call site sets a value immediately before
  `store()`, so this never differs in practice.
- Known architectural divergence (pre-existing, out of scope): C splits Python's single
  `storage.json` into `system_settings.json`/`lighting_settings.json`/`sounds.json`.

### Util (done)
- `lib/utils.py`'s `bytes_to_int()` had no C port — added `utils.c`/`include/utils.h`, ported
  bug-for-bug (matches Python's non-standard bit-twiddling, verified against Python output).
  Only caller is `I2CManager` in `lib/comms.py`, which is itself dead code (never instantiated).
- `lib/control.py` (`ThinkTank`, `Orientation`) confirmed dead/unused via repo-wide grep — every
  method is `pass`, never imported anywhere. No port needed.
- `lib/timing.py`'s `TimerManager`/`Timey` (repeating-callback scheduler used by WiFi reconnect
  check + audio poll loop) has NO C equivalent — `timing.c` only has millis/micros/elapsed
  helpers. Left as a **known gap**: `sound_manager.c` and `wifi_manager.c` each reimplement
  their own poll loop directly instead of using a shared timer abstraction. Not fixed here to
  avoid stepping on the networking agent's concurrent work on `wifi_manager.c`.
- Bonus fix: `components/util/CMakeLists.txt` was missing `esp_wifi`/`esp_netif` from
  `REQUIRES` despite `comms.c` including those headers — added them (build-breaking gap found
  incidentally).

### LEDs (done)
- Added a full `onboard_led_*` API (init/set/off) — was entirely missing; single-NeoPixel
  direct-addressing bypass used for boot-time IP-flash, no gamma/brightness applied, matching
  Python's `OnboardLED`.
- FIXED: global brightness (0.0-1.0) was missing entirely (only the gamma curve existed).
  Added `leds_set_brightness()/get_brightness()`, applied after the curve step.
- FIXED: brightness curve was a hardcoded 2.2-gamma table, not Python's actual quadratic
  curve — regenerated the 256-entry table from Python's real formula.
- FIXED: curve application was per-strip; Python ORs a single global flag across all strips —
  now computed once and applied uniformly.
- FIXED: color order only handled literal "RGB"/"GRB"; "RGBW" fell through to GRB (3 bytes,
  losing the W channel). Rewrote to parse any 3/4-char R/G/B/W permutation per strip with
  correct bpp/byte order — real RGBW support now works.
- Added `leds_fill()/range()/identify()/wheel()` for full API parity.
- **Known gap, needs follow-up**: `main/boot.c` does not yet call the new `onboard_led_*` API
  to actually run the IP-last-octet flash sequence at boot — that's boot.c wiring, out of this
  agent's file scope (leds/billboard only). Needs picking up when reconciling with the
  networking/boot agent's work.

### Billboard / MAX7219 (done)
- FIXED: daisy-chain byte order was reversed (sent module N-1's data first instead of module
  0's) — would have mirrored/scrambled text across a real 4-module chain.
- FIXED: SPI mode mismatch — Python uses mode 2 (polarity=1, phase=0), C hardcoded mode 0.
- FIXED: register init order didn't match Python's shutdown-then-configure-then-enable sequence.
- FIXED: init-time clear ran before `initialized` was set, so the guard in `max7219_show()`
  silently no-op'd it — display could show power-on garbage until the first real draw.
- Added `billboard_set_pixel()`, `billboard_fill_pattern()`, `billboard_get_width()` for API
  parity. Deliberately did NOT add a raw-framebuffer accessor (Python's `.matrix` property) —
  nothing else in the C project needs it; would be speculative API surface.
