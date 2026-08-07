# Programming Guide — Effects, Sounds, and Scenes

This page is the complete reference for configuring what your Lightmotron
does: lighting patterns, filters, sounds, and how they combine into scenes.
Everything here is done through the device's web interface (the **Setup**
page) — no code required.

If you haven't installed the firmware yet, start with the main
[README](../README.md). If you're a developer looking for how this is
implemented internally, see the [Developer Guide](developer.md).

## Key Concepts

* **Scene** — A named configuration that controls LED behavior. Scenes can
  run continuously or be triggered on-demand.
* **Effect** — A reusable LED animation (e.g. "pulse", "wave", "breathe").
  Each effect has parameters you can customize.
* **Filter** — Optional post-processing applied to an effect's output (e.g.
  "sparkle", "flicker"). Multiple filters can be stacked.
* **Named Range** — A group of LEDs that you label (e.g. "engine lights").
  Useful for targeting groups instead of individual indices.
* **Custom Color** — Save your own colors with custom names to reuse across
  scenes.
* **Model** — A complete, self-contained set of the above (see
  [Models](#models) below), letting one device hold configurations for
  multiple kits.

## Setting Up via the Web Interface

The **Setup** page provides a visual editor for all lighting and sound
configuration. Each card opens a dialog to manage that category.

1. **Custom Colors** — Define and name your own colors once, reuse them
   everywhere.
2. **Named Ranges** — Group LEDs and give them meaningful names (e.g.
   "Nacelle", "Hull"). Use the LED picker to visually select individual
   LEDs, or include another named range to build composite groups. Range
   buttons show the resolved LED summary, and included subranges are
   listed separately in the editor where they can be removed. In the LED
   picker, highlighted buttons show only LEDs directly selected in the
   current range — LEDs inherited through included subranges aren't shown
   as direct selections.
3. **Effects** — Create reusable animations with a pattern, colors, and
   optional filters. See [Effects Reference](#effects-reference) below for
   every available pattern and its parameters.
4. **Filters** — Create reusable post-processing filters (sparkle, flicker,
   etc.) that can be applied to any effect. See
   [Filters Reference](#filters-reference) below.
5. **Scenes** — Combine effects into complete lighting scenarios. Each
   scene contains one or more jobs assigning effects to LED targets. See
   [Scenes](#scenes) below for scene-level behavior (kills, trigger sounds,
   stop lists).
6. **Sounds** — Create named sounds that map to MP3 file numbers on the SD
   cards in your audio modules. See [Sounds & Soundscapes](#sounds--soundscapes)
   below.
7. **Soundscapes** — Create ordered groups of sound entries that play in
   sequence.
8. **Models** — Manage multiple named configurations on one device. See
   [Models](#models) below.
9. **Theme** — Choose a CSS theme to customize the look of the interface.
   See the [Theming Guide](theming.md) if you want to create your own.

Once configured, use the **Home** page to start/stop animation playback,
view active scenes, and trigger immediate scene changes.

## Effects Reference

Quick summary of every built-in pattern:

| Effect | What It Does | Good For |
|---|---|---|
| **Solid** | Single color, no animation | Steady lights, engine glow |
| **Blink** | Symmetric on/off flashing | Warning lights, indicators |
| **Pulse** | Asymmetric flashing with separate on-time and period | Pulsing beacons |
| **Fade In** | Smooth color transition | Startup sequences, transitions |
| **Breathe** | Smooth up-and-down oscillation | Life-like breathing, organic feel |
| **Wave** | Moving light across LEDs | Scanning beams, comet sweep |
| **Cylon** | Wave that bounces back and forth | Iconic bouncing scan effect |
| **Phaser Strip** | Two waves converge from opposite ends | Sci-fi phaser effects |
| **Rainbow** | Cycling, spatially-spread hue sweep | Party lights, prismatic effects |
| **Color Wipe** | Progressive fill, then wipe back | Loading/charging sequences, reveals |
| **Fire** | Flickering flame simulation | Torches, engine fire, forges |
| **Gradient** | Spatial blend across two or more colors, optionally scrolling | Ambient backdrops, smooth color transitions |
| **Warp Pulse** | Band expanding outward from the center | Sci-fi warp/energy surges |
| **Theater Chase** | Evenly spaced dots marching along the strip | Marquee lights, runway effects |
| **Heartbeat** | Organic double-thump ("lub-dub") then rest | Life-support monitors, tension cues |

Use `Blink` for equal on/off timing. Use `Pulse` when you need a short
flash with a longer gap, such as one tick on every second.

### Pattern Parameters

Ticks are the animation engine's internal time unit (the Setup UI's
duration/period fields enter minutes/seconds/ticks and store the total).

#### `solid`
Sets all target LEDs to a fixed color. No animation.

| Parameter | Description |
|---|---|
| `colors[0]` | The color to display |

#### `blink`
Alternates between two colors symmetrically. The on-time and off-time are the same.

| Parameter | Description |
|---|---|
| `duration` | Half-period in ticks. Shows `colors[0]` for this many ticks, then `colors[1]` for the same number of ticks. |
| `frequency` | Optional override in blinks per second. If present, it takes precedence over `duration`. |
| `colors[0]` | On color |
| `colors[1]` | Off color |

#### `pulse`
Like `blink` but with separate on-time and full-cycle timing, allowing asymmetric pulses.

| Parameter | Description |
|---|---|
| `duration` | Number of ticks the on color is shown |
| `period` | Full cycle length in ticks (`duration` + off-time) |
| `colors[0]` | On color |
| `colors[1]` | Off color |

#### `fade_in`
Linearly interpolates from `colors[0]` to `colors[1]` over a set duration, then holds.

| Parameter | Description |
|---|---|
| `duration` | Number of ticks for the full fade |
| `colors[0]` | Start color |
| `colors[1]` | End color |

#### `breathe`
Uses a sine wave to smoothly oscillate between two colors, creating a breathing effect.

| Parameter | Description |
|---|---|
| `duration` | Full cycle length in ticks |
| `colors[0]` | Dim/off color |
| `colors[1]` | Bright/on color |

#### `wave`
A comet or comets sweep across the LEDs. Each peak is set to `colors[1]` for one tick, then fades back to `colors[0]` over `width` LEDs of travel.

| Parameter | Default | Description |
|---|---|---|
| `duration` | 40 | Full sweep length in ticks |
| `width` | 5 | Fade trail length in LEDs |
| `number` | 1 | Number of simultaneous peaks, evenly spaced |
| `reverse` | `false` | If true, sweeps from last LED to first |
| `colors[0]` | — | Background/trail-end color |
| `colors[1]` | — | Peak color |

#### `cylon`
Like `wave` but the peak bounces back and forth (forward sweep then reverse sweep).

| Parameter | Default | Description |
|---|---|---|
| `duration` | 40 | One-way sweep length in ticks |
| `width` | 5 | Fade trail length in LEDs |
| `colors[0]` | — | Background/trail-end color |
| `colors[1]` | — | Peak color |

#### `phaser_strip`
Two waves start at opposite ends of the target range and converge on a randomly chosen meeting point, both arriving at the same tick. After meeting, the meeting point holds lit while trails fade, then resets.

| Parameter | Default | Description |
|---|---|---|
| `duration` | 40 | Total ticks for the wave convergence phase |
| `width` | 5 | Fade trail length in LEDs |
| `colors[0]` | — | Background/trail-end color |
| `colors[1]` | — | Peak/meeting color |

#### `rainbow`
Cycles hue over time and spreads it spatially across the strip, HSV-based. Ignores `colors` entirely.

| Parameter | Default | Description |
|---|---|---|
| `duration` | 80 | Ticks for one full hue cycle |
| `number` | 1 | Number of rainbow repeats spread across the strip |
| `saturation` | 1.0 | Color saturation (0.0 = white, 1.0 = fully saturated) |

#### `color_wipe`
Progressively fills the target from the first LED to the last with `colors[0]`, then wipes back to `colors[1]` the same way.

| Parameter | Default | Description |
|---|---|---|
| `duration` | 40 | Ticks for one fill direction (full cycle is `duration * 2`) |
| `colors[0]` | — | Fill color |
| `colors[1]` | — | Background/wipe-back color |

#### `fire`
Flickering flame simulation: each tick cools every LED slightly, diffuses heat toward the far end, and randomly sparks new heat near the base. Heat values map to a black→red→orange→yellow→white ramp. Ignores `colors` entirely.

| Parameter | Default | Description |
|---|---|---|
| `cooling` | 55 | How fast heat dissipates each tick; higher = shorter, choppier flames |
| `sparking` | 120 | Chance (out of 255) per tick of a new spark near the base; higher = more active fire |
| `duration` | 40 | Only used as the cadence for counting `cycles=` (fire itself has no natural cycle) |

#### `gradient`
A spatial blend across two or more `colors`, evenly spread over the target. With `duration` set, the gradient scrolls along the strip over time instead of staying static.

| Parameter | Default | Description |
|---|---|---|
| `duration` | 0 | `0` = static gradient; `>0` = ticks for one full scroll cycle |
| `colors` | — | Two or more colors; the gradient blends through them in order, wrapping back to the first |

#### `warp_pulse`
A band of color expands outward from the center to both ends, then resets and repeats.

| Parameter | Default | Description |
|---|---|---|
| `width` | 4 | Half-width (in LEDs) of the expanding band's soft edge |
| `period` | 40 | Ticks for one full expansion cycle |
| `colors[0]` | — | Peak/band color |
| `colors[1]` | — | Background color |

#### `theater_chase`
Evenly spaced dots march along the strip, marquee-style.

| Parameter | Default | Description |
|---|---|---|
| `spacing` | 3 | LED spacing between dots (minimum 2) |
| `width` | 1 | Dot width in LEDs |
| `duration` | 6 | Ticks per step; lower = faster movement |
| `reverse` | `false` | If true, dots march in the opposite direction |
| `colors[0]` | — | Dot color |
| `colors[1]` | — | Background color |

#### `heartbeat`
An organic double-thump ("lub-dub") followed by a rest, looping.

| Parameter | Default | Description |
|---|---|---|
| `period` | 50 | Ticks per full heartbeat cycle (minimum 8) |
| `colors[0]` | — | Thump/peak color |
| `colors[1]` | — | Rest/background color |

## Filters Reference

Filters add visual flavor to effects after they render. Multiple filters
can be chained; in the effect editor UI, the displayed filter list is
intentionally reversed relative to execution order (the top filter runs
last). Filters are order-independent in their result — see the
[Developer Guide](developer.md) if you want to know why.

| Filter | What It Does | Good For |
|---|---|---|
| **Scintillate** | Independent sparkling per LED | Twinkling stars, fireworks |
| **Sizzle** | Synchronized group flicker | Electrical arcing, unified flicker |
| **Brightness** | Multiplies each RGB channel by a constant, clamped to 0-255 | Global dimming/boost, quick intensity matching |
| **Spike** | Periodically overrides LEDs with a flash color | Warning strobes, energy discharge |
| **Dropout** | Periodically overrides LEDs with black | Flickering connections, damage effects |
| **Afterglow** | Blends each LED toward its own previous frame | Motion trails, phosphor-glow look |
| **Tint** | Multiplicative color overlay, like a lighting gel | Red-alert washes, mood color grading |
| **Shimmer** | Smooth brightness wave traveling along the strip | Water/light shimmer, energy fields |
| **Hue Shift** | Rotates every color around the hue wheel | Animated color cycling on any effect |
| **Saturation** | Scales color saturation toward gray or full color | "Systems failing" desaturation, color pop |
| **Vignette** | Dims the ends of the range, brightest in the middle | Spotlight focus, framing an effect |

### Filter Parameters

#### `null`
Passes the LED list through unchanged. Useful for testing or as a placeholder.

#### `sizzle`
Computes a single random deviation from the first LED's current position toward its target color, then applies that same deviation uniformly to all LEDs. Creates a coordinated group flicker.

| Parameter | Default | Description |
|---|---|---|
| `frequency` | 40 | Updates per second |
| `variation_percent` | 20 | Maximum random channel deviation as a percentage of each channel's current target value |

#### `scintillate`
Like `sizzle` but each LED is adjusted independently, creating a sparkling/twinkling effect where individual LEDs vary in different directions.

| Parameter | Default | Description |
|---|---|---|
| `frequency` | 40 | Updates per second |
| `variation_percent` | 20 | Maximum random channel deviation as a percentage of each channel's current target value |

#### `brightness`
Multiplies each target RGB channel by a constant factor. Each resulting channel value is clamped to the inclusive integer range `0..255`.

| Parameter | Default | Description |
|---|---|---|
| `brightness` | 1.0 | Constant multiplier per channel (`0.5` = half brightness, `2.0` = double brightness) |

#### `spike`
Periodically overrides LEDs with a spike color. Each spike lasts `duration` ± `heat` ticks; the next spike is scheduled `period` ± `variation` ticks after the previous one ends, so spikes don't cluster when they run long. In the Setup UI, `duration` and `period` are entered with minutes/seconds/ticks controls.

| Parameter | Default | Description |
|---|---|---|
| `color` | `white` | Spike color (name or RGB) |
| `duration` | 5 | Spike length in ticks |
| `period` | 40 | Delay before next spike (after previous spike ends) |
| `variation` | 0 | Random ± tick offset applied to each period |
| `heat` | 0 | Random ± tick offset applied to each spike duration |
| `scope` | `all` | Grouping mode: `all`, `subranges`, or `leds` |

#### `dropout`
Identical to `spike` but the override color is always black. For `scope='subranges'` on an aggregate named range, each component subrange is treated as its own group so adjacent components don't accidentally merge.

| Parameter | Default | Description |
|---|---|---|
| `duration` | 5 | Dropout length in ticks |
| `period` | 40 | Delay before next dropout (after previous dropout ends) |
| `variation` | 0 | Random ± tick offset applied to each period |
| `heat` | 0 | Random ± tick offset applied to each dropout duration |
| `scope` | `all` | Grouping mode: `all`, `subranges`, or `leds` |

#### `afterglow`
Leaves fading trails by blending each LED toward its own previous frame instead of switching instantly — trails only fade, they never dim something that just got brighter.

| Parameter | Default | Description |
|---|---|---|
| `decay` | 0.85 | Fraction (0.0-0.99) of the previous frame retained each tick; higher = longer trails |

#### `tint`
Overlays a color like a lighting gel, via a multiplicative blend. Good for a droppable "red alert" wash over an existing effect.

| Parameter | Default | Description |
|---|---|---|
| `color` | `red` | Gel color (name or RGB) |
| `strength` | 0.5 | Blend amount (0.0 = no effect, 1.0 = fully gelled) |

#### `shimmer`
A smooth brightness wave travels along the strip, dimming and brightening LEDs as it passes.

| Parameter | Default | Description |
|---|---|---|
| `wavelength` | 8 | LEDs per wave crest |
| `speed` | 1.0 | Wave travel speed |
| `depth` | 0.5 | Dimming amount at the troughs (0.0 = no dimming, 1.0 = fully dark at troughs) |

#### `hue_shift`
Rotates every LED's color around the hue wheel, in HSV space.

| Parameter | Default | Description |
|---|---|---|
| `offset` | 0.0 | Fixed hue rotation (0.0-1.0, wraps around) |
| `speed` | 0.0 | Additional rotation per tick; `0` leaves the shift static at `offset` |

#### `saturation`
Scales color saturation in HSV space, leaving hue and brightness alone.

| Parameter | Default | Description |
|---|---|---|
| `amount` | 1.0 | Saturation multiplier (0.0-2.0); `<1` drains toward gray (e.g. a "systems failing" cue), `>1` boosts, `1.0` is unchanged |

#### `vignette`
Dims the ends of the target range so the middle is brightest, like a camera vignette (or the reverse, with `invert`).

| Parameter | Default | Description |
|---|---|---|
| `falloff` | 0.5 | Dimming strength at the edges (0.0 = no dimming, 1.0 = edges fully dark) |

## Colors

Colors can be selected by name or as a custom RGB value. Most patterns take
two colors: the first is the primary/on color, the second is the
secondary/off color.

| Name | RGB | Description |
|---|---|---|
| `white` | (255, 255, 255) | Full-brightness white |
| `warm_white` | (255, 220, 160) | Warm incandescent white |
| `cool_white` | (180, 210, 255) | Cool daylight white |
| `dim_white` | (64, 64, 64) | Low-level ambient white |
| `silver` | (180, 180, 200) | Slightly cool silver-grey |
| `grey` | (128, 128, 128) | Mid grey |
| `black` | (0, 0, 0) | Off |
| `red` | (255, 0, 0) | Pure red |
| `dark_red` | (128, 0, 0) | Deep red |
| `orange` | (255, 100, 0) | Orange |
| `amber` | (255, 160, 0) | Amber / warm orange |
| `gold` | (255, 200, 0) | Bright gold |
| `yellow` | (255, 255, 0) | Yellow |
| `green` | (0, 255, 0) | Pure green |
| `dark_green` | (0, 128, 0) | Deep green |
| `lime` | (128, 255, 0) | Yellow-green |
| `teal` | (0, 180, 128) | Blue-green teal |
| `cyan` | (0, 255, 255) | Cyan |
| `ice_blue` | (80, 160, 255) | Light icy blue |
| `blue` | (0, 0, 255) | Pure blue |
| `dark_blue` | (0, 0, 128) | Deep blue |
| `indigo` | (60, 0, 180) | Deep indigo |
| `violet` | (180, 0, 255) | Bright violet |
| `purple` | (128, 0, 128) | Mid purple |
| `magenta` | (255, 0, 255) | Magenta |
| `pink` | (255, 80, 150) | Hot pink |
| `fire` | (255, 40, 0) | Deep orange-red flame |
| `plasma` | (0, 200, 255) | Sci-fi plasma blue |
| `engine_glow` | (100, 40, 255) | Purple engine exhaust glow |

Use **Custom Colors** on the Setup page to define and name your own colors
alongside these built-ins.

## Target Specification

Every job in a scene targets one or more LEDs. The LED picker builds these
for you visually, but it's useful to know what they mean:

| Value | Meaning |
|---|---|
| A single number | One LED index |
| A list of numbers | Explicit list of indices |
| A range like `0-7` | Inclusive range |
| `all` | All LEDs |
| A named range | Look up a range you've defined under Named Ranges |

Named ranges can include other named ranges to build composite groups (for
example, an "engine" range that includes a "wing" range plus a few extra
LEDs). The Setup UI validates named-range edits and rejects circular
references.

## Models

The system supports multiple named **Models**. Each Model is a complete,
self-contained lighting configuration — its own scenes, effects, filters,
named ranges, custom colors, and optionally its own sounds. Use the
**Models** card on the Setup page to create, rename, delete, or switch the
active model.

This is useful if you use one device to control lighting for more than one
kit — build out a full configuration for each kit as its own Model, and
switch between them instead of reconfiguring from scratch.

## Sounds & Soundscapes

**Sounds** map a name you choose to an MP3 file number on the SD card in
one of your audio modules. Once created, a sound can:

* **Loop** — automatically restart when playback ends, repeating
  indefinitely until stopped manually.
* **Stop other sounds** — specify which other sounds get stopped when this
  one starts, useful for enforcing mutually exclusive playback (e.g. muting
  background music when an alert plays).
* **Chain** — automatically start a different sound when this one ends,
  for sequences like intro → main → outro with no further input needed.
* **Show on Home** — appear as a button on the Home page for manual
  triggering, and be marked as **high quality** for preferential playback
  on a high-quality-flagged audio module.

**Soundscapes** are ordered groups of sound entries that play in sequence.
Each entry can independently repeat: with repeat disabled it plays once,
with repeat enabled and a count of `0` it repeats forever, and with repeat
enabled and a positive count it repeats that many additional times.
Soundscape progression only advances when the active entry actually
finishes — unrelated sounds ending elsewhere don't affect it.

## Scenes

Beyond assigning effects to LED targets, a scene can define scene-level
behavior in its settings:

* **Kill other scenes on start** — stop specific other scenes when this one
  activates.
* **Play a trigger sound** — start a named sound automatically when the
  scene activates.
* **Stop sounds on start** — stop specific sounds when the scene activates.
* **Stop sounds on end** — stop specific sounds when the scene is removed
  or replaced.

A scene is considered "ongoing" (shown in the Home page's Active Scenes
list) if any of its effects run indefinitely (no cycle limit). A scene
where every effect has an explicit cycle count auto-completes and is
removed once all of them finish.

## Advanced: Direct Configuration

For advanced users, all of the above can be inspected or edited directly
via the persistent storage JSON, viewable on the **Storage** page. See
[docs/settings_template.py](settings_template.py) for the complete schema
reference, or the [Developer Guide](developer.md) for how the storage
system works internally.
