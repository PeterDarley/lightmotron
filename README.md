# Lightmotron

A lighting controller for plastic model kits, running on an ESP32-S3. Uses
NeoPixel LED strips to produce dynamic lighting effects, with optional MP3
sound playback — all configured and controlled from a web interface over
WiFi, no app or account required.

## License

You are welcome to use this project in products, research, kits, and services.
Selling the software itself, or minor variations of it, as a standalone product is against the spirit of the project.

```
This software is licensed under GPLv3 with the following additional permission:
You may sell products or services that include this software, provided the software is not sold independently.
Distribution of the software itself, except as part of a larger product or service, is not permitted.
```

## Features

* NeoPixel LED strip control with 15 built-in animation patterns and 11 post-processing filters
* Combine effects into named **scenes**, triggered manually or automatically
* Optional MP3 sound playback (looping, chaining, soundscapes) synced to scenes
* Multiple named **Models**, for controlling more than one kit from a single device
* Browser-based control interface — no app install required
* WiFi connectivity with a guided first-time setup (captive portal)
* Firmware updates over WiFi, right from the web interface

## Install

<a id="install"></a>

**[peterdarley.github.io/lightmotron](https://peterdarley.github.io/lightmotron/)**
— flashes the firmware directly from your browser over USB. No drivers,
no command line, no account.

1. Connect your ESP32-S3 board to your computer with a USB-C **data**
   cable (not a charge-only cable).
2. Open the link above in **Google Chrome** or **Microsoft Edge** (this
   uses the [Web Serial API](https://developer.chrome.com/docs/capabilities/serial),
   which only those browsers support).
3. Click **Install Lightmotron** and select your device's serial port when
   prompted.

That's it — the page writes everything a blank board needs. Once it's
done, unplug the USB cable (or leave it connected for power) and continue
to [First Boot](#first-boot--wifi-setup) below.

Already have a Lightmotron device and just want to update it? Use
**Setup → Updates** in the web interface instead — see
[Firmware Updates](#firmware-updates) below.

## Hardware

* **Microcontroller**: YD-ESP32-S3 board (ESP32-S3-WROOM-1 N16R8 module —
  16MB flash, 8MB octal PSRAM)
* **Lighting**: a NeoPixel-compatible LED strip (default GPIO 4;
  configurable in System Settings) — see the
  [NeoPixel Wiring Diagram](docs/neopixel-wiring.md)
* **Sound (optional)**: up to 3 YX5200-based MP3 player modules with a
  small amplifier and speakers — see the [Hardware Reference](docs/hardware.md)

## First Boot — WiFi Setup

On first boot (or whenever WiFi credentials are missing or fail to connect), the device starts a **captive portal** access point:

1. Look for a WiFi network called **`lightmotron-setup`** on your phone or laptop and connect to it.
2. Your device should automatically open the portal page. If not, navigate to `http://192.168.4.1/`.
3. The page shows a list of nearby WiFi networks sorted by signal strength. Select yours, enter the password, then tap **Save & Connect**. If your network isn't listed, choose **Other** to enter the SSID manually, or use the **rescan** link if you don't see it yet.
4. The device reboots and joins your network. You can then access it at `http://lightmotron.local/` (or `http://<hostname>.local/` if you've set a custom hostname). If `.local` addresses don't resolve on your network (common on Android), try `http://lightmotron.lan/`, `http://lightmotron.home/`, or just `http://lightmotron/`.

SSID matching is case-insensitive, so credentials stored with different capitalization will still connect.

Credentials are stored persistently, so this only needs to be done once. To change them later, use **System Settings** on the Setup page.

## Web Interface

Once the device is on your network, open its address in a browser.

### Home Page

Control animation playback and trigger scenes from here day-to-day: start/stop the lighting engine, switch between scenes, play sounds and soundscapes. The Active Scenes list shows both ongoing scenes and any immediate (one-shot trigger) scenes while they're running.

![Home page](docs/screenshots/home.png)

### Setup Page

Configure everything: custom colors, named LED ranges, effects, filters, scenes, sounds, soundscapes, models, and the interface theme. See the **[Programming Guide](docs/programming.md)** for a complete walkthrough of every option and what it does.

![Setup page](docs/screenshots/setup.png)
![Custom Colors](docs/screenshots/setup-colors.png)
![Named Ranges](docs/screenshots/setup-ranges.png)
![Filters](docs/screenshots/setup-filters.png)
![Effects](docs/screenshots/setup-effects.png)
![Scenes](docs/screenshots/setup-scenes.png)
![Theme picker](docs/screenshots/setup-theme.png)

System Settings on this page also covers WiFi credentials, hostname, NeoPixel strip wiring, and audio module pin assignments — changes there take effect on the next reboot.

**IP address announcement**: if audio modules are configured and the device's IP address has changed since it last booted, it announces the new address by voice automatically (each octet's digits, repeated once after 15 seconds). Use **Mark Current IP as Announced** on the Setup page if you don't want to hear it again for the current address.

### Status Page

Monitor system health: memory and storage usage, networking details, animation state, the reason for the last reboot, and the responsiveness of any configured audio modules. Download or restore your entire configuration as a JSON backup file from here — restoring never overwrites WiFi credentials, so a backup is safe to move between devices.

![Status page](docs/screenshots/status.png)

### Storage Page

View the raw JSON configuration underlying everything else on the device, for advanced inspection or manual editing. WiFi passwords are masked as `***` here for safety.

## Firmware Updates

Setup → Updates checks the configured GitHub repository (defaults to this
one) for a newer release and, when you confirm, downloads and installs it
— no cable required. Your settings, scenes, and sounds are untouched by an
update; only the firmware itself changes.

## History

Lightmotron began as a MicroPython project. It's since been fully rewritten in C for better performance and reliability, and that's what you'll get from the installer above — the original MicroPython version is no longer maintained. If you're curious about the internals or want to build the firmware yourself, see the [Developer Guide](docs/developer.md).

## Documentation

* **[Programming Guide](docs/programming.md)** — how to configure effects, filters, sounds, and scenes
* **[Developer Guide](docs/developer.md)** — architecture, building from source, and links to the rest of the technical docs
