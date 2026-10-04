# Lists serial ports that look like an ESP32 and (optionally) confirms each one
# by asking esptool for the chip ID.
#
#   .\tools\find_esp32_port.ps1           # list likely ESP32 ports
#   .\tools\find_esp32_port.ps1 -Probe    # also run esptool chip_id on each
#
# USB IDs used:
#   303A = Espressif native USB (USB-Serial/JTAG, ESP32-S3 USB-C port)
#   1A86 = WCH CH340/CH343 bridge (the UART port on YD-ESP32-S3 boards)
#   10C4 = Silicon Labs CP210x bridge (common on other dev boards)

param(
    [switch]$Probe
)

$espressifVid = '303A'
$bridgeVids   = @('1A86', '10C4')

$ports = Get-CimInstance Win32_PnPEntity |
    Where-Object { $_.Name -match '\((COM\d+)\)' } |
    ForEach-Object {
        $com = [regex]::Match($_.Name, 'COM\d+').Value
        $vid = [regex]::Match($_.DeviceID, 'VID_([0-9A-F]{4})').Groups[1].Value
        [pscustomobject]@{
            Port        = $com
            Description = $_.Name
            VID         = $vid
        }
    } |
    Sort-Object { [int]($_.Port -replace 'COM', '') }

if (-not $ports) {
    Write-Host 'No serial ports found. Is the board plugged in with a data-capable USB cable?'
    exit 1
}

$candidates = $ports | Where-Object { $_.VID -eq $espressifVid -or $bridgeVids -contains $_.VID }

Write-Host ''
Write-Host 'All serial ports:'
$ports | Format-Table Port, VID, Description -AutoSize

if (-not $candidates) {
    Write-Host 'No ESP32-style USB IDs found. Check the cable (charge-only cables have no data lines).'
    exit 1
}

Write-Host "Likely ESP32 ports: $(($candidates.Port) -join ', ')"
Write-Host '  303A = native USB port (ESP32-S3 USB-C)'
Write-Host '  1A86/10C4 = UART bridge port'

if ($Probe) {
    $esptool = Get-Command esptool.py -ErrorAction SilentlyContinue
    if (-not $esptool) {
        Write-Host 'esptool.py not found on PATH. Run this from an ESP-IDF PowerShell (idf.py environment).'
        exit 1
    }
    Write-Host ''
    foreach ($c in $candidates) {
        Write-Host "Probing $($c.Port)..."
        $out = & esptool.py --port $c.Port --connect-attempts 1 chip_id 2>&1 | Out-String
        if ($out -match 'Chip type:\s+(.+)') {
            Write-Host "  ESP32 found: $($Matches[1].Trim())" -ForegroundColor Green
        } else {
            Write-Host '  No ESP32 response on this port.' -ForegroundColor Yellow
        }
    }
}
