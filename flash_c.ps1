#!/usr/bin/env pwsh
# Flashes the compiled c_project firmware to an ESP32-S3 over USB.
#
# Mirrors upload.ps1 (which pushes the MicroPython project's source files
# to a device already running MicroPython), but for the from-scratch
# ESP-IDF C port: this builds (if needed) and flashes pre-built binaries
# rather than copying source files to a running interpreter.
#
# Default action is APP-ONLY flashing (idf.py app-flash): writes just the
# app binary, touching neither the bootloader/partition table nor either
# SPIFFS partition. This is safe to run any time you've only changed C
# source -- it can never wipe the "data" partition (settings, scenes,
# effects, colors, sounds, WiFi credentials, ...), because it doesn't
# write to it at all.
#
# Pass -Full when you've also changed something under www/ or templates/
# (the "webassets" partition needs rebuilding+rewriting to pick that up),
# for a first-ever flash of a blank board, or after a partition table
# change. -Full still never touches "data" -- only "webassets" is part of
# the default flash target (see main/CMakeLists.txt's
# spiffs_create_partition_image(webassets ...)) -- but a partition table
# change specifically (not just a -Full flash) can still invalidate
# "data"'s existing contents; back up via the Status page first if in doubt.
$ErrorActionPreference = 'Stop'

$cProjectDir = Join-Path $PSScriptRoot 'c_project'
if (-not (Test-Path $cProjectDir)) {
    Write-Error "c_project directory not found at $cProjectDir"
    exit 1
}

# Parse arguments: optional port, -Force for a full clean rebuild, -Full for
# bootloader+partition-table+app+webassets (instead of the app-only
# default), -Erase to wipe the ENTIRE flash first, -Monitor to attach the
# serial monitor after flashing.
#
# -Erase note: neither the default app-flash nor -Full ever touches the
# "data" (settings/scenes/etc.) or "nvs" partitions -- that's intentional so
# deploys don't wipe user settings. But it also means a bad or stale value
# persisted in "data" survives every reflash. If a board misbehaves in a way
# a normal reflash won't clear (e.g. it was migrated from the old single-
# "storage" partition layout and "data" now overlays leftover SPIFFS
# content), -Erase runs `idf.py erase-flash` first to blank the whole chip;
# the firmware then re-seeds fresh defaults into "data"/"nvs" on next boot.
# This DOES discard all on-device settings, so use it deliberately.
$port = $null
$forceClean = $false
$fullFlash = $false
$eraseFlash = $false
$doMonitor = $false
foreach ($a in $args) {
    if ($a -eq '-Force') { $forceClean = $true }
    elseif ($a -eq '-Full') { $fullFlash = $true }
    elseif ($a -eq '-Erase') { $eraseFlash = $true }
    elseif ($a -eq '-Monitor') { $doMonitor = $true }
    elseif (-not $port) { $port = $a }
}
if (-not $port) { $port = 'COM3' }
# Erasing the whole chip removes the partition table too, so a plain
# app-flash afterwards would have nothing to flash into -- force a full
# flash whenever -Erase is used.
if ($eraseFlash) { $fullFlash = $true }

# Locate and dot-source the ESP-IDF PowerShell environment (adds idf.py etc.
# to PATH for this process only).
#
# NOTE: run this script from a plain PowerShell window, not a VS Code
# integrated terminal. VS Code's Python extension auto-activates this
# repo's venv, and stacking the ESP-IDF profile's own venv activation on
# top of that corrupts PATH (see c_project/BUILD_NOTES.md for details).
# Likewise, don't invoke this script from git-bash -- MSYS's environment
# leaks into child PowerShell processes and makes the ESP-IDF profile
# script treat its own informational Mingw/MSys warning as fatal.
$idfProfileCandidates = @(
    'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
)
$idfProfile = $idfProfileCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $idfProfile) {
    Write-Error "Could not find the ESP-IDF PowerShell profile script. Expected one of: $($idfProfileCandidates -join ', ')"
    exit 1
}
. $idfProfile

Push-Location $cProjectDir
try {
    if ($forceClean) {
        Write-Output "Full clean rebuild requested (-Force)..."
        idf.py fullclean
        if ($LASTEXITCODE -ne 0) {
            Write-Error "fullclean failed."
            exit 1
        }
    }

    if ($eraseFlash) {
        Write-Output "Erasing ENTIRE flash (all partitions, including data + nvs) on ${port}..."
        idf.py -p $port erase-flash
        if ($LASTEXITCODE -ne 0) {
            Write-Error "erase-flash failed."
            exit 1
        }
    }

    # Both build first (incrementally, unless -Force triggered a fullclean
    # above). `flash` writes bootloader + partition table + app + webassets;
    # `app-flash` writes only the app binary -- neither touches "data".
    $flashAction = if ($fullFlash) { 'flash' } else { 'app-flash' }
    $flashArgs = @('-p', $port, $flashAction)
    if ($doMonitor) { $flashArgs += 'monitor' }

    Write-Output "Building (if needed) and flashing ($flashAction) to ${port}..."
    idf.py @flashArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build/flash failed. See output above."
        exit 1
    }
}
finally {
    Pop-Location
}

if (-not $doMonitor) {
    Write-Output "Flash complete. Run with -Monitor to also attach the serial monitor (Ctrl+] to exit), e.g.:"
    Write-Output "  .\flash_c.ps1 $port -Monitor"
}
