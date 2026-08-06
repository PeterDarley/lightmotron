# SPIFFS -> LittleFS migration — progress notes

**Status as of 2026-08-05: hardware testing IN PROGRESS.** Erase + reflash +
captive-portal reconnect attempted; hit and fixed one real bug (task stack
overflow, see Timeline). Not yet fully verified end-to-end (WiFi reconnect
+ settings restore + retest of the original crashing actions still
pending). This file exists so the migration can be picked back up mid-work
across sessions; update as remaining steps complete, and append to the
timeline rather than rewriting it.

## Known follow-up risk: task stacks sized for SPIFFS may be tight under LittleFS

LittleFS's write path (`lfs_dir_commit` -> `lfs_dir_relocatingcommit` ->
`lfs_dir_orphaningcommit` -> `lfs_bd_sync` -> `lfs_bd_flush` -> ... -> the
flash driver) is measurably deeper than SPIFFS's was — confirmed by a real
stack-canary crash in the captive portal's `http_task` (was 6144 bytes,
sized for SPIFFS-era needs; bumped to 16384, see Timeline). Any other task
that calls `persistent_dict_save()` and was sized under the SPIFFS
assumption is a candidate for the same issue:

- **`animation_task`** (`lighting/animation.c`, 8192 bytes) — already
  generous (was bumped earlier this session for an unrelated reason, a
  large `scene_metadata_t` local), probably fine, but hasn't been
  specifically re-verified against a LittleFS write happening on this task
  (e.g. via `trigger_scenes_on_completion` triggering a save somewhere in
  that path — check whether any lighting code path actually calls
  `persistent_dict_save()` before assuming this needs attention).
