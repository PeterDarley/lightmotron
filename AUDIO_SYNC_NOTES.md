# Scene/sound synchronization — design notes

**Status as of 2026-08-03: planning only, nothing implemented yet.** This
file exists so the thinking survives across sessions; append rather than
rewrite if this gets picked back up later.

## The problem

`activate_scene()` (`c_project/components/lighting/lighting.c`) calls
`sound_manager_play()` as its very first action, before any job/LED state
is touched. The lights then render their first frame on the next tick
(≤25ms later) — visually instant. Sound is not: the UART command
round-trip itself costs ~50-120ms (module selection, optional volume set,
play command — see `audio_player.c`/`sound_manager.c`/`yx5200.c`), and on
top of that, the module's own SD-card seek + MP3 decode start (outside
this firmware's control) typically adds another ~200ms-1s+. Net effect:
lights consistently lead sound by an unpredictable, hardware-dependent
amount.

No microphone/audio-feedback path exists on this hardware, so "sound
became audible" can never be detected in firmware — only measured by a
human.

## Options considered

1. **Detect sound has started, then trigger the scene.** Would mean
   polling `yx5200_query_status()` (~100ms blocking UART round-trip each)
   after the play command, with timeout handling for bad file
   numbers/missing SD card. Rejected as the primary approach: real
   complexity (new async coordination between two currently-decoupled
   subsystems) for uncertain precision gain, since the module's "playing"
   status bit most likely flips when it starts decoding, not when sound is
   actually audible at the speaker.

2. **Fixed/configurable delay on the scene's visual start.** Chosen
   approach — see design below. Reuses an existing mechanism in the
   tick loop almost exactly as-is.

3. Hybrid (status poll as a soft signal, capped by a max fixed delay) —
   not pursued; adds the complexity of (1) without removing the need for
   (2)'s configuration story.

## Recommended design

- New per-scene field, `scene_settings.<name>.sound_sync_delay`, **stored
  in ticks** (not ms) — matches the existing convention for
  duration/period fields elsewhere in the UI (min/sec/ticks widget,
  stored as total ticks). Default `0` = today's exact behavior.
- Only takes effect when the scene has both a sound configured
  (`meta.sound[0] != '\0'`) and a nonzero delay — a stray delay value on a
  soundless scene should be a no-op, not added lag.
- In `activate_scene()`: when both conditions hold, mark every job that
  does **not** have an `after` dependency as delay-pending, with a target
  tick of `current_tick + delay`. Jobs that already have an `after`
  dependency need no special-casing — they wait for their predecessor to
  finish regardless, so if the predecessor's start shifts later, the
  whole chain shifts with it automatically.
- In `lighting_process_tick()`'s per-job loop: add a pending check
  alongside the existing `after_pending` check (same shape — skip via a
  bare `continue`, no pattern/filter runs, target job's `logical_colors`
  untouched) until the target tick, then clear the flag and reassign
  `job->start_tick = tick`, exactly like the `after_pending` reassignment
  already does.
- Kills / `stop_sounds_on_start` continue to fire immediately (unchanged,
  unaffected by this — they already run before the sound-play call in
  `activate_scene()`).
- No explicit blackout needed during the delay window:
  `activate_scene()` already never clears `logical_colors[]` on
  activation, so the *previous* scene's last-rendered pixels simply keep
  showing until the delay elapses — a natural hold rather than a flash to
  black.
- `lighting_add_scene()` already routes through `activate_scene()`
  internally, so the same mechanism applies there for free.
- Cycle-count limits are unaffected — `local_tick` is computed relative to
  the (now-later) `start_tick`, so a scene's own duration/cycle logic
  still works correctly relative to its actual (delayed) start point.

### Touch points (not yet implemented)

- `c_project/components/lighting/include/metadata.h` — add
  `int sound_sync_delay;` to `scene_metadata_t`.
- `c_project/components/lighting/metadata.c` — read the new field in
  `metadata_get_scene()`, same pattern as `sound`/`kills`/etc.
- `c_project/components/lighting/lighting.c` — `activate_scene()` (mark
  pending jobs) and `lighting_process_tick()` (honor the pending flag),
  per the design above.
- `c_project/components/web/views_scenes.c` — parse the new field in
  `update_scene_settings_from_form()`; expose current value in
  `fill_scene_edit_context()`.
- `templates/setup/scene_edit.html` — new input next to the existing
  "Play Sound on Trigger" control, submitted through the same
  `update_scene_settings` action. Reuse the existing min/sec/ticks widget
  pattern already used for effect Duration/Period, for UI consistency.

Scope: `c_project` only, per the standing decision that the Python side is
frozen (see `.github/copilot-instructions.md`).

## Measuring real-world delay (no firmware changes needed for this part)

Since nothing in the hardware can report "audible," the practical way to
get a real number is to make the trigger moment visually unambiguous and
measure it with a phone camera:

1. Build a test scene: `Solid` pattern at full brightness + a sound
   configured, delay left at 0. Because the sound command fires as the
   very first action in `activate_scene()` and the light's first frame
   renders on the next tick (~25ms later — well under a video frame even
   at high frame rates), the flash and the sound command are effectively
   simultaneous from the camera's point of view.
2. Trigger the scene and film it in slow motion (240fps phone video ≈
   ~4ms/frame resolution — far finer than the tick granularity, 25ms,
   that the delay will actually be configured in).
3. Count frames between the flash and the moment sound is audible;
   convert to ms, then to ticks.
4. Run it 3-4 times with the actual sound file(s) intended for real use —
   latency can vary by SD-card seek position (which file/folder) and
   whether the module was idle vs. just finished playing. Use a
   representative (e.g. median) value rather than a single sample.
5. If in doubt which way to round, err slightly short rather than long —
   lights a touch early reads as less wrong than lights lagging behind.

A "test trigger" UI affordance (fire a scene on demand, outside normal
trigger conditions, to make repeated trials faster) was floated as a
minor convenience but is not required and not yet requested.

## Open questions (not yet decided)

- Default delay for scenes with a sound but no explicit value set — stay
  at `0` (today's behavior, no surprises) or ship a nonzero default
  (~12-16 ticks / 300-400ms) so untouched scenes get some improvement out
  of the box? Leaning toward `0`/opt-in, since the right value is
  hardware/content-dependent and a wrong nonzero default could make an
  already-fine scene worse.
- Whether to add the "test trigger" UI convenience mentioned above.
