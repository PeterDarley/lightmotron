"""
Billboard test — run this in the REPL or via mpremote to verify wiring.

Usage (REPL):
    exec(open('billboard_test.py').read())

Or from host:
    mpremote connect COM3 run billboard_test.py
"""

import time
from billboard import Billboard

print("Billboard test starting...")

# Construct using settings (reads settings.BILLBOARD)
bb = Billboard.from_settings(debug=True)

# --- Test 1: fill all pixels on -------
print("Test 1: all pixels on")
bb.matrix.fill(1)
bb.show()
time.sleep(1)

# --- Test 2: all pixels off -----------
print("Test 2: all pixels off")
bb.clear()
time.sleep(0.5)

# --- Test 3: smiley-face pattern ------
print("Test 3: smiley face")
SMILEY = [
    0b00111100,
    0b01000010,
    0b10100101,
    0b10000001,
    0b10100101,
    0b10011001,
    0b01000010,
    0b00111100,
] * bb._num
bb.fill_pattern(SMILEY)
time.sleep(2)

# --- Test 4: static text --------------
print("Test 4: static text 'Hi'")
bb.static_text("Hi")
time.sleep(2)

# --- Test 5: scrolling text -----------
print("Test 5: scrolling text")
bb.scroll_text("Hello World!  ", delay_ms=60)

# --- Done -----------------------------
bb.clear()
print("Billboard test complete.")