- **`client_task`** (`webserver.c`, `CLIENT_TASK_STACK_SIZE` = 12288,
  pooled up to `MAX_CLIENT_TASKS` = 6 concurrent) — the main webserver's
  per-request task, used for every settings-save endpoint (system
  settings, audio volume, restore, scene settings, ...). **Deliberately
  left unchanged** rather than guess-bumped: this exact constant was
  previously *cut down* from 20KB specifically because oversizing it
  caused "Failed to spawn client task" under load (internal DRAM
  fragmentation across `MAX_CLIENT_TASKS` concurrent stacks) — raising it
  blindly risks reintroducing that. There's already a proactive warning
  (`webserver.c:369-377`, "Client task stack low: ... raise
  CLIENT_TASK_STACK_SIZE") that fires when headroom drops under 2KB,
  *before* an actual canary-triggered crash. **Watch for that log line**
  when testing settings-saves through the normal web UI (not the captive
  portal) post-migration; if it appears, raise `CLIENT_TASK_STACK_SIZE`
  with real headroom data instead of guessing.

## Why

`c_project`'s two SPIFFS partitions ("webassets", "data") repeatedly
triggered `Interrupt wdt timeout on CPU1` panics during flash writes this
session (restore, system settings save, audio volume save), all traced to
`spiffs_object_find_object_index_header_by_name` taking longer than the
interrupt watchdog's budget. The watchdog timeout was bumped
300ms -> 800ms -> 3000ms as a stopgap. `asset_cache.h`'s own doc comment
(pre-dating this session) records an *earlier*, independently-discovered
instance of the same class of problem on `/setup`'s template reads — SPIFFS
having exactly this kind of GC-driven latency spike is not a one-off theory,
it's now been hit twice in this codebase's history. LittleFS (ESP-IDF's
now-recommended filesystem) doesn't have this unbounded-pause
characteristic — this migration is the durable fix.

## Scope

Both SPIFFS partitions migrated to LittleFS:
- **`webassets`** (`www/`, `templates/`) — low risk, rebuilt from source on
  every flash, no user data at stake.
- **`data`** (settings/scenes/effects/colors/sounds/named_ranges/...) — the
  partition that was actually crashing. Real user data lives here.

SPIFFS and LittleFS are **incompatible on-flash formats** — this was NOT an
in-place conversion. **The device's `data` partition must be erased and
reseeded, then settings restored from a backup JSON** (the existing
Backup/Restore feature). No firmware-side SPIFFS-to-LittleFS converter was
written; erase+restore is safer and far less code than a one-time migration
reader.

Component used: `joltwallet/littlefs` v1.22.3, fetched via the ESP-IDF
Component Manager (this project already used the component manager for
`espressif/cjson` and `espressif/mdns` — see `c_project/main/idf_component.yml`).

## Checklist

- [x] Add `joltwallet/littlefs` to `c_project/main/idf_component.yml`
      (`'*'`, resolved to 1.22.3)
- [x] `c_project/main/CMakeLists.txt`: `spiffs_create_partition_image` ->
      `littlefs_create_partition_image` for `webassets` (same
      `FLASH_IN_PROJECT` calling convention). Renamed the staging var/dir
      `SPIFFS_STAGING_DIR` -> `WEBASSETS_STAGING_DIR` for clarity.
- [x] `c_project/main/boot.c`: both mounts (webassets + data) now use
      `esp_vfs_littlefs_conf_t`/`esp_vfs_littlefs_register()`;
      `#include "esp_littlefs.h"`. Dropped `.max_files` (SPIFFS-only field,
      doesn't exist on `esp_vfs_littlefs_conf_t`). `base_path`/
      `partition_label` values unchanged.
- [x] `c_project/components/web/views_system.c`: `esp_spiffs_info("data", ...)`
      -> `esp_littlefs_info("data", ...)` (identical signature); local vars
      renamed `spiffs_*` -> `littlefs_*`; `#include <esp_spiffs.h>` ->
      `#include <esp_littlefs.h>`
- [x] `c_project/components/storage/CMakeLists.txt`: `REQUIRES spiffs` ->
      `REQUIRES joltwallet__littlefs` (this is how `views_system.c` gets
      `esp_littlefs.h` transitively: web -> storage -> joltwallet__littlefs;
      `storage`'s own .c files don't call filesystem-specific APIs directly,
      they're plain POSIX file I/O)
- [x] **Found and fixed a real functional bug during the grep sweep, not
      just a naming issue**: `components/webserver/asset_cache.c`'s
      `asset_cache_init()` explicitly skipped `DT_DIR` entries when walking
      the mount root, because SPIFFS has no real directories — a file at
      `templates/home.html` was one flat dirent with that literal name.
      LittleFS has genuine nested directories: `templates` and `www` are
      real `DT_DIR` entries one level down from the mount root, and the old
      code would have skipped both entirely, caching **zero assets** and
      breaking every page. Rewrote as a proper recursive `scan_directory()`.
      Also added a loud `ESP_LOGE` if the cache ends up empty after init, so
      a regression here fails obviously instead of silently 404ing/500ing
      everything.
- [x] Checked `components/webserver/static_files.c` and
      `components/webserver/template_engine.c` for the same flat-vs-nested
      assumption — both build full paths and `fopen()`/`stat()` them
      directly (no `opendir`/`readdir`), which works identically under
      either filesystem. No changes needed.
- [x] Checked `components/web/views_system.c`'s `list_theme_files()`
      (`opendir(THEMES_DIR)`, lists `.css` files under `www/themes`) — this
      is a non-recursive listing of files *within* an already-fully-
      qualified leaf directory, not the mount root, so it's correct under
      both filesystems as-is. No change needed.
- [x] `partitions.csv`: offsets/sizes unchanged. Subtype changed
      `spiffs` -> `littlefs` for both partitions — checked
      `gen_esp32part.py` directly, `littlefs` (0x83) is a real recognized
      subtype in this ESP-IDF version, not a guess/hack.
- [x] Updated stale "SPIFFS" wording in doc comments for accuracy:
      `main/boot.h`, `components/storage/include/persistent_dict.h`,
      `components/storage/include/json_helpers.h`,
      `components/webserver/include/asset_cache.h`. Left the `/spiffs`
      *mount path string* itself unchanged everywhere (`STORAGE_MOUNT_POINT`,
      `TEMPLATES_DIR`, `WWW_DIR`, `ASSET_MOUNT`, `THEMES_DIR`, etc.) — it's
      just a VFS mount-point name at this point, not an actual SPIFFS
      reference, and renaming it would touch many files for zero functional
      benefit.
- [x] `.github/copilot-instructions.md` — grepped, no SPIFFS mentions
      there, nothing to update.
- [x] Build clean: `idf.py build` succeeded, 1129/1129, no warnings from
      any of the above changes. `webassets.bin` generated at exactly
      0x200000 bytes (matches partition size) confirming
      `littlefs-python`'s auto-provisioned venv (part of the joltwallet
      component's CMake macro) worked correctly on this Windows host.
- [x] Erase + reflash executed. Device boots, mounts both LittleFS
      partitions fine (`webassets LittleFS mounted` / `data LittleFS
      mounted` in log), seeds defaults, brings up the captive portal
      correctly (WiFi scan found 12 SSIDs, AP/DNS/HTTP all started).
- [x] Found and fixed a second real bug during this first hardware test:
      captive portal's `http_task` stack (6144 bytes) overflowed the first
      time it actually exercised a full settings write
      (`save_wifi_credentials()` -> LittleFS's deeper write path) —
      see "Known follow-up risk" section above and Timeline. Fixed,
      rebuilt, not yet reflashed/retested.
- [ ] **Not yet done**: reflash with the `http_task` stack fix, retry
      entering WiFi credentials via the captive portal, confirm it
      actually connects and boots normally this time.
- [~] User reported a follow-up symptom on a subsequent captive-portal
      attempt: "no response" when submitting the WiFi password form (no
      crash dump given this time). Added per-request logging to
      `http_task` (client IP/method/path/status/elapsed-ms on every
      request, plus two checkpoint logs bracketing
      `save_wifi_credentials()`) to localize this precisely on the next
      attempt, rather than guessing — nothing in the current code shows an
      obvious hang (every branch reaches a `send()`), so this needs real
      data. **Not yet root-caused.**
- [x] Found and fixed a third bug during this hardware test, unrelated to
      LittleFS itself: `view_restore()` (`views_storage.c`) would overwrite
      the device's current WiFi credentials with whatever (if anything)
      the uploaded backup file had for `"wifi"` — including the placeholder
      `"password": "***"` still present in the backup file used earlier
      this session. `view_backup()` already strips `"wifi"` on export for
      exactly this reason but `view_restore()` had no matching guard on
      import. Fixed: current WiFi config is captured before
      `replace_all_keys()` and force-restored after, so a restore can
      never touch WiFi regardless of what's in the uploaded file. Rebuilt
      clean, not yet reflashed/retested.
- [ ] Restore settings from backup JSON once WiFi reconnect succeeds
      (WiFi should now survive the restore untouched).
- [ ] Retest the originally-crashing actions (restore upload, system
      settings save, audio volume save) through the normal web UI (not the
      captive portal) to confirm the interrupt-watchdog crashes are gone.
      **Watch for the "Client task stack low" warning** (see "Known
      follow-up risk" above) during this step specifically.

## Deployment steps for the user (not yet executed)

This is a **destructive** step (wipes the `data` partition — settings,
scenes, effects, WiFi credentials, everything) and needs explicit
confirmation before running, per this session's standing git/destructive-
action policy. Sequence:

1. **Get a fresh backup first**, if the current on-device settings aren't
   already captured in a backup JSON on this machine — Status page ->
   Backup & Restore -> Download Backup. (The last backup JSON used earlier
   this session had `"password": "***"` redacted in it — real WiFi
   credentials will need re-entering via the captive portal after erase
   regardless, this isn't new/specific to this migration.)
2. `flash_c.ps1 -Erase` — full chip erase (not just `data`; simplest given
   the partition *subtype* changed too, so the partition table entry itself
   is different from what's currently on the chip) then full flash of this
   build.
3. Device boots, seeds fresh defaults into the new LittleFS `data`
   partition, comes up with `active_on_boot` scenes / default WiFi (none
   configured) -> captive portal.
4. Reconnect WiFi via the captive portal (real credentials).
5. Restore settings from the backup JSON via Status page -> Backup & Restore
   -> Restore.
6. Confirm the previously-crashing actions now work: another restore
   upload, a system settings save, an audio volume change. If any of these
   still panic, that's a real regression in this migration to debug — not
   an interrupt-watchdog/SPIFFS-GC issue anymore, so a fresh crash dump
   would point somewhere new.

## Timeline

- **2026-08-05**: User approved migration ("Let's go ahead and move to
  littlefs. Keep notes so you can pick up if I run out of tokens."). Full
  implementation completed in one session: component added, both mounts
  migrated, storage-stats call migrated, partition table subtypes updated
  to the real recognized `littlefs` value, a real recursive-directory-walk
  bug found and fixed in `asset_cache.c` (would have silently broken every
  page had it shipped), stale SPIFFS doc comments updated, build verified
  clean.
- **2026-08-05 (later same day)**: User erased + reflashed. Boot/mount/
  captive-portal-start all worked correctly. Entering WiFi credentials via
  the captive portal panicked: `Stack canary watchpoint triggered
  (portal_http)`, backtrace through `save_wifi_credentials()` ->
  `persistent_dict_save()` -> `json_write_file()` -> deep into LittleFS's
  commit protocol (`lfs_dir_commit`/`lfs_dir_relocatingcommit`/
  `lfs_bd_sync`/...) -> the flash driver. Root cause: `http_task`'s stack
  (6144 bytes, `captive_portal.c`) was sized for SPIFFS's shallower write
  path and never got exercised against a real LittleFS write until this
  first hardware test (the erase+reflash test plan's step 4, WiFi
  reconnect, is literally the first time this codebase ever calls
  `persistent_dict_save()` from this specific task). Bumped to 16384,
  rebuilt clean. Also audited `client_task`/`animation_task` for the same
  risk class (see "Known follow-up risk" section) — deliberately left
  `client_task` unchanged pending real evidence (it has its own proactive
  low-stack warning already) rather than guess-bumping and risking
  reintroducing a previously-fixed fragmentation problem. Not yet
  reflashed with this fix; WiFi reconnect + settings restore + full
  regression retest still pending.
- **2026-08-05 (later still)**: after 5 rounds of interrupt-watchdog
  timeout bumps (300->800->3000->5000->8000ms) and LittleFS
  lookahead/cache tuning — including a fresh full erase — still didn't
  stop the panics, added timing-checkpoint logging to `json_write_file()`
  to gather real data before guessing further. Before that data came back,
  user asked directly: "Is it possible that the crashes while saving the
  .json file are because we're writing to a different file name then
  renaming it?" Investigated and concluded **yes, very likely a real
  contributor**: every panic in this investigation has landed in
  `lfs_dir_splittingcompact`, which only runs during a directory-metadata
  commit, and the old tmp-file+rename pattern
  (`fopen(file.tmp)`+close, `remove(file)`, `rename(file.tmp, file)`) did
  **3 separate commits per save** instead of 1 — tripling the odds of
  hitting the expensive path on any given save. Confirmed via LittleFS's
  own `lfs.h`/`DESIGN.md` that file writes are already committed
  atomically on sync/close (power-loss mid-write reverts to the previous
  committed state, doesn't leave a torn file) — the app-level tmp+rename
  dance was providing a guarantee LittleFS already gives natively on this
  filesystem (it's a carryover from the SPIFFS era, which didn't have that
  guarantee). Simplified `json_write_file()`
  (`components/storage/json_helpers.c`) to write directly to the target
  path — no `.tmp`, no `remove()`, no `rename()` — keeping the timing
  checkpoints (fewer of them now) in case a single commit can still stall
  on its own. Build verified clean. **Not yet reflashed/retested** — this
  is now the leading fix candidate for the recurring theme-save crash;
  next step is reflash and retry the theme-change action that's been
  reproducing it.
- Also implemented (unrelated, same session): hostname display in the
  title/navbar now replaces `-` with a space (e.g. "event-horizon" ->
  "EVENT HORIZON"), in `components/web/context_processors.c`'s
  `add_uppercase_string()` helper. No effect on the stored hostname value
  itself, or on the underscore-not-allowed validation in
  `views_system.c`'s `hostname_chars_valid()`.
- **2026-08-05 (later still)**: user asked "Can we force the maintenance
  on the filesystem on boot?". LittleFS itself has exactly this —
  `lfs_fs_gc()` (found in the vendored
  `managed_components/joltwallet__littlefs/src/littlefs/lfs.h`), whose own
  doc comment says it exists to "allow the offloading of expensive
  janitorial work to a less time-critical code path" — literally this
  request. Problem: `esp_littlefs` (the ESP-IDF wrapper) doesn't expose
  it; the `lfs_t*` handle is kept behind a private, file-static `_efs[]`
  registry (`src/esp_littlefs.c`) with no public accessor. Added a thin
  wrapper to the **vendored component itself**:
  `esp_err_t esp_littlefs_gc(const char *partition_label)` in
  `include/esp_littlefs.h` + `src/esp_littlefs.c`, following the exact
  lock pattern (`sem_take`/`sem_give`) already used by neighboring calls
  like `esp_littlefs_info()`. Wired into `main/boot.c` right after both
  `data` and `webassets` mount, before anything else touches either
  partition (`boot.c:294` area) — timed with `ESP_LOGI` so the boot log
  shows how long GC took on each partition. Not fatal if `esp_littlefs_gc`
  fails — falls back to today's behavior (debt paid lazily on next
  write).
  **Caveat worth watching for on the next boot**: `lfs_fs_gc()` does the
  *same* `lfs_dir_splittingcompact`-class work that's been tripping the
  interrupt watchdog mid-write — running it at boot doesn't make that work
  cheaper, just moves *when* it happens. If a lot of compaction debt has
  accumulated (plausible, given many crash-mid-write cycles this session),
  it's plausible boot itself now stalls long enough to trip
  `CONFIG_ESP_INT_WDT_TIMEOUT_MS` (currently 8000ms) instead of a later
  settings save. That would still be diagnostically useful (an
  immediately-reproducible boot-time crash beats an intermittent
  mid-request one) but is a real possibility to watch for, not a
  guaranteed fix by itself.
  **Vendored-component-modification caveat**: this change lives in
  `managed_components/joltwallet__littlefs`, which the ESP-IDF Component
  Manager treats as a fetched dependency, not project source — if that
  directory is ever deleted and re-resolved (e.g. `idf.py fullclean`,
  a fresh clone without this folder committed, or the pinned lockfile
  version changing), **this edit will be silently lost** and need
  reapplying. Worth committing `managed_components/` for this project
  specifically, or at minimum noting this as a required manual step after
  any clean re-fetch.
  **Not yet reflashed/tested.**
