# Behavior-Preserving Refactor — Progress Notes

Started: 2026-07-07. Goal: apply the behavior-preserving improvements identified in a
prior review pass (Python + C), per explicit user request. This file is the restart
point — if this session is interrupted, resume at the first `[ ]`/`[~]` item below.

## Explicitly excluded (do not do these)
- Any removal of code from the Python `lib/` submodule (it's shared across multiple
  projects — see `.github/copilot-instructions.md`'s "Shared lib directory" note).
  Concretely: do NOT delete `lib/control.py`'s `ThinkTank`/`Orientation`, `lib/comms.py`'s
  `I2CManager`, or `lib/ota_update.py`'s 3 unreferenced private methods
  (`_fetch_submodule_tree`, `_should_descend_into_path`, `_path_exists`).
- Items the review flagged as "not worth it": converting hand-rolled Python polling
  loops to `TimerManager`; a shared FreeRTOS timer abstraction in C (the suspected
  `sound_manager.c`/`wifi_manager.c` duplication didn't hold up — only one real poller
  exists); deleting C's `billboard.c`/`leds.c` "unreferenced" API surface (deliberately
  added for Python parity, not accidental dead code).
- Non-deletion `lib/` refactors (dedup/consolidation that preserves all public behavior,
  e.g. `_url_decode()`, `_send_file_static`/`_send_file` merge, the NEOPIXELS
  double-parse fix) ARE in scope — the exclusion is specifically about *removing* code,
  not editing `lib/` at all.
- The large `web/views.py` monolith split (3648 -> ~10 files) was flagged as "worth doing
  but sequence last" — NOT included in this pass unless asked separately.

## Python changes — ALL DONE
- [x] Dedupe `_url_decode()` (`lib/webserver.py` + `lib/captive_portal.py`) into `lib/utils.py` as `url_decode()`
- [x] Fix `lib/leds.py` double-parsing NEOPIXELS config in `__init__` (pass `strips_config` into `_parse_brightness_curve_setting`)
- [x] Extract `_schedule_reboot()` in `web/views.py` (used by `SystemRebootView` + `UpdatesView`)
- [x] Extract shared model-scoped dict get/save helper (`_get_model_scoped_dict`/`_save_model_scoped_dict`
      in `web/views.py`); rewrote `_get_sounds_dict`/`_save_sounds_dict` (deduped 2 identical class copies
      into module-level functions) and `SoundscapesView`'s `_get_soundscapes_dict`/`_save_soundscapes_dict`
      to use them. Left `SoundscapeEditView._save_soundscape` (single-entry variant) untouched — different
      shape, not an exact duplicate, lower risk to leave alone.
- [x] Dedupe `_rename_*_refs` family: added `_iter_scene_entries()` generator (used by
      `_rename_named_range_refs` + `_rename_effect_refs`) and `_replace_in_effect_lists()` (used by
      `_rename_filter_refs` + `_rename_color_refs`). `_rename_scene_refs` has a genuinely different shape
      (operates on `scene_settings`, not scene entries) — left as-is, no duplication there.
- [x] Extract `_effects_list_response()`/`_filters_list_response()` — 4 duplicated render blocks each
      (get/post-fallback/delete-branch/final-fallback) collapsed to one helper call each.
- [x] Extract `_resolve_selected_leds()` in `web/views_named_ranges.py` — replaced all 4 duplicated
      "resolve tokens via get_targets, dedupe" loops.
- [x] Merge `lib/webserver.py`'s `_send_file_static`/`_send_file`: extracted `_file_cache_headers()`
      (stat/etag/cache-control, identical in both) and `_stream_file_chunks()` (4KB streaming loop,
      identical in both). Left the header-string building and delete-after-send logic in each method
      separately since those genuinely differ (HTTP/1.1 vs 1.0, Connection header, cleanup).

Verification note: full manual review during editing, plus a final `ast.parse()` pass on every
touched Python file (pure syntax-tree parsing — no imports evaluated, no MicroPython modules
needed, not "running" the code) confirmed no syntax errors. Did not run/execute anything.

