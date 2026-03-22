#!/usr/bin/env pwsh
$ErrorActionPreference = 'Stop'

# Path to mpremote in the venv
$mpremote = Join-Path $PSScriptRoot "venv\Scripts\mpremote.exe"
if (-not (Test-Path $mpremote)) {
    Write-Error "mpremote not found at $mpremote. Run: pip install mpremote"
    exit 1
}

# Parse arguments: optional port.
$port = $null
foreach ($a in $args) {
    if (-not $port) { $port = $a }
}
if (-not $port) { $port = 'COM3' }

# NOTE: This script does NOT reboot the device after upload.
# The device must already be at a quiet REPL before running this script.
# To stop the webserver from the REPL: WebServer().stop()
# Reboot manually with: python tools\reset_device.py COM3 2

# Copy project files to device in a single mpremote session using chained commands.
Write-Output "Uploading to $port..."

# Build a single chained mpremote invocation: connect once, copy everything.
# mpremote supports chaining commands with '+' in a single session.
$cpArgs = [System.Collections.Generic.List[string]]::new()
$cpArgs.Add('connect')
$cpArgs.Add($port)

$first = $true
foreach ($f in (Get-ChildItem -Path $PSScriptRoot -Filter *.py)) {
    Write-Output "  $($f.Name)"
    if (-not $first) { $cpArgs.Add('+') }
    $cpArgs.AddRange([string[]]@('fs', 'cp', $f.FullName, ':'))
    $first = $false
}

if (Test-Path (Join-Path $PSScriptRoot 'lib')) {
    Write-Output "  lib/"
    $cpArgs.Add('+')
    $cpArgs.AddRange([string[]]@('fs', 'cp', '-r', (Join-Path $PSScriptRoot 'lib'), ':'))
}

if (Test-Path (Join-Path $PSScriptRoot 'www')) {
    Write-Output "  www/"
    $cpArgs.Add('+')
    $cpArgs.AddRange([string[]]@('fs', 'cp', '-r', (Join-Path $PSScriptRoot 'www'), ':'))
}

if (Test-Path (Join-Path $PSScriptRoot 'templates')) {
    Write-Output "  templates/"
    $cpArgs.Add('+')
    $cpArgs.AddRange([string[]]@('fs', 'cp', '-r', (Join-Path $PSScriptRoot 'templates'), ':'))
}

& $mpremote @cpArgs

# Soft reset device to reload boot.py/main.py with new files.
# A fresh connect without --no-soft-reset will trigger soft reset on disconnect.
Write-Output "Soft resetting device..."
$ErrorActionPreference = 'Continue'
& $mpremote connect $port exec "pass" 2>$null
$ErrorActionPreference = 'Stop'

Write-Output "Upload complete."