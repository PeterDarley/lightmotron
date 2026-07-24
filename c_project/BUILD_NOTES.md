# C Project Build Shakedown Notes

Status as of first successful `idf.py build`: **BUILD SUCCEEDS.**
`lightmotron.bin` = 0x121cf0 bytes (~1.14MB), 62% of the 0x300000 app
partition free.

This file records what it took to get the never-before-compiled
`c_project/` (ESP-IDF v6.0.2, target esp32s3) to build, so the fixes
aren't rediscovered from scratch if something regresses.

## Environment

- Build **only** from a plain Windows Terminal/PowerShell window that is
  NOT a VS Code integrated terminal. VS Code's Python extension
  auto-activates this repo's `venv`, and the ESP-IDF profile script's own
  venv activation stacks on top of it, corrupting `PATH` (observed at
  ~1247 chars instead of the expected 3000-5000+), which breaks
  `cmake`/`idf.py` resolution.
- Similarly, do **not** shell out to `powershell.exe` from a git-bash /
  MSYS shell to run `idf.py`. MSYS's environment (`MSYSTEM` etc.) leaks
  into the child PowerShell process and makes the ESP-IDF profile
  script's own Mingw/MSys detection treat its normal informational
  warning as a fatal `NativeCommandError`, aborting before the build
  even starts. Run `idf.py` from native PowerShell (or the plain
  terminal above) directly.
- Activation: dot-source
  `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1`, then
  `cd C:\Development\lightmotron\c_project`, then `idf.py build`.

## Component Manager / CMakeLists REQUIRES fixes

ESP-IDF v6.0.2 split several things out of the old monolithic components
that this project's `CMakeLists.txt` files assumed were bundled. Each of
these needed an explicit `REQUIRES` entry added to the component that
directly uses the header (transitive `REQUIRES` does NOT cascade further
than one hop for these — each direct consumer needs its own entry):

| Component | Missing header | Fix |
|---|---|---|
| `storage` | `cJSON.h` | `REQUIRES spiffs espressif__cjson` (added `main/idf_component.yml` dependency `espressif/cjson: ^1.7.19` via `idf.py add-dependency`) |
| `audio` | `driver/uart.h` | added `esp_driver_uart` |
| `billboard` | `driver/spi_master.h`, `driver/gpio.h` | added `esp_driver_spi esp_driver_gpio` |
| `leds` | `driver/rmt_tx.h`, `driver/gpio.h` | added `esp_driver_rmt esp_driver_gpio` |
| `util` | `driver/gpio.h`, `esp_timer.h` | added `esp_driver_gpio esp_timer` |
| `network` | `esp_ota_ops.h`, mDNS | added `app_update`; mDNS via `espressif/mdns` component-manager dep |
| `webserver` | `IPSTR`/`IP2STR` macros | added `esp_netif` to REQUIRES + `#include "esp_netif.h"` in `webserver.c` |
| `main` | `nvs_flash.h` (boot.c calls nvs init) | added `nvs_flash` |

## Source fixes

- `components/storage/include/json_helpers.h` — missing `#include
  "esp_err.h"` for `esp_err_t` in a declaration.
- `components/webserver/template_engine.c` — `process_for_loops` was
  called before its definition and missing from the file's existing
  forward-declaration block; added it.
- `components/lighting/patterns.c` — 3x `-Werror=misleading-indentation`
  from single-line `if (...) x = lo; if (...) x = hi;` pairs; split onto
  separate lines (formatting only, no logic change).

## Regression repair (self-inflicted, same session)

