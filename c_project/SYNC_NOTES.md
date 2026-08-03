# C Project Sync Audit — Progress Notes

Started: 2026-07-06. Goal: bring `c_project/` (ESP-IDF C port) up to date with the
current state of the Python/MicroPython implementation, per explicit user request
(overriding the normal "c_project does not need to stay in sync" default noted in
`.github/copilot-instructions.md` for this one-time full audit).

This file is the restart point. If this session is interrupted, read this file first,
then resume at the first `[ ]` (not started) or `[~]` (in progress) item below.

**STATUS: full first-pass audit complete as of 2026-07-07** — every file pair in the mapping
below is `[x]`. Remaining work is the small follow-up list a few sections down (WiFi timeout,
a couple of deferred lighting features, module_idx gap in sounds), not a fresh audit pass.
Nothing has been committed to git — all of this sits as uncommitted working-tree changes,
per explicit instruction that only the user runs git commands.

## Post-audit feature additions (kept in sync, not part of the original audit)

- **2026-07-24 — Named Ranges "Reorder Strings" tool**: new feature (not an
  audit finding) letting the user swap/reorder physically-distinct LED
  strings (primary, contiguous, non-overlapping named ranges) and have the
  system rewrite every affected LED index reference automatically. Landed in
  both builds together: Python `web/views_common.py` (`_reindex_leds`),
  `web/views_named_ranges.py` (`_is_primary_contiguous`,
  `_reorder_eligible_ranges`, `_compute_swap_permutation`,
  `_swap_named_ranges`, `NamedRangeReorderView`), and a prerequisite parity
  fix to `lib/lighting/lighting.py`'s `get_targets` (added comma-separated
  multi-token string support, matching what `target_spec_resolve` already
  did in C) — mirrored line-for-line in
  `c_project/components/web/views_named_ranges.c`
  (`is_primary_contiguous`/`reorder_eligible_ranges`/
  `compute_swap_permutation`/`reindex_leds`/`swap_named_ranges`/
  `view_named_range_reorder`), `routes.c`, and `views.h`. New shared
  template `templates/setup/named_ranges_reorder.html` serves both builds.
  No C-side equivalent of the Python parity fix was needed since
  `target_spec_resolve` already supported comma-joined specs.

