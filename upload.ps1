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
if (-not $port) { $port = 'COM4' }

# NOTE: This script does NOT reboot the device after upload.
# The device must already be at a quiet REPL before running this script.
# To stop the webserver from the REPL: WebServer().stop()
# Reboot manually with: python tools\reset_device.py COM4 2

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

$directories = @('lib', 'www', 'templates', "web")

foreach ($dir in $directories) {
    $dirPath = Join-Path $PSScriptRoot $dir
    if (Test-Path $dirPath) {
        Write-Output "  $dir/"
        $cpArgs.Add('+')
        $cpArgs.AddRange([string[]]@('fs', 'cp', '-r', $dirPath, ':'))
    }
}

& $mpremote @cpArgs

# Soft reset device to reload boot.py/main.py with new files.
# A fresh connect without --no-soft-reset will trigger soft reset on disconnect.
Write-Output "Soft resetting device..."
$ErrorActionPreference = 'Continue'
& $mpremote connect $port exec "pass" 2>$null
$ErrorActionPreference = 'Stop'

Write-Output "Upload complete."