#!/usr/bin/env pwsh
$ErrorActionPreference = 'Stop'

# Path to mpremote in the venv
$mpremote = Join-Path $PSScriptRoot "venv\Scripts\mpremote.exe"
if (-not (Test-Path $mpremote)) {
    Write-Error "mpremote not found at $mpremote. Activate venv or install mpremote in the project's venv."
    exit 1
}

# Default COM port can be overridden by providing a first argument
$port = $args[0]
if (-not $port) { $port = 'COM3' }

& $mpremote connect $port
