# Audio/UART crash investigation + change log

**RESOLVED (root cause found 2026-07-24):** the crash was NOT a general
code bug. Flashing the exact same firmware to a **fresh** ESP32-S3 did not
crash (twice). The original board carried **persistent on-device state that
a full flash deliberately does not erase**, and that state is what
triggered the crash. Details in "Root cause" below.

The changes described in "Independent fixes" have since been **re-applied**
(the user had reverted them along with a failed crash-fix attempt; only the
failed attempt should stay gone). A `-Erase` option was added to
`flash_c.ps1` as the remedy for the persistent-state problem.

## Root cause: persistent `data` partition state survives full flashes

`flash_c.ps1 -Full` runs `idf.py flash`, which writes only bootloader,
partition table, app, and the `webassets` SPIFFS image. It intentionally
does **not** write the `data` partition (0x810000, holds
settings/scenes/sounds/etc.) or `nvs` (0x9000) -- that's the whole point of
the earlier partition split, so ordinary deploys don't wipe user settings.
The side effect: anything persisted in `data`/`nvs` survives every reflash,
including `-Full`.

The original board had originally run the **old single-`storage`-partition
layout** (one big SPIFFS spanning 0x610000-0xFFFFFF). When that was split
into `webassets` (0x610000) + `data` (0x810000), the new `data` partition
began overlaying flash that previously held old SPIFFS content. On top of
that, its `system_settings.json` had a populated `audio_players` entry
(boot log: "Audio module 0 ready (UART1, TX=6, RX=5)"). A **fresh** board
seeds `audio_players` as an empty array (`main/boot.c` boot_seed_defaults),
so it never initializes the audio UART at all -- which is exactly why fresh
boards don't crash.

So the chain was: persisted `audio_players` config -> audio YX5200 UART
driver initializes -> `ip_announcement` (and/or audio poll) generates UART
TX interrupt traffic around boot/first-request -> that collides with the
web server's SPIFFS template reads (which disable the flash cache on both
cores) -> cross-core deadlock -> interrupt watchdog reset. A `-Full` flash
never cleared it because `data` isn't in the flash target.

**Remedy:** erase the whole chip once to clear the stale `data`/`nvs`
state, then flash fresh. `flash_c.ps1 -Erase` now does this
(`idf.py erase-flash` first; the firmware re-seeds fresh defaults on next
boot). One-off command equivalent:
`idf.py -p COMx erase-flash` then a normal `-Full` flash.

## Is the deadlock actually still a risk with the default config? No.

Re-analyzed 2026-07-24: the UART-vs-SPIFFS deadlock **cannot occur with the
current (default, non-IRAM) UART interrupt config**, and this is verifiable
from the mechanism, not just observation:

- Every SPIFFS read goes through
  `spi_flash_disable_interrupts_caches_and_other_cpu()`, which calls
  `esp_intr_noniram_disable()`.
- `esp_intr_noniram_disable()` disables every interrupt NOT flagged
  `ESP_INTR_FLAG_IRAM` for the duration of the flash op.
- The YX5200 UART interrupt is installed with `flags = 0`
  (`components/audio/yx5200.c` `uart_driver_install(..., 0)`) -> non-IRAM
  -> masked during every flash window. It literally cannot run concurrently
  with the SPIFFS read; it runs microseconds later, after the read
  completes. No concurrency -> no cross-core spinlock contention -> no
  deadlock.

The deadlock traces we captured all *required* the UART ISR to run during
the flash window, which is only possible when the ISR is IRAM-exempt from
that mask -- i.e. exactly what the (now-reverted) `ESP_INTR_FLAG_IRAM` +
`CONFIG_UART_ISR_IN_IRAM` changes did. With those gone, the trace is
impossible to produce.

Verified there is no `ESP_INTR_FLAG_IRAM` anywhere in `components/`/`main/`
and no `UART_ISR_IN_IRAM` enabled (the only `*_ISR_IN_IRAM` flags on are
`SPI_MASTER`/`SPI_SLAVE`, ESP-IDF defaults, correctly IRAM-safe and not
implicated).

**Empirical confirmation that would fully close it** (not yet done, since
it needs a board): erase a board, configure a bogus audio module via the
web UI (System Settings -> add an audio player; no real hardware needed to
generate the UART traffic), then hammer page loads. If it doesn't reset,
the deadlock is confirmed dead on a board that actually has the audio UART
active. Until then, the mechanism argument above is the basis for
considering it resolved.

## Dead end: do NOT make the UART ISR IRAM-resident

Two attempts (`ESP_INTR_FLAG_IRAM` on `uart_driver_install()`, then also
`CONFIG_UART_ISR_IN_IRAM=y`) made it WORSE, not better. Normally
`spi_flash_disable_interrupts_caches_and_other_cpu` *masks* non-IRAM
interrupts for the flash-op window precisely so they can't fire then;
making the UART ISR IRAM-safe *exempts* it from that masking, so it fires
exactly when it must not and deadlocks on a spinlock the flash machinery
holds on the other core. Both were reverted. The default (UART ISR masked
during flash ops, `intr_alloc_flags = 0`, `CONFIG_UART_ISR_IN_IRAM` unset)
is correct -- do not revisit the IRAM angle.

