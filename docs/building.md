# Building & Flashing (Developer Guide)

This page is for building the firmware from source. If you just want to
install Lightmotron on a device, use the [web installer](../README.md#install)
instead — nothing here is required for that.

## Prerequisites

* [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) installed, targeting `esp32s3`.
* A plain Windows Terminal / PowerShell window — **not** a VS Code
  integrated terminal, and **not** git-bash / MSYS. Both corrupt the build
  environment before `idf.py` ever runs:
  * VS Code's Python extension auto-activates this repo's `venv`; stacking
    the ESP-IDF profile script's own venv activation on top of that
    corrupts `PATH` (observed truncated to ~1200 chars instead of the
    expected 3000-5000+), which breaks `cmake`/`idf.py` resolution.
  * MSYS's environment (`MSYSTEM` etc.) leaks into a `powershell.exe`
    child process spawned from git-bash, and makes the ESP-IDF profile
    script's own Mingw/MSys detection treat its normal informational
    warning as fatal.

## First-time setup

```powershell
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
cd c_project
idf.py build
```

(Adjust the profile script path to wherever your ESP-IDF install placed
it.) This dot-sources the ESP-IDF environment into the current shell
session (adds `idf.py` etc. to `PATH` for that process only — it doesn't
persist), then does a normal build.

## Everyday builds

```powershell
.\flash_c.ps1 [COMx] [-Full] [-Erase] [-Monitor] [-Force]
```

* No flags: builds (if needed) and flashes **just the app binary**
  (`idf.py app-flash`). Safe to run any time you've only changed C source —
  it never touches the `data` partition (settings, scenes, effects, colors,
  sounds, WiFi credentials, ...) or the `webassets` partition.
* `-Full`: also writes bootloader, partition table, and `webassets` — needed
  after changing anything under `www/`/`templates/`, for a first flash of a
  blank board, or after a partition-table change.
* `-Erase`: wipes the **entire** chip first (`idf.py erase-flash`), including
  settings. Only needed to recover from a corrupted/incompatible `data`
  partition (e.g. after a partition-table change) — back up via the
  Status page first if the device has real data on it.
* `-Monitor`: attaches the serial monitor after flashing (`Ctrl+]` to exit).
* `-Force`: runs `idf.py fullclean` before building.

Default port is `COM3`; pass a different one as the first positional
argument.

## Releasing a build

Two separate output paths, for two separate distribution methods:

```powershell
.\build_c_release.ps1
```
Builds and copies a single versioned app binary into `c_project/releases/`,
ready to attach to a GitHub Release — this is what the in-app OTA updater
(Setup → Updates) checks against. Doesn't touch git; creating the release
and attaching the file is a separate, deliberate step.

```powershell
.\build_web_install.ps1
```
Builds and stages all four images a **blank chip** needs (bootloader,
partition table, app, webassets) into `deployment/`, along with a
`manifest.json` that the [web installer](../README.md#install) reads. Also
doesn't touch git — committing and pushing `deployment/` (served via
GitHub Pages) is a separate step.

Both scripts run `idf.py reconfigure` first, since the firmware version
string (`git describe`) is computed at CMake *configure* time, not on every
build — without this, a version bump after tagging wouldn't be picked up
by a plain `idf.py build`.

## See also

* [Developer Guide](developer.md) — architecture overview and links to the
  rest of the internals documentation.
* [Lighting Internals](internals.md) — implementation reference for the
  lighting/storage/web subsystems.