An earlier regex-based bulk edit (dedup'ing `form_field_last()` across
`components/web/*.c`) had a DOTALL non-greedy leading-comment-match group
that catastrophically backtracked and deleted far more than intended in
exactly 3 files: `views_filters.c`, `views_scenes.c`, `views_effects.c`.
Lost content (file header comments, `#include` blocks, and helpers
`standard_color_rgb`/`hex_from_rgb`/`color_name_to_hex`/`cmp_str`/
`is_hex6`, plus `views_filters.c`'s `FILTER_METADATA` table) was
reconstructed from conversation memory (first two files) and by
cross-referencing compiler errors + surviving call-sites + the Python
source `web/views_effects.py` (third file, `views_effects.c`). Verified
by a custom brace-balance tokenizer (naive `grep -c '{'` gives false
positives on the template engine's own `"{%"/"{{"` string literals).

Two helpers were still missing from the `views_effects.c` reconstruction
and only surfaced once the file actually compiled against its callers:
`is_hex_digits(const char *, size_t)` and `sorted_keys_array(cJSON *)`
(the latter copied verbatim from the equivalent function already present
in `views_scenes.c`).

## DRAM overflow (the big one)

Final link error: `.dram0.bss` overflowed internal DRAM by 159248 bytes.
Root cause: `components/lighting/lighting.c` had
`static active_scene_t active_scenes[MAX_ACTIVE_SCENES];` as a plain
static/BSS array. `active_job_t` embeds `int
target_indices[MAX_LEDS]` (300 ints = 1200 bytes) per job, and with
`MAX_JOBS_PER_SCENE=16` and `MAX_ACTIVE_SCENES=8` that one array alone
was 324160 bytes — far more than internal DRAM can hold.

Fix: since the board (YD-ESP32-S3 N16R8) has 8MB PSRAM and
`CONFIG_SPIRAM_USE_MALLOC=y` with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`,
any heap allocation over 4KB is automatically placed in external PSRAM by
the standard allocator — no menuconfig changes needed. Changed
`active_scenes` to `static active_scene_t *active_scenes = NULL;` and
`calloc()`'d it once in `lighting_init()` (with an `ESP_ERR_NO_MEM`
failure path). Capacity (`MAX_ACTIVE_SCENES`, `MAX_JOBS_PER_SCENE`,
`MAX_LEDS`) is unchanged — this is a storage-location change only, not a
behavior change.

## SPIFFS web-assets image, and the two-partition split

`main/CMakeLists.txt` stages `../../www` and `../../templates`
(repo-root, shared with the Python build) into
`${CMAKE_CURRENT_BINARY_DIR}/spiffs_staging` at configure time and calls
`spiffs_create_partition_image(webassets ... FLASH_IN_PROJECT)`.

Originally this targeted a single `storage` partition that *also* held
`/spiffs/data/*.json` (settings/scenes/effects/colors/sounds/WiFi
credentials). That was a real problem: `FLASH_IN_PROJECT` bundles the
generated image into the *default* `idf.py flash` target, and flashing a
partition is a full overwrite, not a merge — every code or asset deploy
was silently wiping all persistent user data, since the freshly-built
image only ever contains `www/`+`templates/` (nothing under `data/` is
pre-staged; see below).

Fixed by splitting into two SPIFFS partitions (`partitions.csv`):
- `webassets` (2MB, `0x610000`–`0x810000`) — `www/`+`templates/` only,
  mounted at `/spiffs`, rebuilt and rewritten on every default flash.
- `data` (~7.9MB, `0x810000`–`0x1000000`) — settings JSON, mounted at
  `/data` (new `DATA_MOUNT_POINT` in `main/settings.h`), deliberately
  **not** part of the default flash target. `STORAGE_SYSTEM_SETTINGS_FILE`
  / `STORAGE_LIGHTING_SETTINGS_FILE` / `STORAGE_SOUNDS_FILE`
  (`components/storage/include/persistent_dict.h`) moved from
  `/spiffs/data/*.json` to `/data/*.json` accordingly. `main/boot.c` now
  calls `esp_vfs_spiffs_register()` twice, once per partition.
  `esp_spiffs_info()` in the Status page's storage-usage card
  (`components/web/views_system.c`) was repointed from the now-gone
  `"storage"` partition label to `"data"` specifically, since that's the
  one whose free space users actually care about.

`persistent_dict_open()` doesn't need `data/*.json` pre-staged —  it
creates those files on first write, same as `lib/storage.py` (confirmed
by reading `ensure_loaded()` in `components/storage/persistent_dict.c`,
which falls back to an empty object when a file is missing/invalid).

**`flash_c.ps1` now defaults to `idf.py app-flash`** (app binary only —
touches neither SPIFFS partition) instead of `idf.py flash`. Pass `-Full`
for bootloader+partition-table+app+webassets, needed when `www/`/
`templates/` changed, for a first-ever flash of a blank board, or after a
partition-table change. Note that a *partition-table* change specifically
(not just an ordinary `-Full` flash) can still invalidate whatever's
physically sitting in the `data` region, since SPIFFS's internal layout
isn't a stable byte-for-byte mapping — back up via the Status page's
"Download Backup" first if the device already has real data on it before
adopting a partition-table change like this one.

Caveat (both here and before this split): the `www/`/`templates/` copy
into `spiffs_staging` happens at CMake *configure* time, not build time.
After editing files under those directories, run `idf.py reconfigure`
(or touch `main/CMakeLists.txt`) before building so the staged copy
picks up the changes.

Hit one SPIFFS limitation along the way: classic SPIFFS's default
`CONFIG_SPIFFS_OBJ_NAME_LEN=32` counts the *full path* as the object
name, and several nested template paths exceed that (longest:
`/templates/scenes/select_scene_button_immediate.html` at 52 chars).
Bumped it to 64 in both `sdkconfig.defaults` (so a fresh
`idf.py set-target`/`reconfigure` picks it up) and the already-generated
`sdkconfig` (so it takes effect without a full reconfigure).

## Setup page: missing summary-card data on initial load

`view_setup()` (`components/web/views_system.c`, GET `/setup`) originally
built only a minimal context (theme/hostname/current model). Every other
setup card's data — System Settings, Custom Colors, Filters, Effects,
Scenes, Sounds, Soundscapes — comes from context keys that only the
individual `/X/summary` htmx endpoints populated. Those endpoints are
correct in isolation, but `templates/setup.html`'s summary `<div>`s use
bare `hx-get` with no `hx-trigger`, so htmx never fires them
automatically on page load — they only refresh via the explicit
`htmx.ajax(...)` calls that fire after closing a "Manage X" modal. So on
first load (and for System Settings/Models specifically, which aren't
even in that modal-close refresh list, *forever*), every card fell back
to its empty state.

Python's reference (`web/views_system.py`'s `SetupView.get()`)
deliberately avoids this: it builds one fully-populated context up front
— *"Include all summary card data so the browser receives everything in
one request rather than firing 5+ lazy HTMX loads after page paint."*
The C port never got that treatment. Fixed by exposing each section's
existing (already-correct) context-building logic as reusable functions
— `add_scenes_summary_context`, `add_effects_summary_context`,
`add_filters_summary_context`, `add_named_ranges_summary_context` (new),
`add_sounds_context` (un-static'd) — following the pattern
`add_soundscapes_context` already established (declared in
`components/web/include/views.h`, already reused by `view_home()`).
`view_setup()` now calls all of them. Deliberately does *not* trigger a
live GitHub update check for the Updates card (matching Python's use of
a cached last-check result rather than hitting the network on every page
load) — pending-update count just defaults to 0 until explicitly checked.

Also fixed while in there: the `www/styles/app.css` `.dashboard-card-grid`
/ `.setup-card-grid` layout used fixed `repeat(4, 1fr)` columns at
`min-width: 1200px`, which — combined with the LCARS theme's fixed
`~7rem` label-strip overhead per card — left too little room for long
unbroken values (hostnames, SSIDs) and forced them to wrap
character-by-character. Replaced with
`repeat(auto-fit, minmax(min(320px, 100%), 1fr))` on both classes so the
grid picks its own column count instead of forcing 4 regardless of
available width. Shared CSS file — fixes both the C and Python builds at
once.

## Still to do

- Not yet flashed to real hardware with the current partition layout —
  build success only confirmed so far. The board currently has data
  flashed under the *old* single-`storage`-partition scheme; adopting
  this partition table change requires a full wipe/reflash (back up via
  Status page's "Download Backup" first, restore after — note the backup
  doesn't cover WiFi credentials or sounds/soundscapes, so those need
  re-entering by hand).
- The modal-close JS refresh list in `templates/setup.html` still doesn't
  include System Settings or Models, so after editing those two
  specifically the card won't visually update until a full page reload
  (separate, smaller issue from the initial-load one above — noted but
  not fixed).