## The unresolved crash (READ THIS FIRST)

**Symptom:** ESP32-S3 resets with `Guru Meditation Error: Core panic'ed
(Interrupt wdt timeout)` shortly after boot, specifically when the web
server handles its first page request (`view_home()` rendering
`home.html`, though `view_setup()` was also implicated in earlier partial
traces). Device has **no physical hardware attached** — no LEDs, no audio
module — which is relevant (see below).

**What the crash trace shows, consistently across every capture:**
- One core is inside `client_task -> view_home -> render ->
  template_render_file -> process_includes -> template_render_file ->
  fopen -> ... -> SPIFFS_open -> ... -> spiffs_phys_rd -> esp_partition_read
  -> esp_flash_read -> spiflash_start_default -> spi1_start -> cache_disable
  -> spi_flash_disable_interrupts_caches_and_other_cpu` — i.e. reading a
  template file off the `webassets` SPIFFS partition, which requires
  briefly disabling the flash cache on **both** CPU cores.
- The other core is inside `ipc_task -> spi_flash_op_block_func`, the
  companion busy-wait that lets the first core safely disable cache on both
  cores.
- **After** the two IRAM-related attempts below, the panicking core was
  instead caught with `EPC1` inside `uart_hal_write_txfifo`, stuck trying
  to `vPortEnterCritical`/`spinlock_release` from inside
  `shared_intr_isr` — i.e. a UART TX interrupt (from the YX5200 audio
  driver, `components/audio/yx5200.c`) fired *during* the flash-cache-disabled
  window and deadlocked against the flash machinery's spinlock on the
  other core.

