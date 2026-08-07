# Audio/UART crash investigation + change log

**RESOLVED (2026-07-24, final/verified this time):** root cause was
per-request template reads from the `webassets` SPIFFS partition. Every
`GET` that rendered a template did a fresh `fopen`/`fread` off flash, which
briefly disables the flash cache on **both** CPU cores; if a UART TX
interrupt (from the YX5200 audio driver) fired in that window, it deadlocked
against the flash machinery's cross-core spinlock and the interrupt
watchdog reset the board. Fixed by caching every web asset in RAM at boot
(`components/webserver/asset_cache.c`) so a request never touches flash at
all. Two earlier "resolved" write-ups in this file's history turned out to
be wrong — see "Dead ends" below for why they looked plausible at the time.

## The fix

`asset_cache_init()` (called from `main/boot.c`, right before
`webserver_start()`) reads every file on the `webassets` SPIFFS partition
into RAM once at boot. `template_engine.c`'s `template_render_file()` and
`webserver/static_files.c`'s `static_file_serve()` both check this cache
first and only fall back to a real flash read on a cache miss (which
shouldn't happen for anything actually deployed). Since `persistent_dict.c`
already RAM-caches `/data/*.json` settings after first load, this made the
template read the *only* remaining per-request flash access — eliminating
it means normal page requests never disable the flash cache at all, so the
UART-collision window that caused every captured crash trace no longer
exists.

Buffers are allocated with `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`
(not plain `malloc()`) — see "Follow-on: RAM exhaustion" below for why that
distinction mattered.

**Residual risk (accepted, much rarer):** a settings POST still does a real
flash write while holding `persistent_dict`'s mutex; if a render's read of
the *same* PSRAM buffer somehow raced a write to actual flash at the exact
same instant, the same class of window could theoretically reopen. In
practice these are different codepaths (writes go through
`json_write_file()`/SPIFFS, not through the asset cache), so this is
believed to be a non-issue, not just a smaller one — noted for completeness
rather than as a known gap.

## Follow-on: RAM exhaustion (found immediately after deploying the fix)

Caching ~80 files (514KB total) with plain `malloc()` triggered a *new*
symptom: `webserver: Failed to spawn client task, dropping connection`,
CSS/JS missing from pages. Cause: this project's
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` forces any allocation under 4KB
into internal DRAM regardless of PSRAM availability — and most individual
template/CSS/JS files are under 4KB. That silently ate the same internal
RAM pool that 20KB client-task stacks need (task stacks can never live in
PSRAM). Fixed by allocating asset-cache buffers with
`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` explicitly, forcing all of them
into PSRAM regardless of size.

## Follow-on: stack fragmentation (found right after the RAM fix)

Even with 126KB total free internal RAM, `xTaskCreate` for client tasks
kept failing. Diagnostic logging (`heap_caps_get_largest_free_block`) added
to `webserver.c`'s spawn-failure path and `boot.c`'s post-cache-init log
line showed the real ceiling was a *contiguous* block, not total free
bytes — internal RAM is shared with WiFi/lwip/mDNS and gets fragmented, so
a 20KB stack request (`CLIENT_TASK_STACK_SIZE`, bumped from 8KB earlier in
this file's history on a since-disproven stack-overflow theory) frequently
couldn't find a contiguous block even with plenty of total free space. A
single page load fires ~6 parallel asset requests, so this reliably starved
out CSS/JS on every load. Fixed by right-sizing the stack down to 12KB
(template includes only nest ~3 deep in practice — verified via
`uxTaskGetStackHighWaterMark`, which now logs a warning if any client task
ever gets within 2KB of overflowing), which raised `MAX_CLIENT_TASKS` from
4 to 6 (matches a browser's per-host connection limit) without exceeding
the RAM budget, and shortened the keep-alive idle timeout from 5s to 3s so
idle connections release their slot faster.

## Dead ends (don't retry these without genuinely new evidence)

### 1. UART ISR made IRAM-resident — made it WORSE

`uart_driver_install(..., ESP_INTR_FLAG_IRAM)` in `yx5200.c`, later also
`CONFIG_UART_ISR_IN_IRAM=y`. `spi_flash_disable_interrupts_caches_and_other_cpu`
normally *masks* non-IRAM interrupts specifically so they can't fire during
a flash op; making the UART ISR IRAM-safe exempts it from that masking, so
it now fires exactly when it must not and deadlocks on the flash
machinery's spinlock. Both reverted. Default config (`intr_alloc_flags = 0`,
`CONFIG_UART_ISR_IN_IRAM` unset — UART ISR masked during flash ops) is
correct; do not revisit this angle.

### 2. "Persistent `data`-partition state survives a full flash" — incomplete, not the real fix

An earlier pass in this file's history found that a **fresh** board
(freshly erased) didn't crash while an old board with stale `data`-partition
content did, and concluded the crash needed a populated `audio_players`
config to trigger UART traffic at all. That's true as far as it goes — a
board with zero audio config was never going to generate UART interrupts,
so it couldn't hit the collision. But it was mistaken for *the* fix: later,
a genuinely fresh, fully-erased board **still crashed** once audio was
configured through the UI and pages were hit — proving the deadlock
mechanism itself was untouched; the earlier test had just avoided the
trigger condition (no audio config) rather than fixing the collision.
`flash_c.ps1 -Erase` is still good practice for clearing stale settings
after a partition-table change, just not a fix for this crash.

### 3. "The deadlock cannot occur with the default config" — reasoning was correct but incomplete

A later pass in this file's history argued (correctly, as far as the
argument went) that a non-IRAM UART ISR can't run *during* the flash-cache
window at all, so no collision was possible with the default (reverted)
config. The logic about IRAM masking is accurate — but it missed that the
deadlock only needs the flash-cache window to be open when the UART ISR is
*pending*, not literally mid-instruction; a `SPIFFS` read on every page
request kept re-opening that window constantly, so the actual fix was
never about the UART side at all — it was making the flash-cache window
stop opening on every request in the first place (the asset cache, above).

## Independent fixes, still valid

These aren't related to the crash and remain correctly in place:

### Audio module "no hardware attached" handling

Once a play was attempted with nothing attached to respond, the driver got
stuck believing a module was still playing forever, so every later call
retried indefinitely. Fixed (same shape in C and Python) by tracking
consecutive no-response reads per module; after 5 in a row the module is
marked non-responsive and skipped by the fast paths. See
`components/audio/yx5200.c`/`audio_player.c`, `components/network/ip_announcement.c`,
and the `lib/audio.py`/`lib/ip_announcement.py` mirrors.

**Correction (2026-08-05):** the line above used to say this "recovers
automatically the moment a real response comes in" — that was the *intent*
(see `yx5200.h`'s doc comment on `responsive`), but not what the code
actually did in C: `audio_player_play_file()`'s fast path skips any module
with `responsive == false` outright, and nothing else in the normal
playback flow ever calls `yx5200_query_status()` on it again, so there was
no way back to `responsive == true` short of the manual health-check
endpoint. Real symptom this caused: booting with the audio module
genuinely attached and working, the IP announcement would play its first
1-2 files, then the module would go quiet for about a second during a
normal file-to-file transition (all 1 configured module, `no_response_streak`
hits `NO_RESPONSE_THRESHOLD=5` within ~1s given `play_with_retry()`'s
~100-200ms poll cadence), get marked "no module attached" even though it
was fine, and every subsequent play attempt for the rest of the boot
session silently skipped it — no crash, no error, just silence.

Fixed by adding `yx5200_recover_if_due()` (`yx5200.c`/`.h`): when a module
is unresponsive, `audio_player_play_file()` now re-probes it at most once
every `REPROBE_INTERVAL_MS` (2000ms) instead of skipping it forever. Cheap
enough not to add latency for genuinely-absent hardware (still skipped
between reprobes), but frequent enough to recover well within
`play_with_retry()`'s 10s-per-file retry budget. Not ported back to
`lib/audio.py` — Python side is frozen, `c_project` is the only actively
developed version.

**Follow-up (2026-08-05, same day): the above fix alone didn't help — two
more bugs in the same area, found by tracing why not.**

User reflashed with the reprobe fix; got the *identical* log signature (3x
"All 1 modules busy" then "no response after 5 attempts" at almost the
same boot-relative timestamp) and reported a new symptom: "It sounds like
it's playing the second file before the first is finishing." Traced both:

1. **`note_no_response()` was forcing `is_playing = false`** the moment the
   miss-streak crossed the threshold — conflating "hasn't answered a
   status query in ~1s" with "confirmed stopped." For a module that's
   simply busy decoding/playing and doesn't answer `CMD_QUERY_STATUS`
   while busy (plausible for these clones), that's backwards: it frees the
   module up while it's still audibly playing, so the next file's play
   command cuts off/overlaps the current one. Fixed: no-response no longer
   touches `is_playing` at all — a module marked unresponsive is already
   fully skipped by `audio_player_play_file()`'s fast path regardless of
   `is_playing`'s value, so leaving it alone doesn't reopen the original
   "stuck busy forever" problem.
2. **The reprobe fix from the same-day-earlier entry above was never
   actually reached.** `ip_announcement.c`'s `play_with_retry()` bails out
   the instant `audio_player_has_responsive_module()` reads false — which
   happens the moment a module is marked unresponsive — so the retry loop
   gave up almost immediately, well before `yx5200_recover_if_due()`'s
   2-second cooldown ever elapsed a second time. Fixed by adding a sticky
   `known_present` flag (set once, first time a module ever answers a
   query, never cleared) and a new `yx5200_could_recover()` check
   (`responsive || known_present`) that `audio_player_has_responsive_module()`
   now uses instead of raw `responsive`. A module that's proven itself
   real at some point is no longer treated the same as one that's never
   answered at all — only the latter causes an early bail.

Also added logging per user request ("add more debugging around playing
sounds"): play-issued, query-outcome (inconclusive/no-frame/valid-frame),
confirmed-stopped, module-selection, and busy/skip decisions are now all
at `ESP_LOGI` (previously buried at `ESP_LOGD`, invisible unless
`audio_debug_logging` is enabled in System Settings) — so a normal boot
log capture now shows the full play/query lifecycle without needing to
flip that setting first. TX/RX hex dumps (`log_hex()`) remain at `DEBUG`
only, gated by `audio_debug_logging`, since those are high-volume.

**Not yet reflashed/retested.**

### RESOLVED: root cause was a GPIO conflict with the (inert) billboard driver, not the module or the debounce logic at all

User reflashed with the above and still got the identical failure. Then
ran the decisive test: same physical board, known-good (just proven by
booting the *Python* build on it and getting the full IP announcement),
reflashed with the C build, same board, same wiring — and got the exact
same "only the first file" symptom. That conclusively ruled out wiring/
hardware, which the previous entry's "no response after 5 attempts, even
on the very first idle-state health-check query" evidence had pointed at.

Real root cause: `components/billboard/max7219.c`'s `max7219_init()`
claims its CS pin as a GPIO output and drives it high
(`gpio_config()`/`gpio_set_level(cs_pin, 1)`) **unconditionally, before**
validating the rest of the SPI config — and never releases it on the
failure path. This device's boot log showed `spicommon_bus_initialize_io:
mosi not valid` / `SPI bus init failed` (a stale/incomplete `billboard`
settings entry, no billboard hardware actually present) — but the CS pin
claim above already happened by that point regardless. `DEFAULT_BILLBOARD_CS`
(`main/settings.h`) is GPIO5, which is exactly the pin this board's audio
module was configured to use as its UART RX line. With that pin pinned
high by the billboard's GPIO claim, the YX5200's TX signal was permanently
overridden on the shared wire: play/reset/volume commands (TX-only, ESP→
module) worked perfectly, but every status query (which needs the
module's response to actually reach the ESP32's RX) got nothing, from the
very first query onward — exactly matching every log captured across this
whole investigation. This was never a timing, debounce, or protocol issue;
every fix applied earlier in this file (the reprobe mechanism, the
no-response/is_playing decoupling, the logging) was real, correct, and
still worth having for genuine transient flakiness, but none of it could
fix a permanently-shorted RX line.

Two fixes applied:
1. `max7219_init()` reordered so the CS pin is only claimed *after*
   `spi_bus_initialize()`/`spi_bus_add_device()` both succeed, with the
   framebuffer/bus properly released on either failure path. This alone
   prevents a broken billboard config from ever holding a GPIO hostage
   again, regardless of what else that pin is used for.
2. Per user direction ("the billboard is cruft from an old version... it
   shouldn't be activated at all"), `main/boot.c`'s billboard init call
   and its default-settings seeding block were both removed. The billboard
   driver code stays in `components/billboard/` for reference, but nothing
   in the current firmware calls into it, and a fresh device no longer
   gets a `billboard` key seeded into its settings at all — removing the
   GPIO5 collision at its source rather than relying only on fix #1's
   defensive reordering. Confirmed via a clean rebuild that this actually
   drops now-unreferenced code (~30KB smaller binary).

**Not yet reflashed/retested**, but this is the real fix — next boot
should play the complete IP announcement.

### FreeRTOS stack-overflow watchpoint

`c_project/sdkconfig.defaults`: `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y`.
Hardware-watchpoint stack overflow detection — catches the exact
overrunning write instead of letting corrupted memory crash something
unrelated later. Still valuable as a general diagnostic.

### `build_c_release.ps1` reconfigure fix

Runs `idf.py reconfigure` before `idf.py build` so `FIRMWARE_VERSION`
(computed via `git describe` at CMake *configure* time) picks up new tags/
commits instead of reporting a stale version baked in at the last
reconfigure.
