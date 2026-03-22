"""neopixel_test

Simple demo for `lib/leds.py`.
Run on the device to exercise the LEDs wrapper.
"""

import time

try:
    from lib.leds import LEDs
except Exception as e:
    print('LEDs module not available:', e)
    raise


def hex_to_rgb(h):
    h = h.strip()
    if h.startswith('#'):
        h = h[1:]
    if len(h) != 6:
        return (0, 0, 0)
    try:
        return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))
    except Exception:
        return (0, 0, 0)


def show_color(strip, color, delay=0.6):
    strip.fill(color)
    strip.show()
    time.sleep(delay)


def rainbow_cycle(strip, cycles=1, delay=0.002):
    n = strip.n
    for j in range(256 * cycles):
        for i in range(n):
            color = strip.wheel((int(i * 256 / n) + j) & 255)
            strip.set(i, color)
        strip.show()
        time.sleep(delay)


def brightness_demo(strip):
    for b in (0.1, 0.3, 0.6, 1.0):
        strip.brightness = b
        strip.fill((255, 100, 0))
        strip.show()
        time.sleep(1)


def main():
    strip = LEDs(brightness=0.5)

    try:
        print('Demo: red, green, blue')
        show_color(strip, (255, 0, 0))
        show_color(strip, (0, 255, 0))
        show_color(strip, (0, 0, 255))

        print('Demo: brightness levels')
        brightness_demo(strip)

        print('Demo: rainbow')
        rainbow_cycle(strip, cycles=2, delay=0.02)

    except KeyboardInterrupt:
        pass
    finally:
        strip.clear()
        strip.show()
        print('Done')


if __name__ == '__main__':
    main()