**Why audio UART, with no hardware attached:** `ip_announcement.c` tries to
announce the device's IP address over the (unresponsive) audio module
right around boot/first-request time, generating UART TX interrupt traffic
even though nothing ever ACKs it. This lines up with the timing ("crashes
when I hit the web page").

**Two things were tried on the UART side and BOTH made it worse or didn't
help — do not redo these without a different angle:**

1. `uart_driver_install(..., ESP_INTR_FLAG_IRAM)` in `yx5200.c` (line
   ~144) — passing this flag alone. Result: same crash, unchanged.
2. Also enabling `CONFIG_UART_ISR_IN_IRAM=y` (sdkconfig / sdkconfig.defaults)
   on top of (1), so the UART driver's ISR code is genuinely IRAM-resident.
   Result: **crash still happened, but the trace changed** — now clearly
   showing the UART interrupt firing *inside* the flash-cache-disabled
   window and deadlocking on a spinlock. This is the opposite of the fix:
   `spi_flash_disable_interrupts_caches_and_other_cpu` normally *masks*
   non-IRAM interrupts for the duration of the flash op specifically so
   they can't fire at that moment; making the UART ISR IRAM-safe exempts
   it from that masking, so it now fires exactly when it shouldn't and
   contends for a lock the flash code holds on the other core.

Both changes were reverted (back to `intr_alloc_flags = 0` in
`uart_driver_install()`, `CONFIG_UART_ISR_IN_IRAM` left unset) before this
notes file was written, since they were actively making things worse.
**Even after reverting them fully, the user reported the crash was still
happening** — meaning the deadlock, or at least *a* crash with the same
signature, has a cause not yet isolated. The IRAM angle is a dead end;
don't retry it without new evidence.

**Suggested next diagnostic step, not yet tried:** since no hardware is
attached anyway, temporarily disable the audio subsystem entirely at boot
(skip the `audio_player_init_from_config()` / `ip_announcement_check_and_announce()`
calls in `main/boot.c`) and see if the crash still reproduces on a page
load. If it stops, audio/UART is confirmed as at least *a* contributing
factor and the fix needs to be on the ip_announcement/audio side (e.g. not
touching the UART during the boot window when the webserver is starting up,
or delaying IP announcement until after first requests settle) rather than
on the UART driver's interrupt flags. If it still crashes with audio fully
disabled, the SPIFFS/flash-cache angle needs to be re-examined independent
of audio (e.g. something else touching flash concurrently — NVS from WiFi,
the "data" partition, etc.).

## Independent fixes, safe to redo (not related to the crash)

These were all verified working (built and, where applicable, ran) before
the crash investigation started, and aren't implicated by any of the crash
traces above. Re-applying them should be low-risk.

### 1. Audio module "no hardware attached" handling

Problem: once a play was attempted, the driver optimistically marked the
module `is_playing = true`. With nothing attached to respond, every later
status query got no response, and the existing design (both C and Python)
deliberately *keeps the cached state* on an inconclusive read (correct
behavior for avoiding false "stopped" readings on real hardware, but with
no hardware at all it meant "playing" got stuck true forever). Callers
(`ip_announcement`'s retry loop, `audio_player_play_file()`) then treated
the module as permanently busy and kept retrying every call, forever.

Fix (same shape in both languages): track consecutive no-response reads
per module; after 5 in a row, mark it `responsive = false` and release the
stuck "playing" state; skip non-responsive modules in play/health-check
fast paths (no UART round-trip); recover automatically the moment a real
response comes in later. Also fixed the Status page's health check, which
reported `ok=1` just because the UART *write* queued successfully,
regardless of whether anything answered.

Files touched:
- `c_project/components/audio/include/yx5200.h` — added `bool responsive`,
  `int no_response_streak` fields to `yx5200_t`; declared
  `yx5200_is_responsive()`.
- `c_project/components/audio/yx5200.c` — added `NO_RESPONSE_THRESHOLD 5`;
  `note_no_response()`/`note_response_ok()` helpers wired into
  `yx5200_query_status()`'s three inconclusive/error branches and its
  success path; `yx5200_is_responsive()` implementation.
- `c_project/components/audio/include/audio_player.h` — declared
  `audio_player_has_responsive_module()`.
- `c_project/components/audio/audio_player.c` — `audio_player_play_file()`'s
  candidate loop skips non-responsive modules; `audio_player_check_health()`'s
  `ok` field now uses `yx5200_is_responsive()` instead of the UART-write-
  succeeded flag; added `audio_player_has_responsive_module()`.
- `c_project/components/network/ip_announcement.c` — `play_with_retry()`
  bails out immediately once `audio_player_has_responsive_module()` is
  false, instead of burning its full retry budget every call.
- `lib/audio.py` (submodule) — mirrored: `YX5200Player.responsive`,
  `_no_response_streak`, `_no_response_threshold`, `_note_no_response()`,
  `_note_response_ok()` wired into `query_status()`'s three branches;
  `AudioPlayer.play_file()` skips non-responsive players and raises a
  distinct `"No responsive audio modules"` message; `AudioPlayer.check_health()`'s
  `ok` now reflects `player.responsive`; added
  `AudioPlayer.has_responsive_module()`.
- `lib/ip_announcement.py` (submodule) — `_play_with_retry()` bails out via
  `audio_player.has_responsive_module()` the same way.

**Note:** this was implemented *before* the audio-UART-interrupt crash
theory was identified. It doesn't touch `uart_driver_install()` or
interrupt flags at all, so it's orthogonal to the crash - safe to redo
independent of however the crash gets fixed. It also doesn't reduce the
*frequency* of UART TX activity during the vulnerable boot/first-request
window (a play is still attempted once before non-responsiveness is
learned), so it alone won't fix the crash - see the "suggested next
diagnostic step" above for that.

### 2. Client HTTP task stack size increase

`components/webserver/webserver.c`: `CLIENT_TASK_STACK_SIZE` raised from
`8192` to `20480`. Rationale at the time: `view_setup()` was recently
changed to render 9+ nested template includes in one request, and 8KB
seemed tight for that call depth. Turned out not to be the crash's cause
(the crash is a cross-core deadlock, not a stack overflow — no stack-
overflow message was ever produced even after enabling watchpoint
detection, see below), but there's no reason to revert it — cheap
(anything over 4KB already goes to PSRAM automatically on this board,
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`) and still reasonable insurance
against a real stack overflow on heavy pages.

### 3. FreeRTOS stack-overflow watchpoint

`c_project/sdkconfig.defaults`: added
`CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y`. Precise hardware-watchpoint
stack overflow detection, catching the exact overrunning write instead of
letting corrupted memory crash something unrelated later. Useful
diagnostic in general; also directly relevant to future debugging of this
crash if it's ever misdiagnosed as a stack overflow again. Should also be
mirrored into `sdkconfig` directly (not just `sdkconfig.defaults`) since
`sdkconfig` is gitignored and won't pick up defaults changes without a
reconfigure.

### 4. `build_c_release.ps1` reconfigure fix

Added `idf.py reconfigure` before `idf.py build` in the release-build
script. `FIRMWARE_VERSION` (`components/network/CMakeLists.txt`) is
computed via `git describe` at CMake *configure* time only, not on every
build — without forcing a reconfigure, committing/tagging after a build
wouldn't be picked up and the release binary would keep reporting a stale
version string.

## Reference: full uncommitted diff stats at time of revert

```
 build_c_release.ps1                               | 15 ++++++
 c_project/components/audio/audio_player.c         | 32 +++++++++++--
 c_project/components/audio/include/audio_player.h |  8 ++++
 c_project/components/audio/include/yx5200.h       | 16 +++++++
 c_project/components/audio/yx5200.c               | 57 +++++++++++++++++++++++
 c_project/components/network/ip_announcement.c    |  7 +++
 c_project/components/webserver/webserver.c        |  9 +++-
 c_project/sdkconfig.defaults                      | 13 ++++++
 lib (submodule)                                    |  0
 9 files changed, 152 insertions(+), 5 deletions(-)

 lib/audio.py           | 74 ++++++++++++++++++++++++++++++++++++++++++++++++++----
 lib/ip_announcement.py |  6 +++++
 2 files changed, 75 insertions(+), 5 deletions(-)
```

(The UART IRAM attempt and its revert both happened within this same
uncommitted window, so they don't show up separately here - by the time
this diff was captured, `yx5200.c`/`CMakeLists.txt`/`sdkconfig.defaults`
were already back to the "masked, not IRAM" state described above.)
