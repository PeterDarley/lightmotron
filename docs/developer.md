# Developer Guide

This page is for anyone building, modifying, or contributing to
Lightmotron's firmware. If you just want to install and use the device,
you don't need anything on this page — see the [README](../README.md)
and the [Programming Guide](programming.md) instead.

## History

Lightmotron started as a MicroPython project. As of August 2026, it's been
fully rewritten in C on ESP-IDF, and that C rewrite (`c_project/`) is now
the only actively developed version — the original MicroPython
implementation is frozen and no longer receives changes. Shared assets
that both versions still use — HTML templates (`templates/`), static web
assets (`www/`) — are kept current for both, but everything below describes
the current C firmware.

## Architecture

* **Platform**: ESP-IDF (Espressif's official C SDK) on ESP32-S3, running
  FreeRTOS across both CPU cores.
* **Structure**: one ESP-IDF component per subsystem, under
  `c_project/components/` — `web` (HTTP request handlers), `webserver`
  (the HTTP server + template engine itself), `storage` (persistent JSON
  config), `lighting` (patterns/filters/scenes/animation), `leds`
  (NeoPixel driver), `audio` (YX5200 MP3 modules), `billboard` (MAX7219
  matrix display), `network` (WiFi, captive portal, mDNS, OTA), and `util`.
* **Storage**: all runtime state (settings, scenes, effects, sounds, WiFi
  credentials) lives as JSON on a dedicated LittleFS partition, kept
  separate from the web-asset partition so that flashing new firmware
  never touches user configuration.
* **Web UI**: served by a custom, purpose-built HTTP server and template
  engine (not a third-party framework) — see [Lighting Internals](internals.md#webhttp-layer).

## Where to go next

* **[Building & Flashing](building.md)** — dev environment setup, build
  scripts, and how releases (both GitHub Release OTA and the browser
  web-installer) get produced.
* **[Lighting Internals](internals.md)** — how the web layer, storage
  subsystem, lighting/animation pipeline, audio driver, and OTA updater
  actually work.
* **[Programming Guide](programming.md)** — the configuration
  reference (patterns, filters, colors, scenes) — written for end users,
  but also the closest thing to a spec for the on-disk JSON schema's
  *behavior*, as opposed to its shape.
* **[Storage Format Reference](settings_template.py)** — the full JSON
  schema, key by key.
* **[Theming Guide](theming.md)** — the CSS theming system for the web UI.
* **[Hardware Reference](hardware.md)** — audio and matrix-display wiring
  (see also [NeoPixel Wiring](neopixel-wiring.md) for LED strip wiring).
