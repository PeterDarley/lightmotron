# LED strip hardware bring-up — investigation notes

**Status as of 2026-08-03: ROOT CAUSE CONFIRMED — the logic level converter.**
Wiring GPIO 4 directly to the strip's DIN (bypassing the level shifter
entirely) works. Every other suspect (ESP32/firmware/RMT, the strip, the
series resistor, power delivery) is conclusively cleared. Decision needed:
keep running direct at 3.3V, or debug/replace the shifter — see "Open
decision" below. This file exists so we can pick this up across sessions
without re-deriving context; append to the timeline rather than rewriting
it if this needs revisiting.

## Root cause (confirmed 2026-08-03)

The level shifter between GPIO 4 and the strip's DIN was not passing a
usable signal — everything upstream (ESP32 GPIO, RMT peripheral, firmware
color pipeline) and downstream (the strip itself, the series resistor,
power delivery) checked out individually, and connecting GPIO 4 directly to
the strip's DIN (no shifter, no resistor) lights it correctly. This
retroactively explains every earlier observation: RMT reported successful
transmits because the distortion/blocking happened after the ESP32's own
output; the strip's power draw only reflected idle current because it never
received valid data to display. Specific failure mode inside the shifter
(bad part, wrong wiring channel, OE pin issue, etc.) not yet identified and
likely moot if the direct-3.3V connection is kept (see below).

## Open decision: keep direct 3.3V, or fix/replace the level shifter?

- **Keep it simple**: many WS2812B-family strips work fine driven directly
  at 3.3V logic (out of nominal spec, but common in practice), especially
  on a run this short (33 LEDs). It's already working. Zero further effort.
- **Fix/replace the shifter**: gives proper 5V logic margin, which matters
  more for longer strips, noisier wiring, or ambient temperature extremes
  — more relevant if this model kit's strip run grows later, or if flicker/
  reliability issues show up over time that a shifter would have prevented.
  Would need the shifter's model/chip identified (never determined) to
  debug it specifically (OE pin, wrong channel, etc. — see below).

Not yet decided. No firmware changes needed either way — this was always a
board-level wiring issue, not something the C project's code affects.

## Setup

- First-ever hardware test of `c_project` (the C/ESP-IDF port) with a real
  LED strip attached — 33 LEDs (`leds=33` in the driver's own count),
  single strip.
- Data: ESP32-S3 GPIO 4 → logic level converter (3.3V side) → level
  converter (5V side) → strip DIN.
- Power: **one shared power supply for everything** (ESP32 and LED strip
  both run off it) — confirmed 2026-08-02. This rules out the "separate
  supplies without a tied-together ground" failure mode as the cause; no
  need to re-suggest checking that.
- Scene under test: "PZL P.11a" (per user's earlier message) / a "Running"
  scene seen in an earlier crash-debugging log this session — same device,
  hostname `nautilus` (see `MDNS_NOTES.md` for unrelated mDNS investigation
  on this same unit).

## What's confirmed working (software + electrical basics)

- `lighting_process_tick()`'s debug log (added 2026-08-01, still in place —
  see "Debug instrumentation" below) shows, every 40 ticks: `active_scenes=1`,
  `leds=33`, and real, **changing** RGB values for LEDs 0-4 (e.g. LED 0/1
  cycling roughly 86→117→219→250→168→86... — a breathing/pulse-style
  pattern, correctly computed). Software color pipeline is fully healthy.
- `leds_show()`'s `rmt_transmit()` / `rmt_tx_wait_all_done()` calls
  (error-checked as of 2026-08-02 — see below) report **no failures** —
  the RMT peripheral believes every transmit succeeds.
- 5V confirmed present between the strip's positive/negative power inputs.
- GPIO 4 confirmed wired to the level shifter's 3.3V (low) side.
- Level shifter's 3.3V side confirmed actually reading 3.3V (multimeter).
- Single shared power supply confirmed (see above) — rules out the
  cross-supply common-ground gap.
- **Power draw with LEDs attached: 0.5W → 1W (≈100mA @ 5V)** (2026-08-02).
  This is the right order of magnitude for 33 WS2812-family chips *idling*
  (~1mA/chip baseline just for powered logic, ≈33mA for 33 chips) and far
  too low for the strip actually displaying what the debug log computed —
  LED[2] alone was `(255,255,255)` (full white), which if genuinely lit
  would pull up to ~60mA by itself. **Conclusion: the strip is powered and
  idling, not receiving usable/valid data** — rules out "it's actually
  working but too dim to see" and points more specifically at the signal
  not reaching the strip's DIN pin in valid form (shifter/OE/wiring/timing),
  not a dead strip or power delivery problem.

## What's NOT yet checked (next steps, in priority order)

0. ~~**Series data resistor value.**~~ RULED OUT (2026-08-02): user has a
   resistor between the level shifter output and the strip's DIN (standard
   practice for reflection damping/ESD protection). Measured at
   **463Ω** — squarely within the standard recommended range (220-500Ω,
   330-470Ω most commonly cited) — essentially a textbook-normal value, not
   a plausible cause. (First measurement was misread as 630Ω, which would
   have been a more reasonable suspect; corrected by user to 463Ω.) No
   action needed here.
