"""
Hardware reset an ESP32 devkit via the serial port's RTS pin.
On standard devkits (CP2102/CH340), RTS is connected to the EN (reset) pin
via a transistor circuit -- toggling it resets the board without needing a REPL.

Usage:
    python tools/reset_device.py [PORT] [WAIT_SECONDS]

    PORT         - serial port (default: COM3)
    WAIT_SECONDS - seconds to wait after reset for boot to complete (default: 2)
"""

import serial
import time
import sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0

print(f"Hardware resetting {port}...", flush=True)
with serial.Serial(port, 115200) as s:
    # Standard esptool-style reset: pull EN low, then release
    s.setDTR(False)
    s.setRTS(False)
    time.sleep(0.05)
    s.setRTS(True)   # EN low → reset starts
    time.sleep(0.1)
    s.setRTS(False)  # EN released → board boots normally

time.sleep(wait)
print(f"Done — waited {wait:.1f}s for boot.", flush=True)