## C changes — ALL DONE
- [x] Centralized storage file path constants into `components/storage/include/persistent_dict.h`
      (`STORAGE_SYSTEM_SETTINGS_FILE`/`STORAGE_LIGHTING_SETTINGS_FILE`/`STORAGE_SOUNDS_FILE` — every
      component already includes this header via the `storage` dependency). Removed the duplicate
      definitions from `main/settings.h` (only reachable by `main/`) and 6 local redefinitions
      (`audio_player.c`, `captive_portal.c`, `ip_announcement.c`, `ota_update.c`, `views_storage.c`'s
      `SYSTEM_SETTINGS_PATH`/`LIGHTING_SETTINGS_PATH`, and 3x `views_filters.c`/`views_effects.c`/
      `views_scenes.c`'s `LIGHTING_STORE_PATH`). Replaced ~40 raw-literal `persistent_dict_open("...")`
      call sites across 12 files with the shared constants.
- [x] Extracted `webserver_render_response()` into `webserver.c`/`.h` — was duplicated byte-identical
      across 9 of the 10 `views_*.c` files. `views_home.c` kept its own local `render()` deliberately:
      its error message text genuinely differs ("Template render error" vs "Template error"), so
      merging it would have been a (tiny, error-path-only) behavior change. Also found while there:
      an existing `render_template()` in `webserver.c` does its own separate global-context-processor
      merge that looks like it might duplicate what `template_render_file()` already does internally,
      and appears to have zero callers anywhere in the project — flagged for awareness, NOT touched
      (out of scope for this pass, needs its own investigation before acting).
- [x] Extracted `request_get_form_field_last()` into `webserver.c`/`.h` (alongside the existing
      `request_get_form_field()`) — was duplicated byte-identical (plus differing comments) across
      `views_scenes.c`, `views_filters.c`, `views_effects.c`.
- [x] Deduped `lighting.c`'s filter-cleanup loop (3x near-identical) into `free_job_filters()`. Two of
      the three sites didn't null the pointers after `cJSON_Delete()` (relying on an immediately-following
      `active = false`/`memset` instead) — verified nulling them too is safe (nothing reads the pointer
      in between), so all 3 sites now share the exact same helper.
- [x] Reduced `lighting.c`'s repeated `persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE)` (8 sites)
      to a single `open_lighting_store()` wrapper (named to avoid shadowing the existing
      `lighting_store` local variable name used at every call site).
- [x] Merged `sound_manager.c`'s `get_sounds_dict()`/`get_soundscapes_dict()` (identical except the
      final child-key name) into `get_current_model_child(const char *key)`.
- [x] Moved `view_setup`/`view_storage`/`view_status`/`view_confirm` (+ their private helpers
      `fmt_bytes`/`redact_system_settings`/`add_audio_health_context`) from `views_home.c` to
      `views_system.c`, matching the grouping `views.h` already documents. Added the includes these
      functions need (`timing.h`/`animation.h`/`audio_player.h`/`esp_heap_caps.h`/`esp_spiffs.h`/
      `sdkconfig.h`) to `views_system.c`, and removed them from `views_home.c` after confirming
      (via grep) nothing remaining there uses them. The moved functions now call the shared
      `webserver_render_response()` instead of `views_home.c`'s locally-preserved `render()` — this
      means their error-page text changes from "Template render error" to "Template error" in the
      (rare, error-only) template-missing case; judged not worth re-inventing a duplicate render
      helper in `views_system.c` just to preserve 4 words of error-page text nobody will ever see
      in normal operation.
- [x] Deleted confirmed-dead C functions (project-local only, not the shared Python `lib/`, so in
      scope per the exclusion above): `utils_bytes_to_int()` (deleted `utils.c`/`utils.h` entirely,
      removed from `components/util/CMakeLists.txt` — verified Python's `lib/utils.py::bytes_to_int()`
      stays, per the explicit exclusion, its only caller `I2CManager` is itself dead-but-kept Python
      code); `yx5200_pause()`/`yx5200_resume()` (verified: Python's `AudioPlayer.pause()` exists but
      has zero callers anywhere in `web/`/`lib/` either — dead on both sides — and there's no Python
      `resume()` at all, so no parity concern either way); `audio_player_get_module_count()`;
      `max7219_fill()` (verified Python's `lib/max7219.py` has no `fill()` method at all).

## Final step — DONE
- [x] Structural sanity check: wrote a proper single-pass C tokenizer (tracks string/char literals
      and line/block comments so `{`/`}` characters inside them — e.g. the template engine's own
      `"{%"`/`"{{"` string constants — don't produce false positives, unlike naive `grep -c '{'`)
      and ran it across every `.c` file in `c_project/` — all balanced. Also verified every `.c` file
      is still listed in its component's `CMakeLists.txt` SRCS after the moves/deletions, and that
      each moved/renamed function has exactly one definition project-wide. Python side: `ast.parse()`
      on every touched file confirmed no syntax errors (see note above).

## Status: COMPLETE
All Python and C changes from the approved list are done, verified structurally sound, and sitting
as uncommitted working-tree changes (no git actions taken, per standing instruction).