- **2026-07-26 — Scene "active on boot" marking**: new feature letting any
  number of scenes be flagged (`scene_settings.<name>.active_on_boot`) to
  activate together at startup, replacing the previously dead/unreachable
  `default_scene` single-scene mechanism (it had no UI setter in either
  build — confirmed via full-repo grep before starting). If no scene is
  flagged, falls back to the old single-arbitrary-scene behavior so
  existing installs aren't left dark after upgrading. Python:
  `Lighting._activate_boot_scenes()` (`lib/lighting/lighting.py`), called
  from `__init__` and (replacing the old bare `set_scene(None)` call) from
  `set_current_model()`; UI in `web/views_scenes.py`
  (`SceneEditView.post`'s new isolated `set_active_on_boot` action, kept
  separate from the pre-existing `update_scene_settings` batch handler
  specifically so it can't be clobbered by, or clobber, the other
  scene-settings forms) and `templates/setup/scene_edit.html`. C mirror:
  `lighting_activate_boot_scenes()` (`components/lighting/lighting.c/.h`),
  called from `main/boot.c` after `lighting_init()` and from
  `components/web/views_models.c`'s two model-switch call sites (which
  previously activated no scene at all on switch — a pre-existing C/Python
  divergence, fixed as a natural side effect of wiring this in); web side in
  `components/web/views_scenes.c` (`set_active_on_boot_from_form`,
  `fill_scene_edit_context`'s `scene_active_on_boot` context key).

- **2026-08-03 — Per-segment LED color order**: new feature letting an
  LED-index range *within* a single configured strip (e.g. a physically
  joined-on section from a different product/batch) use a different
  color_order than the rest of that strip. Hard constraint: a segment's
  color_order must have the same channel count (bpp) as its strip's own —
  forced by the C driver's `rgb_t{r,g,b}` pixel buffer having no stored W
  channel and `leds_show()` transmitting each strip as one fixed-stride
  `rmt_transmit()` call; supporting mixed bpp within one strip would need a
  stored W channel and variable-stride packing, a materially bigger change.
  Storage: optional `segments: [{start, end, color_order}]` per strip
  entry, start/end inclusive and local to that strip. Validation follows
  the audio-player silent-drop convention (invalid/out-of-range/
  overlapping/bpp-mismatched rows are dropped, not surfaced as a form
  error), not the hostname reject-whole-form pattern — confirmed by
  checking how `SystemSettingsView.post()` already treats its other
  repeating rows before choosing this. Fixed a real bug found along the
  way: `_parse_neopixels_storage`/`parse_neopixels_storage` discarded any
  unknown key from a stored strip dict, which would have made a
  freshly-saved `segments` list vanish on the very next render.
  Python: `lib/leds.py` (`_effective_order()`, per-pixel lookup in
  `set()`/`get()`/`identify()`, `fill()` restructured from one bulk strip
  fill into a bulk base-order fill plus per-segment overwrite since a
  single reordered color is wrong for a segment's pixels);
  `web/views_system.py` (`_parse_segments_storage`, `ss_segments`/
  `ss_strip_count` context keys, new segment-row parsing block in
  `SystemSettingsView.post`); `templates/setup/system_settings.html` (new
  segments table + `addSegmentRow()` JS, building its strip dropdown live
  from the current strip rows rather than stale server context).
  `docs/settings_template.py` updated to document the new key (also fixed
  a pre-existing unrelated bug found while touching this file: the whole
  file's docstring was never closed, so it failed `ast.parse()` even
  before this session's edit).
  C mirror: `components/leds/include/leds.h`
  (`strip_segment_config_t`/`MAX_SEGMENTS_PER_STRIP`), `components/leds/leds.c`
  (`strip_segment_t`, segment parsing in `leds_init_from_config`/
  independent re-validation in `leds_init`, `effective_order_indices()`
  used by `leds_show()`'s per-pixel loop — `bpp`/buffer-stride math is
  untouched, only which `order_indices` gets read per pixel changes);
  `components/web/views_system.c` (`parse_segments_storage`, flattened
  `ss_segments`/`ss_strip_count` in `add_system_settings_context`, segment
  parsing block in `view_system_settings`'s POST branch).

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
- [x] `boot.py` (163L), `main.py` (9L), `settings.py` (214L) <-> `main/boot.c/.h`, `main/main.c`, `main/settings.h` — DONE

### Networking
- [x] `lib/comms.py` (413L, WIFIManager) <-> `components/network/wifi_manager.c/.h` — DONE
- [x] `lib/captive_portal.py` (621L) <-> `components/network/captive_portal.c/.h` — DONE
- [x] `lib/ip_announcement.py` (223L) <-> `components/network/ip_announcement.c/.h` — DONE (new file, now wired into boot.c too)
- [x] `lib/ota_update.py` (1101L) <-> `components/network/ota_update.c/.h` — DONE (deliberately not line-for-line; C uses GitHub-releases + esp_https_ota per c_plan.md, verified settings-key parity)
- [x] mDNS piece of `boot.py` <-> `components/network/mdns_setup.c/.h` — DONE, was already correct/complete (not actually a stub)

### Webserver core
- [x] `lib/webserver.py` (1439L) <-> `components/webserver/webserver.c/.h`, `template_engine.c/.h`, `request_parser.c/.h`, `static_files.c/.h`, `mime_types.c/.h`, `response.h` (no .c, confirmed intentional) — DONE

### LEDs
- [x] `lib/leds.py` (469L, incl. OnboardLED) <-> `components/leds/leds.c/.h`

### Audio
- [x] `lib/audio.py` (623L) <-> `components/audio/audio_player.c/.h`, `yx5200.c/.h` — DONE, already correct, no bugs found
- [x] `lib/sounds.py` (709L) <-> `components/audio/sound_manager.c/.h` — DONE, already correct, no bugs found

### Billboard
- [x] `lib/billboard.py` (214L) <-> `components/billboard/billboard.c/.h`
- [x] `lib/max7219.py` (89L) <-> `components/billboard/max7219.c/.h`

### Lighting core
- [x] `lib/animation.py` (142L) <-> `components/lighting/animation.c/.h` — DONE (part 1 agent)
- [x] `lib/lighting/colors.py` (36L) <-> `components/lighting/colors.c/.h` — DONE (part 1 agent)
- [x] `lib/lighting/effects.py` (292L) <-> `components/lighting/effects.c/.h` — DONE
- [x] `lib/lighting/filters.py` (270L) <-> `components/lighting/filters.c/.h` — DONE (full rewrite, was non-compiling)
- [x] `lib/lighting/lighting.py` (561L) <-> `components/lighting/lighting.c/.h` — DONE
- [x] `lib/lighting/metadata.py` (79L) <-> `components/lighting/metadata.c/.h` — DONE (part 1 agent)
- [x] `lib/lighting/patterns.py` (333L) <-> `components/lighting/patterns.c/.h` — DONE (full rewrite)
- [x] named ranges logic <-> `components/lighting/named_ranges.c/.h` — DONE (part 1 agent)

### Web views
- [x] `web/routes.py` (78L) <-> `components/web/routes.c/.h` — DONE (routes/home/scenes/effects agent). Full route-table audit against every View class's get()/post() in web/views.py + web/views_named_ranges.py. See findings below.
- [x] `web/context_processors.py` (76L) <-> `components/web/context_processors.c` — DONE (same agent). Fixed a real bug, see findings.
- [x] Python web views <-> C web views — BOTH sides now use the same per-feature layout.
  Python was split (2026-07-08) from the old 3,600-line `web/views.py` monolith into
  `views_common.py` + `views_home/scenes/effects/filters/sounds/soundscapes/system/models/storage/colors.py`,
  with `views.py` reduced to a thin re-export aggregator (`routes.py` unchanged). The
  Python-to-C file mapping is now essentially 1:1: `views_<area>.py` <-> `views_<area>.c`.
  Known layout differences: Python's `views_colors.py` content lives in C's `views_system.c`;
  Python's shared `views_common.py` helpers map to C's `webserver.c` helpers or per-file
  statics; C's `view_setup`/`view_storage`/`view_status`/`view_confirm` live in
  `views_system.c` while Python keeps StorageView/backup/restore in `views_storage.py`.
- [x] `web/views_named_ranges.py` (405L) <-> `components/web/views_named_ranges.c` — DONE, 1:1 match confirmed

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

### Lighting core part 2: lighting/patterns/filters/effects (done)
- `lighting.c`: prior pass's model-lookup/default-scene helpers were already complete, no
  stubs left. FIXED: `lighting_create_model`/`delete_model`/`rename_model` were declared in
  the header but never defined (would fail to link) — implemented, mirroring Python's
  deep-copy-allowed-keys / refuse-to-delete-active-model / current_model-update-on-rename.
  FIXED: filters were called with the global animation tick instead of the effect's
  `local_tick`, unlike Python. Added plumbing (`tick_pattern_current_buf`,
  `retained_meet_index/set`) needed for the patterns.c rewrite below.
  **Known gaps (documented, not fixed — out of this pass's scope):**
  `convert_frequencies_to_durations()` migration utility (low-priority, one-time UI action);
  `trigger_scenes_on_completion` scene-metadata field not read (needs a `metadata.h/.c`
  addition); `inherit_target`/passthrough chaining between `after`-dependent scene entries.
- `patterns.c`: FULL REWRITE. Real bugs fixed: blink's default duration was 20 vs Python's 40;
  pulse's period fallback was `duration*4` instead of Python's flat 40 (root cause: a blanket
  duration default of 40 masked each pattern's own correct default — changed to an unset
  sentinel so per-pattern defaults apply); breathe had a spurious -pi/2 phase shift causing it
  to start fully dark instead of mid-brightness; cylon/phaser_strip defaulted to red/blue
  instead of Python's white/black. Rewrote wave/cylon/phaser_strip to fade the *previous
  frame's* color (trailing-decay render) instead of recomputing a static shape each tick,
  matching Python — required extending the pattern function signature to receive a
  `current_colors` buffer. Added phaser_strip's persisted random meeting point and blink's
  legacy frequency->duration conversion.
  **Known gap**: per-pattern cycle counting still happens centrally in `lighting.c` rather
  than per-pattern like Python — an approximation for non-periodic patterns.
- `filters.c`: FULL REWRITE — found a **critical bug**: `filter_spike`/`filter_dropout`
  referenced struct fields (`spike_active`, `last_spike_tick`, `spike_led_index`,
  `spike_subrange_index`) that don't exist in `filter_state_t` — **this would not compile**.
  Rewrote to use the actual group-state array, matching Python's period/duration/variation/
  heat/scope semantics with independent per-group timers. Also fixed: sizzle/scintillate used
  wrong field names/formula and the wrong variation default (20% not 30% for scintillate), and
  wrong per-channel randomization (sizzle needs one shared R/G/B deviation for all LEDs,
  scintillate needs independent per-LED-per-channel deviations — both were applying a single
  deviation to all 3 channels). Added `filter_variation_percent()` for the new float field vs.
  legacy 0-255 int. Confirmed `color_resolve_json()` in `colors.c` (added in an earlier pass)
  already correctly handles string/`custom:`/array color specs for spike. Fixed dropout to
  always force black (previously only did so if "color" was absent).
- `effects.c`: most of Python's `EffectRuntimeMixin` logic already lives directly in
  `lighting.c`'s tick loop (a reasonable architectural split — small file size isn't itself a
  gap). Fixed the duration-default bug and added blink's legacy frequency override here too.

### Boot / main / settings (done)
- Wired up the previously-orphaned `onboard_led_*` API (from the LEDs pass) into a new
  `ip_flash_task()` + `start_ip_flash_sequence()` in `boot.c` — faithfully ports
  `_flash_ip_last_octet()`/`_run_ip_flash_sequence()` (hundreds/tens/ones digit ->
  red/green/blue, 1s on/off, skip zero digits, 3 repeats, background/non-blocking).
- FIXED: `ip_announcement_check_and_announce()` (built by the networking pass) was never
  called from `boot.c` — added the call right after WiFi connects, matching `boot.py`.
- FIXED: hostname was never actually set on the STA interface — `wifi_manager_set_hostname()`
  existed but was never called; added the call before `wifi_manager_connect()`. Also fixed
  the hostname fallback to treat a stored *empty string* as unset (previously only a missing
  key fell back to "lightmotron").
- FIXED: WiFi credential gate only checked `ssid` non-empty; Python requires both `ssid` and
  `password` before attempting to connect. Fixed to match (avoids a wasted ~10s connect
  attempt with a blank password).
- FIXED: `stored_ip_address` default (from `settings.py`'s `_SYSTEM_SETTINGS_DEFAULTS`) was
  never seeded by `boot_seed_defaults()` — added. Was latent, not crash-causing (read-time
  code already handled a missing key gracefully).
- Added missing `ESP_LOGI` calls mirroring `boot.py`'s `print()` statements.
- `main.c` correctly delegates to `boot_init()`, which runs `lighting_init()` +
  `animation_start()` at the end, matching `main.py`'s effective behavior.
- **Known gaps, deliberately left (need a small follow-up, low priority):**
  - `main.py`'s debug-only `add_colors({"test color": (123,45,67)})` has no C equivalent
    (no runtime "add named color" API exists in `lighting.h`/`colors.h`) — debug-only, low value.
  - ~~`wifi_manager.c` hardcodes a 10s connect timeout vs. Python's `timeout=20`~~ — fixed
    since this audit, now 20000ms.
  - `boot_seed_defaults()` seeds a `"billboard"` settings block with a `"clk"` key that
    Python's real defaults don't include (only a legacy `"sck"`-named fallback) — numeric pin
    values match exactly, so no hardware risk, just a schema-naming quirk.

### Networking (done)
- Previous pass's `wifi_manager.c/.h`, `captive_portal.c/.h`, `ota_update.c/.h`,
  `ip_announcement.c/.h` all verified correct line-by-line against their Python counterparts.
- FIXED (real bug): `wifi_manager_disconnect()` didn't suppress the
  `WIFI_EVENT_STA_DISCONNECTED` handler's auto-reconnect — a deliberate disconnect (e.g. for
  a hostname-change flow) would race against the event handler's own reconnect using stale
  config. Added a `manual_disconnect` flag, set on disconnect, cleared on connect, checked by
  the event handler.
- `mdns_setup.c/.h`: turned out to already be complete and correct (not a stub as initially
  suspected from line-count alone) — inits `mdns` component, sets hostname, advertises
  `_http._tcp` on port 80.
- `ota_update.c`: confirmed deliberately architected differently from Python (GitHub-releases
  + `esp_https_ota` vs. Python's git-tree file sync, per `c_plan.md`) — verified settings-key
  parity (`system_settings.ota.repo_owner/repo_name`) matches `web/views.py`.
- Both gaps this agent found (`ip_announcement` + hostname-setting never called from boot)
  were fixed by the boot/main/settings pass above instead, since they required touching
  `boot.c` which was out of this agent's scope.

### Audio (done)
- Exceptionally thorough pass — `yx5200.c/.h` (DFPlayer frame protocol, checksum, all
  commands, response parsing with echo-vs-reply disambiguation, 6-confirmation debounce for
  flaky "stopped" reports), `audio_player.c/.h` (0-3 module management, candidate-selection/
  high-quality preference, health check, volume), and `sound_manager.c/.h` (title->file
  lookup, stops-list, native loop_count, soundscape state machine with `after` dependencies
  and repeat/self-heal logic, chain_next) were ALL already correct — no bugs found, no
  changes made.
- **Known gap, not fixed**: Python's UI "Stop" button does a 5-attempt hardware-verified stop
  (up to ~1.25s worst case); the C port's single `sound_manager_stop()` serves both that path
  and the fast internal stops-list path. Upgrading it would regress the internal path's speed.
  Flagged for whoever next touches `views_sounds.c` in case a dedicated verified-stop entry
  point is wanted.

### Webserver core (done)
- `template_engine.c`: 3 bugs fixed — `is_truthy()` used case-insensitive `strcasecmp` for
  "false"/"none" but Python's falsy check is exact-case (a lowercase `"false"` context value
  is truthy in Python, was wrongly falsy in C); an unguarded `clean[len-1]` string-literal
  quote check caused an out-of-bounds read on empty `{{  }}` expressions; `==`/`!=` required
  surrounding spaces and checked `==` before `!=` (Python matches bare `a!=b` and checks `!=`
  first to avoid mis-splitting `!==`). Everything else (dot-notation/index resolution incl.
  index-as-context-var, and/or/not/in/not-in precedence, nested if/else depth tracking, all 4
  for-loop variants incl. nested context cloning, includes, processing order) verified correct.
  Known minor gap left: `{% for x in d.items() %}` with no comma hardcodes the 2nd var to ""
  instead of binding the whole tuple to `x` — low-risk, only affects a malformed loop header.
- `request_parser.c`: 2 bugs fixed — bare query keys with no `=` were silently dropped (Python
  keeps them with an empty value); an oversized Content-Length was clamped and the truncated
  body parsed instead of aborting (Python's `_read_request` returns `None`, connection closes,
  no partial body ever gets processed) — fixed to free/return NULL to match. Multipart parsing,
  boundary extraction, duplicate-key->array merging, URL-decoding all verified correct.
- `webserver.c` (untouched before this pass): 3 bugs fixed — keep-alive default ignored the
  actual HTTP version (`is_http_1_1` was computed but never read), so an HTTP/1.0 client
  without `Connection: keep-alive` was wrongly kept alive; missing `www/index.html` on `GET /`
  fell through to a 404 instead of Python's documented hardcoded 200 health-check page; a
  route handler returning NULL sent no status line at all (breaks HTTP framing on keep-alive)
  instead of Python's 204 No Content. Route dispatch/trailing-slash fallback, 5s keep-alive
  timeout, Connection header parsing, response send path all verified matching.
- `static_files.c`/`mime_types.c`: no bugs found. Noted (not removed) intentional supersets
  beyond Python parity: C does If-None-Match/304 handling Python never had, and has extra MIME
  entries + case-insensitive extension matching vs Python's case-sensitive exact dict lookup —
  both strict improvements, left as-is.
- `response.h`/no `response.c`: confirmed intentional — all response-building lives in
  `webserver.c`; `response.h` is just a re-export pointer.

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

### Web: routes.c / context_processors.c / views_home.c / views_scenes.c / views_effects.c (done)
- `routes.c` was drastically out of sync — audited every (path, View class) pair in
  `web/routes.py` against every `get()`/`post()` in `web/views.py` + `web/views_named_ranges.py`.
  FIXED many missing/wrong method registrations (Python registers ONE path per View class but
  the C table needs one `webserver_add_route()` per (method, path)):
  - Added missing POST registrations (view already existed, just wasn't reachable via POST):
    `/models`, `/named_range`, `/custom_colors`, `/theme`, `/hostname`, `/system_settings`,
    `/updates`, `/sounds`, `/soundscapes`.
  - Added missing GET registrations: `/sounds/edit`, `/soundscapes/edit`.
  - FIXED wrong method: `/restore/confirm` and `/system_reboot/confirm` were registered GET,
    but `RestoreConfirmView`/`SystemRebootConfirmView` only implement `post()` in Python — GET
    would 404/405 in the C build even though the handler exists.
  - Added the sibling agent's new filter routes: `POST /filters` (create/delete, separate
    `view_filters_post` handler, matching `FiltersView.post()`), `GET /filters/edit` (alongside
    existing POST — `view_filter_edit` already branches on `req->method`), and
    `POST /filters/color_select` -> `view_filter_color_select` (was defined in
    `views_filters.c` but never registered anywhere, and not even declared in `views.h` — added
    both).
  - Fixed `/scenes/color_select` and `/effects/color_select`: were registered GET, but
    `ColorSelectView` only implements `post()` — templates (`pattern_params.html`,
    `filter_edit.html`) always `hx-post` to these URLs, so GET was simply unreachable dead code.
  - Added GET+POST for `/scenes`, `/scenes/edit`, `/effects`, `/effects/edit` (see views_scenes.c/
    views_effects.c below — these previously only had one method registered, or the wrong one).
  - **Known gap, not fixed**: `POST /system_settings/ip_announced` (`SystemSettingsIPAnnouncedView`)
    has no C handler at all. A concurrent sibling pass added `view_system_settings_ip_announced`'s
    *declaration* to `views.h` while this agent was working but has not defined it yet in
    `views_system.c` — once it exists, add
    `webserver_add_route(HTTP_METHOD_POST, "/system_settings/ip_announced", view_system_settings_ip_announced);`
    to `routes.c`.
- `context_processors.c`: **FIXED a real bug** — `build_global_context()` computed
  `theme_css_path` as `/themes/<name>.css`, appending `.css` unconditionally, but the stored
  `theme` value (set by `ThemeView.post()` in Python) is already a full filename *with* the
  `.css` extension — so the C build always produced a double extension
  (`/themes/dark_red.css.css`). Worse, **the shared template that actually renders the link,
  `templates/base/imports.html`, reads `{{ theme_css }}` (no leading slash, no `theme_css_path`
  name) — a key the C context never set at all**, so custom themes silently never applied in
  the C build regardless of the double-extension bug. Also fixed the no-theme-selected fallback:
  it hardcoded `"theme": "default"` / `/themes/default.css"`, but no such file exists on disk
  (`www/themes/` only has the actual named themes) and Python's real behavior is an empty
  string (`{% if theme_css %}` guard skips the `<link>` entirely). Now emits a correct
  `theme_css` (`"themes/dark_red.css"` or `""`), and fixed `theme_css_path`/`theme`/`hostname`
  for consistency, though only `theme_css` is actually consumed by any shared template found.
- `views_home.c`, `views_scenes.c`, `views_effects.c`: **all three were severely stale** —
  looked like an early scaffold from before the current Python data model (effects have
  `pattern`/`colors`/`cycles`/`filters`, scene entries have `effect`/`target`/`cycles`/`after`/
  `inherit_target`) existed. `views_effects.c` in particular used flat `pattern`/`target`/
  `period`/`duty` fields that don't correspond to anything in the current app at all. Rewrote
  all three to match current Python line-by-line:
  - `views_home.c`: `view_home`/`view_set_scene`/`view_scene_panel_status` built context with
    raw `scenes`/`scene_settings` JSON objects that `home.html`/`scene_panel.html`/
    `buttons.html` don't use *at all* — the templates actually need `scenes` (sorted name
    list), `ongoing_scenes`/`immediate_scenes` (split by whether every entry has an explicit
    cycle limit — ported `Lighting.is_scene_ongoing()`'s logic locally since the C lighting
    layer has no equivalent function), `active_scenes`/`active_scenes_label` (em-dash
    placeholder when empty), matching `_scenes_context()`. `view_animation` used
    `animation_paused` (a key no template reads) and never set `animation_stopped` (which
    `animation/start_button.html`/`stop_button.html` both need) — so the buttons' active/muted
    styling could never actually update. Also added start/stop-only 400 on invalid action
    (Python's `AnimationView.post()` has no pause/resume at this route — confirmed no template
    or route references pause/resume — and no request validation existed before). `view_set_scene`
    never validated the scene name existed (Python returns 400 "Invalid scene").
    **Known gap**: Python's `HomeView.get()` also merges `_soundscapes_context(include_active=True)`
    for the soundscapes panel `home.html` includes — that helper/data belongs to
    `views_soundscapes.c` (out of scope here). **Known gap**: `SetSceneView.post()`'s "set"
    branch forwards arbitrary extra form fields as `lights.set_scene(name, **kwargs)`; C's
    `lighting_set_scene()` has no such passthrough. **Known gap (pre-existing, untouched)**:
    `view_setup`/`view_storage`/`view_status`/`view_confirm` in this same file are much
    smaller than Python's `SetupView`/`StorageView`/`StatusView`/`ConfirmView` — left alone,
    out of the "home views" scope for this pass (`SetupView` in particular aggregates
    named_ranges/custom_colors/scenes/effects/filters/soundscapes/sounds — a system-level
    concern, not home).
  - `views_scenes.c`: full rewrite of `view_scenes` (was GET-only; added POST
    create_scene/delete_scene/rename_scene/copy_scene), `view_scene_edit` (was POST-only with a
    completely different/stale entry shape; added GET, rewrote to
    `effect`/`target`/`cycles`/`after`/`inherit_target` entries plus scene-level settings —
    kills/trigger_scenes_on_completion/sound/stop_sounds_on_start/stop_sounds_on_end — ported
    `_scene_edit_context()`, `_rename_scene_refs()`, `_rename_scene_entry_after_refs()`,
    `_clear_scene_entry_after_refs()`), and `view_scenes_color_select` (was a stub returning
    `custom_colors` + a raw `field` query param — the real `ColorSelectView.post()` resolves a
    submitted color value into a hex/pill/picker fragment; rewrote to match, this is also what
    `view_effects_color_select` delegates to).
  - `views_effects.c`: full rewrite of `view_effects` (create/delete) and `view_effect_edit`
    (pattern selection -> `pattern_params.html` fragment, the ordered filter-chain manager
    actions add/remove/move up/down, and `update_effect`/`delete_effect`) — ported
    `_pattern_params_context()` (Standard/Custom/color-picker dropdown state per color slot,
    using the existing `metadata_get_pattern()` table), `_parse_effect_from_form()` (indexed
    `param_color_N` hex-or-name parsing, generic typed `param_*` fields, `effect_filter_*`
    checkboxes), and `_rename_effect_refs()`. The `show_target=true` filter-checkbox branch of
    `_pattern_params_context()` was deliberately not ported — every current call site passes
    `show_target=false` (dead code in the Python source itself, per its own docstring: scene
    editing uses the ordered filter manager, not inline checkboxes).
  - Both `EffectsSummaryView`/`ScenesSummaryView` context shapes were double-checked against
    their actual templates: `effects_summary.html` wants plain sorted name strings
    (`effect_names`), `scenes_summary.html` wants `{name, effect_count}` objects (`scenes`) —
    these differ from each other and it would have been an easy mixup.

### Web: views_sounds.c / views_soundscapes.c / views_system.c / views_models.c / views_named_ranges.c / views_storage.c (done)
- FIXED (build-breaking): `views_storage.c` (holds `view_backup`/`view_restore`/
  `view_restore_confirm`, split out of `views_system.c` by an earlier pass) was **missing
  from `c_project/components/web/CMakeLists.txt`'s SRCS list** — would have caused a link
  failure (undefined references) at build time despite the routes already being correctly
  registered in `routes.c`. Added it to SRCS.
- FIXED (functionally broken): `view_custom_colors`/`view_custom_colors_summary` in
  `views_system.c` were substantively wrong vs. `CustomColorsView` + `custom_colors.html`:
  read the wrong form field names (`name`/`color` instead of `color_name`/`color_value`);
  stored colors as raw hex strings instead of `[r,g,b]` integer arrays — the exact format
  `colors.c`'s `color_resolve_custom()` requires, so `"custom:<name>"` color resolution would
  have silently broken at runtime; only supported save/delete, missing update/edit_form/cancel
  (all actively used by the template); missing rename-reference propagation (`custom:old` ->
  `custom:new` across every effect's colors list); wrong context shape (raw cJSON object vs.
  the sorted `[[name,[r,g,b]], ...]` list the template iterates with tuple-unpack + `.0/.1/.2`
  indexing). Rewrote to fix all of the above.
- Cleaned up two stale "not yet wired in" comments left over from before the ip_announced
  route was manually added (in `routes.c` and `views_system.c`).
- Verified correct, no changes needed: `views_sounds.c`, `views_soundscapes.c` (context
  builders/form fields/actions all match; one pre-existing documented gap — `module_idx` is
  hardcoded to 0 in sounds list/play-button responses because `sound_manager_get_playing()`
  only exposes titles, not the title->module-index mapping; fixing needs a `sound_manager.c`
  API extension, out of scope here); `views_named_ranges.c` (1:1 match, including cycle
  detection and the exact `animation.pause()`-on-GET/`resume()`-on-POST behavior);
  `views_models.c` (create/delete/rename/set/wrap all match); rest of `views_system.c` (theme
  picker/upload/delete, hostname validate/apply/restart, WiFi/NeoPixel/audio-player settings
  parsing, reboot, OTA — the OTA architecture divergence is pre-existing/documented, not a
  bug); `view_storage`/`view_status` in `views_home.c` (field-for-field match including
  WiFi-password redaction; one low-value cosmetic gap: status page's "MicroPython
  version"/"Platform" fields are blank in C, not populated).
- `routes.c`/`include/views.h`: final check confirmed every declared view function has a
  matching route registration and there are no duplicates.

## Consolidated follow-up list (small, deliberately deferred items — not a fresh audit needed)

1. ~~`wifi_manager.c` hardcodes a 10s WiFi connect timeout vs. Python's 20s (real behavior diff).~~
   Fixed since this audit: `WIFI_CONNECT_TIMEOUT_MS` is now 20000, matching Python.
2. `lighting.c`: `trigger_scenes_on_completion` scene-metadata field not read (needs a
   `metadata.h/.c` addition); `inherit_target`/passthrough chaining between `after`-dependent
   scene entries not implemented; `convert_frequencies_to_durations()` migration utility not
   ported (low-priority one-time UI action).
3. `sound_manager.c`: no title->module-index mapping exposed, so `views_sounds.c` can't report
   which physical module is playing a given sound (hardcoded to module 0 in the UI).
4. `sound_manager_stop()` is a single fast implementation; Python distinguishes a fast
   internal stop from a 5-attempt hardware-verified UI "Stop" button stop — C's version is
   always the fast one. Low risk, flagged in case a dedicated verified-stop entry point is
   wanted later.
5. `template_engine.c`: `{% for x in d.items() %}` with no comma (single var, no tuple unpack)
   hardcodes the second variable to `""` instead of binding the whole tuple to `x` — only
   affects a malformed loop header, unlikely to appear in real templates.
6. `boot_seed_defaults()` seeds a `"billboard"` settings block with a `"clk"` key that
   Python's real defaults don't include (only a legacy `"sck"`-named fallback) — numeric pin
   values match exactly, so no hardware risk, just a schema-naming quirk.
7. `main.py`'s debug-only `add_colors({"test color": (123,45,67)})` has no C equivalent (no
   runtime "add named color" API) — debug-only, low value.
8. ~~`status.html`'s "MicroPython version"/"Platform" fields are blank in the C status page.~~
   Fixed since this audit: `views_system.c` now reuses those context keys to report
   `"ESP-IDF " IDF_VER` / `CONFIG_IDF_TARGET` instead of leaving them blank.
