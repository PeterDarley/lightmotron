#!/usr/bin/env pwsh
# Flashes the compiled c_project firmware to an ESP32-S3 over USB.
#
# Mirrors upload.ps1 (which pushes the MicroPython project's source files
# to a device already running MicroPython), but for the from-scratch
# ESP-IDF C port: this builds (if needed) and flashes pre-built binaries
# rather than copying source files to a running interpreter.
#
# Default action auto-detects whether webassets need including, rather
# than requiring you to remember -Full: it compares every file under
# www/ and templates/ against the timestamp of the last successful full
# flash (recorded in c_project/build/.last_full_flash_utc) and
# automatically upgrades to a full flash if anything's newer, or if no
# record of a previous full flash exists at all (e.g. first-ever flash of
# a blank board). Otherwise it does an app-only flash (idf.py app-flash):
# writes just the app binary, touching neither the bootloader/partition
# table nor either SPIFFS partition.
#
# Pass -Full to force including bootloader+partition-table+app+webassets
# regardless of the auto-detection above -- e.g. after a partition table
# change, which the timestamp check can't see coming. Neither app-flash
# nor -Full ever touches the "data" partition (settings, scenes, effects,
# colors, sounds, WiFi credentials, ...), because neither writes to it at
# all -- that's true regardless of which path runs.
$ErrorActionPreference = 'Stop'

$cProjectDir = Join-Path $PSScriptRoot 'c_project'
if (-not (Test-Path $cProjectDir)) {
    Write-Error "c_project directory not found at $cProjectDir"
    exit 1
}

# Parse arguments: optional port, -Force for a full clean rebuild, -Full to
# force bootloader+partition-table+app+webassets (overriding the
# auto-detection below), -Erase to wipe the ENTIRE flash first, -Monitor to
# attach the serial monitor after flashing.
#
# -Erase note: neither app-flash nor -Full ever touches the "data"
# (settings/scenes/etc.) or "nvs" partitions -- that's intentional so
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

# Auto-detect whether webassets need including, so a plain run never
# silently skips a www/templates change just because -Full wasn't
# remembered (the actual bug this replaced: a CSS-only fix built cleanly
# but the device kept serving the pre-fix asset because app-flash was run
# out of habit). Only runs when -Full/-Erase weren't already given
# explicitly on the command line.
$markerFile = Join-Path $cProjectDir 'build\.last_full_flash_utc'
if (-not $fullFlash) {
    $lastFullFlash = $null
    if (Test-Path $markerFile) {
        try {
            $lastFullFlash = [datetime](Get-Content $markerFile -Raw).Trim()
        } catch {
            $lastFullFlash = $null
        }
    }

    if (-not $lastFullFlash) {
        Write-Output "No record of a previous full flash -- including webassets (-Full behavior) to be safe."
        $fullFlash = $true
    } else {
        $webDirs = @((Join-Path $PSScriptRoot 'www'), (Join-Path $PSScriptRoot 'templates')) |
            Where-Object { Test-Path $_ }
        $newestWebFile = $webDirs |
            ForEach-Object { Get-ChildItem -Path $_ -Recurse -File } |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1

        if ($newestWebFile -and $newestWebFile.LastWriteTimeUtc -gt $lastFullFlash) {
            Write-Output "Detected a change under www/ or templates/ ($($newestWebFile.FullName), $($newestWebFile.LastWriteTimeUtc.ToString('u'))) since the last full flash ($($lastFullFlash.ToString('u'))) -- including webassets (-Full behavior)."
            $fullFlash = $true
        }
    }
}

# Locate and dot-source the ESP-IDF PowerShell environment (adds idf.py etc.
# to PATH for this process only).
#
# NOTE: don't invoke this script from git-bash -- MSYS's environment
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

    if ($fullFlash) {
        New-Item -ItemType Directory -Force -Path (Split-Path $markerFile) | Out-Null
        (Get-Date).ToUniversalTime().ToString('o') | Set-Content -Path $markerFile -Encoding utf8 -NoNewline
    }
}
finally {
    Pop-Location
}

if (-not $doMonitor) {
    Write-Output "Flash complete. Run with -Monitor to also attach the serial monitor (Ctrl+] to exit), e.g.:"
    Write-Output "  .\flash_c.ps1 $port -Monitor"
}
