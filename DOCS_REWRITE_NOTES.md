# Documentation rewrite — progress notes

**Status as of 2026-08-06: COMPLETE.** This file exists so the project can
be picked back up across sessions per the user's explicit request ("keep
notes in case this gets interrupted"). Update the checklist as steps
complete; append to history rather than rewriting it.

## The ask (verbatim intent)

- Remove documentation about the Python system, other than a brief mention
  that it exists (historical note only — Python/MicroPython side is frozen,
  `c_project` is the only actively developed version, per
  `.github/copilot-instructions.md`'s existing note).
- `README.md` should focus on information meaningful to an end user who is
  **not a developer**, and should **lead with the web installer**.
- A **"programming" page** describing how to set up effects, sounds, etc.
  in the system (end-user configuration reference — patterns, filters,
  colors, scenes, sounds — not source code).
- A **central developer page** that talks about internals and links out to
  more detailed development pages (architecture hub, not a single giant
  file).

## Survey of existing docs (done)

Read/assessed everything under version control that could count as
"documentation" (excluded vendored library docs under `managed_components/`
and `venv/`):

| File | Assessment |
|---|---|
| `README.md` (270 lines) | Heavy mix: MicroPython git-submodule setup, `upload.ps1`/mpremote instructions, and MicroPython syntax caveats (all dev-only, Python-only) interleaved with genuinely end-user content (WiFi setup, web UI walkthrough, effects/filters tables, web installer). Needs a full rewrite/split. |
| `docs/internals.md` (565 lines) | Top ~180 lines are Python-implementation-specific (`web/views_*.py` module map, `lib/lighting/` layout, Python-flavored OTA details, `_poll_tick()` internals). Remaining ~380 lines (Target Specification, Colors table, full Patterns reference, full Filters reference) are pure JSON-schema/behavior docs — language-agnostic, equally true of the C build, and exactly the kind of content the requested "programming" page needs. |
| `docs/theming.md` (188 lines) | CSS/template theming system. Fully shared between builds (`templates/`, `www/` are shared assets per copilot-instructions.md). No Python-specific content. Keep essentially as-is. |
| `docs/neopixel-wiring.md` (77 lines) | Pure hardware wiring diagram (mermaid). Language-agnostic. Keep as-is. |
| `docs/settings_template.py` (214 lines) | JSON storage-schema reference. Schema itself is shared/valid for both builds, but prose references `PersistentDict (lib/storage.py)` (Python-only module). Needs light wording touch-up only, not a rewrite. |
| `lib/README.md` | Documents the **shared MicroPython submodule** (`lib/`), used by other projects too. Out of scope — not this repo's own doc surface, and the standing instruction says don't touch `lib/` for lightmotron-specific reasons. |
| `.github/copilot-instructions.md` | AI-agent operating instructions (append-only "note" file), not user/developer documentation. Contains some now-stale Python-only coding-convention notes (type hints, docstrings, `lib/webserver.py` templating, etc.) but pruning it wasn't asked for and it has its own user-curated-only convention ("single source-of-truth for notes the user asks Copilot to note"). **Leaving untouched** — flagging as a possible separate follow-up, not part of this project. |
| `c_project/c_plan.md` (456 lines), `c_project/BUILD_NOTES.md`, `c_project/SYNC_NOTES.md` | Historical working records from the Python→C port itself (porting checklist, build shakedown log, sync log). Not currently presented as live documentation anywhere. Leaving as historical archive, undisturbed — may reference/pull small facts (e.g. directory layout) into the new developer page, but won't rewrite these files themselves. |
| Root `*_NOTES.md` (REFACTOR_NOTES.md, FOLLOWUP_NOTES.md, MDNS_NOTES.md, AUDIO_SYNC_NOTES.md, VOICE_CONTROL_NOTES.md, LITTLEFS_MIGRATION_NOTES.md, AUDIO_UART_CRASH_NOTES.md) | Session/investigation working notes (this project's own established pattern for cross-session continuity), not polished documentation. Out of scope for this project — explicitly excluded, not forgotten. |

## Target structure (decided)

**New files:**
- `docs/programming.md` — end-user configuration reference. Everything about
  *using* the system: key concepts (Scene/Effect/Filter/Named Range/Custom
  Color/Model), the Setup-page walkthrough, full Patterns reference, full
  Filters reference, Colors table, Target Specification, Sounds/Soundscapes
  behavior, Scene-level behavior (kills/triggers/stop lists). This is where
  the current README's "Lighting System"/"Effects Overview"/"Filters
  Overview" sections move to, merged with `docs/internals.md`'s
  language-agnostic back half (Patterns/Filters/Colors/Target Spec).
- `docs/developer.md` — central developer hub. Short architecture overview,
  the one-paragraph Python-history mention, repo/component layout, and a
  links-out list to: `docs/building.md`, `docs/internals.md` (rewritten),
  `docs/theming.md`, `docs/hardware.md`, `docs/settings_template.py`.
- `docs/building.md` — dev environment setup + build/flash instructions,
  moved out of README (idf.py/ESP-IDF setup, `build_c_release.ps1`,
  `build_web_install.ps1`, `flash_c.ps1`; pulls the still-relevant
  environment gotchas from `c_project/BUILD_NOTES.md`).
- `docs/hardware.md` — audio + MAX7219 billboard wiring reference (pin
  tables currently only exist in `.github/copilot-instructions.md`, not in
  any user-facing doc). Links to `docs/neopixel-wiring.md` for LED wiring
  rather than duplicating it.

**Rewritten in place:**
- `README.md` — full rewrite. End-user only. Order: what it is → **web
  installer (lead)** → first-boot WiFi setup → web UI tour (brief, links to
  programming.md for configuration depth) → hardware requirements (brief,
  links to docs/hardware.md + neopixel-wiring.md) → one-paragraph history
  note (MicroPython origins, now a C rewrite) → license → links to
  programming.md / developer.md. No build instructions, no git/mpremote/
  upload.ps1 content — all of that moves to docs/building.md.
- `docs/internals.md` — strip to pure C-implementation/architecture
  reference (component responsibilities, request flow, storage format
  mechanics, OTA mechanism). Remove the Python module map and the
  Patterns/Filters/Colors/Target-Spec tables (moved to programming.md,
  not duplicated). Links to programming.md for the parameter reference
  instead of repeating it.
- `docs/settings_template.py` — swap `PersistentDict (lib/storage.py)` and
  similar Python-only references for implementation-neutral wording; schema
  content itself is unchanged.

**Untouched (see survey table for why):** `docs/theming.md`,
`docs/neopixel-wiring.md`, `lib/README.md`, `.github/copilot-instructions.md`,
`c_project/c_plan.md`, `c_project/BUILD_NOTES.md`, `c_project/SYNC_NOTES.md`,
all root `*_NOTES.md` files.

## Checklist

- [x] Survey existing docs, decide target structure (this file)
- [x] Write `docs/programming.md`
- [x] Write `docs/hardware.md`
- [x] Write `docs/building.md`
- [x] Rewrite `docs/internals.md` (stripped Python specifics, removed
      duplicated pattern/filter/color tables — now links to
      programming.md for those instead)
- [x] Write `docs/developer.md`
- [x] Rewrite `README.md` (end-user only, leads with the web installer,
      one-paragraph History section is the only Python mention)
- [x] Touch up `docs/settings_template.py` wording (two Python-module
      references neutralized; schema content unchanged)
- [x] Final pass: grepped the new/rewritten doc set for stray
      `mpremote`/`upload.ps1`/`git submodule`/`lib/lighting`/`web/views`
      references (none found outside the one intentional internals.md
      history note explaining why OTA is architecturally different) and
      verified every relative link/screenshot reference resolves to a
      real file.

## Notable finding during the internals.md rewrite

The C build's OTA mechanism (`c_project/components/network/ota_update.c`)
is **not** a straight port of the Python updater — it's architecturally
different, and the old README/internals.md text describing "compare local
files... changed file list... apply updates" was describing the *Python*
mechanism (git-tree file diffing, appropriate for interpreted `.py`
source) which does not apply to the C firmware at all. The C build instead
checks the GitHub Releases API for a `.bin` asset and flashes it whole via
`esp_https_ota()` — confirmed by reading the actual source (which has its
own doc comment explaining this exact divergence) before writing the new
docs, rather than assuming the old description still applied. Both
`README.md`'s "Firmware Updates" section and `docs/internals.md`'s "OTA
Updates" section now describe the real (release-download) mechanism.

## History

- **2026-08-06**: User requested this project. Surveyed all in-repo
  documentation, decided target structure, wrote this notes file. Then
  wrote `docs/programming.md`, `docs/hardware.md`, `docs/building.md`,
  rewrote `docs/internals.md`, wrote `docs/developer.md`, rewrote
  `README.md`, touched up `docs/settings_template.py`, and did a final
  grep/link-check pass. All in one session, no interruption needed after
  all — this file is kept for reference/history rather than active
  resumption.
