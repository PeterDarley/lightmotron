# Voice control — idea notes

**Status as of 2026-08-03: idea stage only.** No hardware purchased, no
code written. Captured here so the reasoning survives across sessions if
this gets picked up later.

## Feasibility

ESP32-**S3** (unlike plain ESP32/ESP32-S2) has the vector instructions
Espressif's [ESP-SR](https://github.com/espressif/esp-sr) component needs
to run entirely on-device: WakeNet (wake-word spotting) + MultiNet
(small-vocabulary command recognition, tens of short phrases, not open
-ended speech). No cloud dependency. The board in use (YD-ESP32-S3,
N16R8 — 16MB flash / 8MB PSRAM) is comfortably within what ESP-SR expects
for MultiNet's models. Full detail in the prior conversation; this file
only covers the hardware side.

## Recommended microphone: INMP441

Cheap (~$2-5 for a breakout board from the usual electronics sellers),
digital I2S output, omnidirectional, widely used — including in
Espressif's own ESP-SR reference projects, so tooling/example code
targets it directly.

- **Alternative**: ICS-43434 — same class of I2S MEMS mic, somewhat
  better SNR, also seen in ESP-SR reference designs, still cheap. Worth
  it if voice recognition accuracy turns out marginal with the INMP441;
  not worth starting there.
- **Avoid** analog electret mic + amp modules (e.g. MAX9814) for this —
  they'd need an ADC channel and more attention to analog signal quality/
  noise, for no benefit over a digital I2S mic that ESP-SR is already
  built to expect.

## Other hardware needed

- **Nothing beyond the mic itself and wiring.** I2S digital output means
  no separate ADC, no preamp — the breakout board's output goes straight
  into the ESP32-S3's I2S peripheral.
- Wiring: 3 GPIOs (BCLK/SCK, WS/LRCLK, SD/DOUT) + 3.3V + GND. ESP32-S3's
  GPIO matrix lets any free pins serve as I2S — no dedicated I2S-only
  pins required. **Need to check which GPIOs are actually free** against
  the existing pin map (GPIO4 → LED data, plus whatever's used for the
  YX5200 UART(s) and anything else already assigned) before picking pins.
- Power draw is a few mA at 3.3V — negligible next to the LED strip's
  draw (see `LED_HARDWARE_NOTES.md`); no power-supply headroom concern.
- Physical placement: keep the mic away from the LED level-shifter/data
  line and the DFPlayer's speaker output, to avoid picking up switching
  noise electrically or the model's own sound output acoustically.

## Next steps if this gets pursued

- Prototype the mic + ESP-SR wake-word/command recognition standalone
  first (confirm recognition accuracy and flash/PSRAM/CPU headroom
  alongside the existing lighting/audio/web stack) before wiring
  recognized commands into scene triggers.
- Scope note: this would touch `c_project` only, consistent with the
  Python side being frozen (see `.github/copilot-instructions.md`).
