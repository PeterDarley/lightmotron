#!/usr/bin/env pwsh
# Builds the c_project firmware and stages a versioned copy of the app
# binary under c_project/releases/, ready to attach to a GitHub Release
# for OTA (components/network/ota_update.c checks the repo's latest
# release for a ".bin" asset).
#
# This does NOT flash anything or touch GitHub -- it only builds and
# copies a file. Creating the actual GitHub Release (and attaching the
# .bin) is a separate, deliberate step for a human to do.
$ErrorActionPreference = 'Stop'

$cProjectDir = Join-Path $PSScriptRoot 'c_project'
if (-not (Test-Path $cProjectDir)) {
    Write-Error "c_project directory not found at $cProjectDir"
    exit 1
}

# Locate and dot-source the ESP-IDF PowerShell environment (adds idf.py etc.
# to PATH for this process only).
#
# NOTE: run this script from a plain PowerShell window, not a VS Code
# integrated terminal -- see c_project/BUILD_NOTES.md for why.
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
    Write-Output "Building c_project..."
    idf.py build
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed. See output above."
        exit 1
    }

    $appBin = Join-Path $cProjectDir 'build\lightmotron.bin'
    if (-not (Test-Path $appBin)) {
        Write-Error "Build succeeded but $appBin wasn't produced."
        exit 1
    }

    # Same `git describe` invocation as components/network/CMakeLists.txt's
    # FIRMWARE_VERSION, so the filename matches what the running firmware
    # will report as its own version.
    $version = & git describe --always --tags --dirty 2>$null
    if (-not $version) { $version = 'unknown' }
    $version = $version.Trim()

    $releasesDir = Join-Path $cProjectDir 'releases'
    if (-not (Test-Path $releasesDir)) {
        New-Item -ItemType Directory -Path $releasesDir | Out-Null
    }

    $outFile = Join-Path $releasesDir "lightmotron-$version.bin"
    Copy-Item -Path $appBin -Destination $outFile -Force

    Write-Output "Release binary ready: $outFile"
    Write-Output "Firmware will report its version as: $($version -replace '^[vV]', '')"
    Write-Output ""
    Write-Output "To publish for OTA, attach this file to a GitHub Release tagged"
    Write-Output "'$version' (or matching tag), e.g.:"
    Write-Output "  gh release create $version `"$outFile`" --title `"$version`""
}
finally {
    Pop-Location
}
