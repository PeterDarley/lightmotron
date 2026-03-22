# Lightmotron

A MicroPython-based lighting controller for plastic model kits, running on an ESP32. Uses NeoPixel LED strips to produce dynamic lighting effects, and exposes a web interface for control over WiFi.

## Features

* NeoPixel LED strip control for model lighting effects
* MAX7219 4-module scrolling LED matrix display
* Web server for browser-based control
* WiFi connectivity

## Hardware

* ESP-WROOM-32 development board
* NeoPixel LED strip (GPIO 32)
* MAX7219 4-module LED matrix display (SPI)

## Setup

This repo uses a git submodule for shared MicroPython libraries. After cloning, run:

```bash
git submodule init
git submodule update
```

## Uploading to the Device

```powershell
.\upload.ps1
```

To hard reset the device after uploading:

```powershell
python tools\reset_device.py COM3
```