# Copilot Instructions

This file is the single source-of-truth for notes the user asks Copilot to "note".

Guideline:
- When the user says "note ..." or asks you to "note something", add it in this file.

## Notes

### Documentation must be kept in sync
Whenever code changes are made, update the relevant documentation (README.md, docs/*.md, docs/settings_template.py) in the same session. Do not leave documentation stale after a code change.

### Deployment scripts
Do not use `upload.ps1` or `repl.ps1`. These are for manual use by the user only. The AI should never run these scripts.

### Temporary working files
Put any temporary files needed during work (screenshots, logs, test outputs, etc.) into the `copilot_working` directory, which is excluded from the repo.

### Shared lib directory
The `lib/` directory is a shared submodule used across multiple projects. Do not remove modules from it just because they are unused in this project.

### Type hints
All function and method parameters should be typed. Return types should also be specified for anything that returns a non-None value.

### Running Python locally
The project code is MicroPython-only and cannot be run on the Windows machine — not even for syntax checking, because the code imports MicroPython-specific modules (e.g. `machine`, `network`) that do not exist in standard CPython. Do not attempt to execute or syntax-check project files locally.

### ESP32 Board Pinout
The file `/docs/esp32_pinout.png` shows the pinout for the ESP32 dev board in use (ESP32-WROOM-32).

### MAX7219 4-module LED matrix display wiring
Uses **SPI** (not I2C). Pin mapping (set in `settings.BILLBOARD`):

| MAX7219 pin | ESP32 GPIO | Wire function |
|---|---|---|
| DIN  | GPIO 23 | SPI MOSI (data in) |
| CLK  | GPIO 18 | SPI SCK (clock) |
| CS   | GPIO 5  | Chip select |
| VCC  | 5 V     | Power (must be 5 V, not 3.3 V) |
| GND  | GND     | Common ground |

### Docstrings
All classes, functions, and methods should include a descriptive docstring. Docstrings MUST be followed by a single blank line.

### Variable names
Prefer long, descriptive variable names over short or abbreviated ones (e.g. `route_handler` not `rh`, `templates_dir` not `tdir`, `client_socket` not `cl`).

### Blank lines after indented blocks
Indented blocks (if/for/while/try/with/class/def bodies) should generally be followed by a blank line for readability.

### Custom templating system
The web server uses a custom templating system (defined in `lib/webserver.py`), not Jinja2. If a template needs a feature the engine doesn't support, update `lib/webserver.py` to add it rather than working around it in the view or template. It supports:
- `{{ variable }}` and `{{ variable.key }}` or `{{ variable.0 }}` for dot-notation access (list indices can also be context variables, e.g. `{{ mylist.i }}` where `i` is a loop variable)
- `{% for var in list %}...{% endfor %}` loops
- `{% for i in range(n) %}` where `n` is an integer literal or a context variable
- `{% for key, value in dict.items() %}`, `{% for key in dict.keys() %}`, `{% for val in dict.values() %}`
- `{% if expression %}...{% endif %}` conditionals (including `==` comparisons, `in`/`not in` membership tests, `and`/`or` connectives, and `not` prefix). Simple variable truthiness uses Python semantics: empty lists/dicts are falsy.
- `{% include 'filename' %}` for template inclusion

When working with templates, remember this is a minimal custom implementation, not a full templating engine.

### Lazy-loaded persistent storage
The `PersistentDict` class (`lib/storage.py`) uses lazy loading to conserve memory. Data is only loaded from disk on first access, not during initialization. This is transparent to the user - all dict operations work normally. The `_ensure_loaded()` method is called automatically by all dict operations.

### Web server performance priority
Web server speed is not very important. Optimizations should prioritize memory efficiency and code simplicity over response latency. Small delays (10-50ms+) for UI operations are acceptable.

### Hardware upgrade to ESP32-S3
Upgrading from ESP32-WROOM-32 to ESP32-S3-WROOM-1 (N16R8 variant with 8MB octal PSRAM). This eliminates all heap constraints (~8MB free heap vs ~28KB on WROOM-32). No code changes needed—PSRAM is transparent to MicroPython. However, GPIO physical pin positions differ between boards; MAX7219 wiring (GPIO 18, 23, 5) numbers stay the same but physical header positions change—confirm pinout before wiring.

### CSS in separate files
Do not use inline `<style>` blocks in templates. All CSS must go in files under `www/styles/` and be linked via `base/imports.html`.

### CSS nesting
All CSS files should use native CSS nesting. Group related selectors under their parent using `&` for modifiers, pseudo-classes, pseudo-elements, and child/descendant rules. Example:
```css
.card {
    background: #111;

    & .card-header { color: orange; }
    & .card-body { padding: 1rem; }
    &:hover { opacity: 0.9; }
    &::before { content: ''; }
}
```

### Lighting filter design
Filters always receive both the **target color** (from the pattern) and the **current color** (from `logical_colors`). Differences are calculated against the target color but applied to the current color. This makes filters order-independent — the result is the same regardless of which order filters are applied in.

### Audio hardware
The hardware includes:
- Up to 3× **WWZMDiB YX5200 MP3 player modules** (serial UART, DFPlayer-compatible protocol)
- 1× **PAM8403 amplifier module** (analog volume via onboard pot; software volume controlled via YX5200 serial commands)
- 1–3× **3W 8Ω mini speakers**

Mixing multiple YX5200 DAC_R outputs into the PAM8403 is done with a passive resistor network (one 1kΩ resistor per player). Each YX5200 UART RX line also requires a 1kΩ resistor from the ESP32-S3 TX pin. The ESP32-S3 has 3 hardware UARTs, sufficient for 3 simultaneous players.