1. **Level shifter model/type, and whether it has an output-enable (OE)
   pin.** Not yet identified. If it's a buffer-style chip (e.g.
   74AHCT125/74HCT245-family), an unconnected or wrongly-tied OE pin would
   let the low side read perfectly correct voltage while the high-side
   output stays tri-stated (passes nothing) — matches every symptom seen so
   far. **Ask user for the specific chip/module.**
2. **Direct-3.3V bypass test**: temporarily wire GPIO 4 straight to the
   strip's DIN, skipping the level shifter (and the series resistor above)
   entirely. Many WS2812B-family strips (especially a short 33-LED run)
   work fine at 3.3V logic despite being nominally out of spec. If this
   lights the strip, the level shifter/resistor (or their wiring) is
   conclusively the problem. If it *doesn't* light either, suspect the
   strip itself or the data wiring between the shifter/GPIO and the strip's
   DIN pad. **Not yet tried.**
3. **Confirm the level shifter's 5V-side output actually reaches the
   strip's DIN pin** — a loose/broken wire between shifter output and
   strip input would look identical to everything verified so far (all
   the *inputs* to the shifter check out; its *output* hasn't been
   measured/traced to the strip yet).
4. **First-pixel-only check**: WS2812-family strips are serial repeaters —
   a dead or miswired first LED would leave the entire rest of the strip
   dark even with a perfect signal. Worth confirming LED 0 specifically
   (visually, or by temporarily setting the configured LED count to 1) once
   the above are ruled out.

## Debug instrumentation currently in the codebase (added this investigation, not yet removed)

- `components/lighting/lighting.c`, `lighting_process_tick()`: logs
  `active_scenes`, driver `leds` count, and LEDs 0-4's RGB every 40 ticks
  (~1s). Marked in-code as temporary/removable once the hardware issue is
  found.
- `components/leds/leds.c`, `leds_show()`: `rmt_transmit()` /
  `rmt_tx_wait_all_done()` return values are now checked and logged
  (throttled to every 40th failure) instead of silently ignored — this is
  a genuine bug fix (was a real gap, not just diagnostic scaffolding) and
  should stay regardless of how this investigation concludes.

## Timeline

- **2026-08-01**: First power-up with LEDs attached, nothing lit. Added
  the tick-based LED-state debug log (see above) to check whether the
  color pipeline was even computing anything.
- **2026-08-02**: Debug log showed healthy, changing colors for LEDs 0-4
  and `active_scenes=1`/`leds=33` — software pipeline confirmed working.
  Added `rmt_transmit`/`rmt_tx_wait_all_done` error checking to `leds_show()`
  (previously silent) — re-tested, no transmit errors logged. Verified 5V
  at strip power inputs, GPIO4→level-shifter-3.3V-side wiring, and 3.3V
  actually present there. Confirmed single shared power supply (rules out
  cross-supply ground gap). Power draw with strip attached measured at
  0.5W→1W (≈100mA @ 5V) — consistent with 33 idling chips, not a lit
  strip (see above for the math). Checked the series data resistor between
  the level shifter and the strip: 463Ω, squarely normal — ruled out as a
  cause. Still dark. Next: level shifter model/OE pin, then the direct-3.3V
  bypass test.
