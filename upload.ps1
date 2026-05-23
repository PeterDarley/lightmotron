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

function Invoke-MpremoteWithRetry {
    param(
        [string[]]$MpArgs,
        [string]$Description,
        [int]$MaxAttempts = 3
    )

    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        $output = @()
        $exitCode = 1
        $oldNativePref = $null
        $hasNativePref = $false
        if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -Scope Global -ErrorAction SilentlyContinue) {
            $hasNativePref = $true
            $oldNativePref = $Global:PSNativeCommandUseErrorActionPreference
            $Global:PSNativeCommandUseErrorActionPreference = $false
        }

        try {
            $output = & $mpremote @MpArgs 2>&1
            $exitCode = $LASTEXITCODE
        }
        catch {
            $output = @($_.ToString())
            $exitCode = 1
        }
        finally {
            if ($hasNativePref) {
                $Global:PSNativeCommandUseErrorActionPreference = $oldNativePref
            }
        }

        if ($output) {
            $output | ForEach-Object { Write-Output $_ }
        }

        if ($exitCode -eq 0) {
            return $true
        }

        $joined = ($output | Out-String)
        $isTransportError = ($joined -match 'Error with transport') -or ($joined -match 'could not enter raw repl')
        if ($isTransportError -and $attempt -lt $MaxAttempts) {
            Write-Warning "$Description failed due to transport/raw REPL sync (attempt $attempt/$MaxAttempts). Retrying..."
            [System.Threading.Thread]::Sleep(350)
            continue
        }

        Write-Warning "$Description failed on attempt $attempt/$MaxAttempts"
        return $false
    }

    return $false
}

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

# Files that exist locally but must never be uploaded to the device.
$excludeUploadPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
[void]$excludeUploadPaths.Add('lib/licence')
[void]$excludeUploadPaths.Add('lib/README.md')

foreach ($f in (Get-ChildItem -Path $PSScriptRoot -Filter *.py)) {
    $newManifest[$f.Name] = (Get-FileHash -Path $f.FullName -Algorithm MD5).Hash
}

$directories = @('lib', 'www', 'templates', 'web')
foreach ($dir in $directories) {
    $dirPath = Join-Path $PSScriptRoot $dir
    if (Test-Path $dirPath) {
        foreach ($f in (Get-ChildItem -Path $dirPath -File -Recurse)) {
            $relativePath = $f.FullName.Substring($PSScriptRoot.Length + 1).Replace('\', '/')
            if (-not $excludeUploadPaths.Contains($relativePath)) {
                $newManifest[$relativePath] = (Get-FileHash -Path $f.FullName -Algorithm MD5).Hash
            }
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

# Always refresh critical runtime modules to recover from partial/stale remote
# files that may have been left behind by interrupted transfers.
$alwaysUploadPaths = @('boot.py', 'main.py', 'lib/audio.py', 'lib/sounds.py')
foreach ($criticalPath in $alwaysUploadPaths) {
    if ($newManifest.ContainsKey($criticalPath) -and -not $changedFiles.Contains($criticalPath)) {
        $changedFiles.Add($criticalPath)
    }
}

if ($changedFiles.Count -eq 0) {
    Write-Output "No files changed since last upload."
    exit 0
}

Write-Output "Uploading $($changedFiles.Count) changed file(s) to ${port}..."

# Try to quiet the runtime before file copy to reduce serial output during raw REPL operations.
$quietCode = @"
import gc
try:
    from webserver import WebServer
    WebServer().stop()
except Exception:
    pass
gc.collect()
"@
[void](Invoke-MpremoteWithRetry -MpArgs @('connect', $port, 'resume', 'exec', $quietCode) -Description 'pre-upload quiet step' -MaxAttempts 2)

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
$cpArgs.Add('resume')

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

if (-not (Invoke-MpremoteWithRetry -MpArgs $cpArgs -Description 'file upload' -MaxAttempts 3)) {
    Write-Error "Upload failed: mpremote could not complete file copy."
    exit 1
}

# Save updated manifest on success.
$newManifest | ConvertTo-Json | Set-Content $manifestPath

# Record the deployed commit SHA to .ota_deployed_commit.json for OTA tracking.
try {
    $commitSha = & git rev-parse HEAD 2>&1
    if ($LASTEXITCODE -eq 0 -and $commitSha) {
        $commitSha = $commitSha.Trim()
        Write-Output "Recording deployed commit: $commitSha"
        
        # Execute Python on device to write the commit file (use double quotes for interpolation).
        $pythonCode = "import json`nwith open('.ota_deployed_commit.json', 'w') as f:`n json.dump({'commit_sha': '$commitSha'}, f)"
        if (-not (Invoke-MpremoteWithRetry -MpArgs @('connect', $port, 'resume', 'exec', $pythonCode) -Description 'commit marker write' -MaxAttempts 2)) {
            Write-Warning "Could not write deployed commit marker to device"
        }
    }
    else {
        Write-Warning "Could not get git commit SHA"
    }
}
catch {
    Write-Warning "Error recording deployed commit: $_"
}

# Soft reset device to reload boot.py/main.py with new files.
# A fresh connect without --no-soft-reset will trigger soft reset on disconnect.
Write-Output "Soft resetting device..."
$ErrorActionPreference = 'Continue'
& $mpremote connect $port exec "pass" 2>$null
$ErrorActionPreference = 'Stop'

Write-Output "Upload complete."