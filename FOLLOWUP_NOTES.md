# Follow-up Improvements — Progress Notes

Started: 2026-07-07. Goal: address the remaining improvement items (1–5 from the review),
per explicit user request. Item 6 (housekeeping: committing, notes-file cleanup) is
EXCLUDED — the user will handle git themselves. This file is the restart point — if this
session is interrupted, read this first, then resume at the first `[ ]`/`[~]` item.

**STATUS: ALL ITEMS COMPLETE as of 2026-07-08.** Every section below is done and verified
(ast.parse on all touched Python; tokenizer brace-balance on all touched C; route-name
resolution cross-check on the views split). Everything sits as uncommitted working-tree
changes — no git actions taken, per standing instruction.

## Standing constraints
- NO git commands, ever, by the orchestrating session or any subagent.
- No deletions from the Python `lib/` submodule (shared across projects) — annotation-only
  changes to `lib/` are fine.
- `.github/copilot-instructions.md`: C project must be kept in sync with Python changes in
  the same session; docs (README.md, docs/*.md, docs/settings_template.py) must be kept in
  sync with code changes.
- Python files can be checked with `ast.parse()` (parse-only) but never executed.

## Work items

### 1. C parity fixes — ALL DONE (done inline by orchestrator; agents kept failing on
      session limits, so this was NOT done by agent A)
- [x] `wifi_manager.c`: `WIFI_CONNECT_TIMEOUT_MS` 10000 -> 20000, comment cites boot.py.
- [x] `trigger_scenes_on_completion`: added `trigger_scenes_on_completion[8][64]` + count to
      `scene_metadata_t` (`metadata.h`), parsing in `metadata.c`'s `metadata_get_scene()`.
      In `lighting.c`'s `lighting_process_tick()`: completed scenes' triggers are COLLECTED
      into a flat capped `pending_triggers[8][64]` stack list while holding the mutex, then
      fired via `lighting_add_scene()` AFTER `xSemaphoreGive` — cannot fire inline because
      `activate_scene()` -> `lighting_remove_scene()` re-takes the non-recursive lighting
      mutex (deadlock). Matches effects.py's add-triggers-then-remove ordering; name
      validation happens inside activate_scene like Python's `in scenes` guard.
- [x] `inherit_target`: added `bool inherit_target` to `active_job_t` (`lighting.h`), parsed
      from scene entries in `activate_scene()`. Applied in the tick loop at the moment an
      `after`-dependent job first starts (`after_pending` -> false): memcpy the finished
      predecessor's resolved `target_indices`/`target_count`. Chains (A->B->C) work because
      the predecessor's arrays already hold whatever IT inherited. (Python inherits the
      target SPEC via passthrough; C inherits the resolved indices — equivalent since C
      resolves specs once at activation.)
- [x] `sound_manager_get_playing_module(title)` added (`sound_manager.h`/`.c`) — returns
      module index or -1. `views_sounds.c` now uses it for the sounds-list `module_idx`
      field AND the play-button response's hidden `module_idx` (both previously hardcoded 0).
- [x] `sound_manager_stop_verified(title)` added — mirrors `SoundManager.stop_sound()`:
      collect candidate modules under mutex, then per module up to 5 attempts of
      stop -> 150ms delay -> hardware status re-query (`audio_player_is_module_playing`),
      only marking stopped + clearing soundscape state on hardware confirmation. Hardware
      retry loop runs OUTSIDE the sound mutex so a stubborn module can't stall the poll
      task. `view_stop_sound` now calls it; all internal stops-list sites keep the fast
      `sound_manager_stop()`.
- [x] `render_template()` investigation: confirmed ZERO callers, and
      `webserver_set_context_processor()` had zero registrations — the whole mechanism was
      dead (views build context from `build_global_context()` explicitly; no missing-context
      bug exists). Removed: `render_template()`, `webserver_set_context_processor()`, the
      `context_processor_fn` typedef, the `global_context_processor` static, and updated the
      stale comments in `views.h` + `webserver.h` that referenced them.
- All touched C files pass the tokenizer brace-balance check.

### 2. web/views.py split — DONE (completed across two agent passes + orchestrator verification)
- Final layout: `views_common.py` (shared helpers + `lights` singleton), plus per-feature
  `views_home.py`, `views_models.py`, `views_soundscapes.py`, `views_storage.py`,
  `views_colors.py` (incl. the shared `ColorSelectView`), `views_system.py` (14 classes:
  setup/status/confirm/theme/hostname/updates/system-settings/reboot),
  `views_scenes.py`, `views_effects.py`, `views_filters.py`, `views_sounds.py`
  (`AudioVolumeView` lives here despite sitting in the soundscapes region of the original).
- `web/views.py` is now a ~97-line aggregator: performs the
  `views_named_ranges.initialize_named_range_views(...)` wiring exactly once, re-creates
  the 4 NamedRange* aliases, and re-exports every view class. `web/routes.py` UNCHANGED.
- Audit (agent + independent orchestrator re-check): all 16 web/*.py files ast.parse-clean;
  all 54 View classes from the snapshot in exactly one module each; all 58 `views.X`
  references in routes.py resolve; every top-level def/constant accounted for. Only 7
  intentional body diffs vs the original: 5 convention fixes (docstrings on HomeView/
  ModelsSetView/SystemRebootConfirmView.post, `-> str` on AnimationView.post/
  SetSceneView.post), StatusView's new `reset_cause` key, and the `home_only` drop.
- `home_only`: dropped from `_soundscapes_context` (verified unused); KEPT on
  `_sounds_context` (used by StopAllSoundsView + SoundsStatusView).
- Docs: `docs/internals.md` "Web UI View Modules" paragraph updated; README had no stale refs.
- Reference snapshot of the original monolith retained at `copilot_working/views_original.py`
  (scratch dir, excluded from repo).
- No C-side changes needed: the C port already had this per-feature layout.
- [ ] Create `web/views_common.py` for cross-feature helpers (lights singleton, rename-refs
      family, _sounds_context, _color_name_to_hex, model-scoped dict helpers, etc.).
- [ ] Split views.py into per-feature modules mirroring the C port's layout
      (views_home/scenes/effects/filters/sounds/soundscapes/system/models/storage/colors...).
- [ ] Keep `web/views.py` as a thin aggregator re-exporting every View class + preserving the
      `views_named_ranges.initialize_named_range_views(...)` wiring, so `web/routes.py` needs
      no (or minimal) changes. Zero behavior change.
- [ ] Fold in the flagged convention fixes while moving: HomeView/ModelsSetView docstrings,
      AnimationView.post/SetSceneView.post `-> str` return types, SystemRebootConfirmView.post
      docstring.
- [ ] Drop `_soundscapes_context()`'s unused `home_only` parameter (verify unused first;
      `_sounds_context` also has one — only drop if verified unused).
- [ ] Check docs/ for stale references to the views.py structure.
- No C-side changes needed: the C port already has this per-feature layout.

### 3. lib/ convention cleanup — ALL DONE (done inline by orchestrator)
- [x] `lib/max7219.py`: type hints on every param/non-None return, docstring blank-lines,
      `__init__` docstring added. `spi`/`cs` params left untyped (MicroPython machine.SPI/Pin,
      not imported in that module — typing them would add an import for annotation only).
- [x] `lib/billboard.py`: `Billboard` class docstring added; type hints + docstring
      blank-lines on every method incl. `from_settings -> "Billboard"` and properties.
- [x] `lib/leds.py`: `LEDs` class docstring added.
- [x] `lib/webserver.py`: `WebServer` class docstring added.
- [x] `lib/utils.py`: `bytes_to_int` re-indented from 8-space to standard 4-space module
      level, `int` hints added.
- All verified with ast.parse. Zero behavior change, zero deletions.

### 4. Reset-cause logging — mostly DONE (done inline); ONE piece still blocked
- [x] `lib/utils.py`: added `reset_cause_name()` (maps machine.reset_cause() constants to
      "power-on"/"hard reset"/"watchdog"/"deep-sleep wake"/"soft reset", getattr-defensive).
- [x] `boot.py`: prints "Reset cause: <name>" right after "Booting...".
- [x] C: added `comms_reset_reason_name()` (`components/util/comms.c`/`.h`, ESP-IDF's richer
      set incl. brownout/panic); logged at top of `boot_init()` in `main/boot.c`; added
      `reset_cause` ctx key in `views_system.c`'s `view_status` (+ comms.h include; web
      component already REQUIRES util).
- [x] `templates/status.html`: added "Reset Cause" row to the System card (shared template —
      serves both Python and C builds; renders empty until the Python view provides the key).
- [x] `README.md`: Status Page section now mentions the reset-cause display.
- [x] Python StatusView: DONE — StatusView had already been relocated to
      `web/views_system.py` by the (interrupted) split agent, so the
      `"reset_cause": reset_cause_name()` context key + `from utils import
      reset_cause_name` import were added there directly. ast.parse-verified.
      Item 4 is now FULLY COMPLETE.

## Sequencing
Agents A (C parity), B (views split), C (lib conventions) run in parallel — disjoint file
sets (A: c_project only; B: web/ only; C: lib/ only). Item 4 runs AFTER B completes to avoid
colliding on StatusView. Orchestrator folds all results into this file; agents do not write
to any notes files.

## Results log
(appended as agents report)
