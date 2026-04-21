#!/usr/bin/env pwsh
$ErrorActionPreference = 'Stop'

# Path to mpremote in the venv
$mpremote = Join-Path $PSScriptRoot "venv\Scripts\mpremote.exe"
if (-not (Test-Path $mpremote)) {
    Write-Error "mpremote not found at $mpremote. Run: pip install mpremote"
    exit 1
}

# Parse arguments: optional port, -Force for full upload.
$port = $null
$forceUpload = $false
foreach ($a in $args) {
    if ($a -eq '-Force') { $forceUpload = $true }
    elseif (-not $port) { $port = $a }
}
if (-not $port) { $port = 'COM4' }

# NOTE: This script does NOT reboot the device after upload.
# The device must already be at a quiet REPL before running this script.
# To stop the webserver from the REPL: WebServer().stop()
# Reboot manually with: python tools\reset_device.py COM4 2

$manifestPath = Join-Path $PSScriptRoot ".upload_manifest.json"

# Load previous manifest for incremental upload.
$oldManifest = @{}
if (-not $forceUpload -and (Test-Path $manifestPath)) {
    $json = Get-Content $manifestPath -Raw | ConvertFrom-Json
    foreach ($prop in $json.PSObject.Properties) {
        $oldManifest[$prop.Name] = $prop.Value
    }
}

# Collect all uploadable files and compute MD5 hashes.
$newManifest = @{}

foreach ($f in (Get-ChildItem -Path $PSScriptRoot -Filter *.py)) {
    $newManifest[$f.Name] = (Get-FileHash -Path $f.FullName -Algorithm MD5).Hash
}

$directories = @('lib', 'www', 'templates', 'web')
foreach ($dir in $directories) {
    $dirPath = Join-Path $PSScriptRoot $dir
    if (Test-Path $dirPath) {
        foreach ($f in (Get-ChildItem -Path $dirPath -File -Recurse)) {
            $relativePath = $f.FullName.Substring($PSScriptRoot.Length + 1).Replace('\', '/')
            $newManifest[$relativePath] = (Get-FileHash -Path $f.FullName -Algorithm MD5).Hash
        }
    }
}

# Determine which files have changed.
$changedFiles = [System.Collections.Generic.List[string]]::new()
foreach ($path in $newManifest.Keys) {
    if (-not $oldManifest.ContainsKey($path) -or $oldManifest[$path] -ne $newManifest[$path]) {
        $changedFiles.Add($path)
    }
}

if ($changedFiles.Count -eq 0) {
    Write-Output "No files changed since last upload."
    exit 0
}

Write-Output "Uploading $($changedFiles.Count) changed file(s) to ${port}..."

# Collect unique remote directories that need to exist.
$remoteDirs = [System.Collections.Generic.HashSet[string]]::new()
foreach ($path in $changedFiles) {
    $parts = $path.Split('/')
    for ($i = 1; $i -lt $parts.Length; $i++) {
        [void]$remoteDirs.Add(($parts[0..($i-1)] -join '/'))
    }
}

# Build a single chained mpremote invocation: connect once, copy everything.
# mpremote supports chaining commands with '+' in a single session.
$cpArgs = [System.Collections.Generic.List[string]]::new()
$cpArgs.Add('connect')
$cpArgs.Add($port)

# Ensure remote directories exist via MicroPython (handles already-existing dirs).
if ($remoteDirs.Count -gt 0) {
    $sortedDirs = $remoteDirs | Sort-Object { $_.Length }
    $mkdirLines = @('import os')
    foreach ($d in $sortedDirs) {
        $mkdirLines += "try:`n os.mkdir('$d')`nexcept OSError:`n pass"
    }
    $mkdirCode = $mkdirLines -join "`n"
    $cpArgs.Add('+')
    $cpArgs.AddRange([string[]]@('exec', $mkdirCode))
}

# Chain individual file copies.
foreach ($path in $changedFiles) {
    $localPath = Join-Path $PSScriptRoot ($path.Replace('/', '\'))
    Write-Output "  $path"
    $cpArgs.Add('+')
    $cpArgs.AddRange([string[]]@('fs', 'cp', $localPath, ":$path"))
}

& $mpremote @cpArgs

# Save updated manifest on success.
$newManifest | ConvertTo-Json | Set-Content $manifestPath

# Soft reset device to reload boot.py/main.py with new files.
# A fresh connect without --no-soft-reset will trigger soft reset on disconnect.
Write-Output "Soft resetting device..."
$ErrorActionPreference = 'Continue'
& $mpremote connect $port exec "pass" 2>$null
$ErrorActionPreference = 'Stop'

Write-Output "Upload complete."