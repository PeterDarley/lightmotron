#!/usr/bin/env pwsh
# Builds c_project and stages the browser-install package under deployment/,
# served via GitHub Pages at deployment/index.html (ESP Web Tools). Unlike
# build_c_release.ps1 (which stages a single app .bin for OTA), a browser
# install targets a BLANK chip and needs all four images a full flash
# writes: bootloader, partition table, app, and web assets.
#
# This does NOT touch git -- copying files into deployment/ still leaves
# committing/pushing as a separate, deliberate step.
$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
$cProjectDir = Join-Path $repoRoot 'c_project'
$deploymentDir = Join-Path $repoRoot 'deployment'
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
    # Force a CMake reconfigure so FIRMWARE_VERSION (git describe, baked in
    # at configure time -- see build_c_release.ps1's identical comment)
    # reflects the current commit, not whatever was configured last.
    Write-Output "Reconfiguring (to pick up current git describe)..."
    idf.py reconfigure
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Reconfigure failed. See output above."
        exit 1
    }

    Write-Output "Building c_project (bootloader + partition table + app + webassets)..."
    idf.py build
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed. See output above."
        exit 1
    }

    # Offsets match c_project/partitions.csv and the flash command idf.py
    # itself prints after a build -- bootloader and partition-table offsets
    # are ESP-IDF's standard locations for this chip/flash-size combo.
    $parts = @(
        @{ Src = 'build\bootloader\bootloader.bin';        Dest = 'bootloader.bin';       Offset = 0 }        # 0x0
        @{ Src = 'build\partition_table\partition-table.bin'; Dest = 'partition-table.bin'; Offset = 32768 }    # 0x8000
        @{ Src = 'build\lightmotron.bin';                  Dest = 'lightmotron.bin';      Offset = 65536 }    # 0x10000
        @{ Src = 'build\webassets.bin';                    Dest = 'webassets.bin';        Offset = 6356992 } # 0x610000
    )

    foreach ($part in $parts) {
        $srcPath = Join-Path $cProjectDir $part.Src
        if (-not (Test-Path $srcPath)) {
            Write-Error "Build succeeded but expected output is missing: $srcPath"
            exit 1
        }
    }

    if (-not (Test-Path $deploymentDir)) {
        New-Item -ItemType Directory -Path $deploymentDir | Out-Null
    }

    foreach ($part in $parts) {
        $srcPath = Join-Path $cProjectDir $part.Src
        $destPath = Join-Path $deploymentDir $part.Dest
        Copy-Item -Path $srcPath -Destination $destPath -Force
        Write-Output "  staged $($part.Dest)"
    }

    # Same git describe invocation as build_c_release.ps1, so the version
    # shown on the install page matches what the flashed firmware itself
    # reports (visible on the device's Status page).
    $version = & git -C $repoRoot describe --always --tags --dirty 2>$null
    if (-not $version) { $version = 'unknown' }
    $version = $version.Trim()

    $partsJson = ($parts | ForEach-Object {
        '        { "path": "' + $_.Dest + '", "offset": ' + $_.Offset + ' }'
    }) -join ",`n"

    $manifest = @"
{
  "name": "Lightmotron",
  "version": "$version",
  "firmware_source": "c_project (ESP-IDF $version)",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
$partsJson
      ]
    }
  ]
}
"@
    # Windows PowerShell 5.1's Set-Content -Encoding utf8 writes a BOM even
    # when explicitly requested (no utf8NoBOM option exists pre-PS6) -- a
    # leading BOM in manifest.json risks tripping up ESP Web Tools' JSON
    # parsing, so write it out explicitly without one.
    $manifestPath = Join-Path $deploymentDir 'manifest.json'
    [System.IO.File]::WriteAllText($manifestPath, $manifest, (New-Object System.Text.UTF8Encoding $false))

    Write-Output ""
    Write-Output "Web-install package staged in $deploymentDir (version: $version)."
    Write-Output "Commit and push deployment/ (and the repo-root index.html redirect, if"
    Write-Output "you haven't already) to publish via GitHub Pages -- that's a separate,"
    Write-Output "deliberate step."
}
finally {
    Pop-Location
}
