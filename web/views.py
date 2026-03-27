from webserver import View, render_template
from billboard import Billboard
from storage import PersistentDict
from lighting import Lighting
import gc
import sys
import machine
import settings
import json

try:
    from network import WLAN, STA_IF

    _wlan = WLAN(STA_IF)
except Exception:
    _wlan = None

billboard = Billboard.from_settings(debug=True)

storage = PersistentDict()
lights = Lighting()


def _animation_context():
    """Return a context dict with the current animation running state."""

    running = lights.animation.running
    return {"animation_running": running, "animation_stopped": not running}


def _scenes_context():
    """Return a context dict with the scenes list and current scene."""

    return {
        "scenes": list(lights.settings["scenes"].keys()),
        "current_scene": lights.scene_name,
    }


class HomeView(View):

    def get(self):
        """Handle GET requests for the home route."""

        context = {"message": "Lighting"}
        context.update(_scenes_context())
        context.update(_animation_context())

        return render_template("home.html", context)


class SetSceneView(View):
    """Handle POST requests to set the current lighting scene."""

    def post(self):
        """Set the current lighting scene based on the POST data."""
        scene_name = self.request.form_data.get("scene")
        if scene_name in lights.settings["scenes"]:
            lights.set_scene(scene_name)
            return render_template("scenes/buttons.html", _scenes_context())
        else:
            return "Invalid scene", 400


class AnimationView(View):
    """Handle POST requests to start or stop the lighting animation."""

    def post(self):
        """Start or stop the animation based on the POST data."""
        action = self.request.form_data.get("action")
        if action == "start":
            lights.animation.start()
        elif action == "stop":
            lights.animation.stop()
        else:
            return "Invalid action", 400

        return render_template("animation/buttons.html", _animation_context())


def _indent_json(json_str):
    """Pretty-print a JSON string with proper indentation."""
    result = ""
    indent_level = 0
    in_string = False
    escape_next = False

    for char in json_str:
        if escape_next:
            result += char
            escape_next = False
            continue

        if char == "\\":
            result += char
            escape_next = True
            continue

        if char == '"':
            in_string = not in_string
            result += char
            continue

        if in_string:
            result += char
            continue

        if char in ("{", "["):
            result += char
            indent_level += 1
            result += "\n" + ("  " * indent_level)
        elif char in ("}", "]"):
            indent_level -= 1
            result += "\n" + ("  " * indent_level) + char
        elif char == ",":
            result += char
            result += "\n" + ("  " * indent_level)
        elif char == ":":
            result += char + " "
        elif char not in (" ", "\n"):
            result += char

    return result


class StorageView(View):
    """Display the persistent storage dictionary as pretty-printed JSON."""

    def get(self):
        """Return the storage contents."""
        try:
            storage = PersistentDict()
            # Convert to plain dict and dump as JSON
            storage_dict = {}
            for key, value in storage.items():
                storage_dict[key] = value
            storage_json = json.dumps(storage_dict)
            # Pretty-print with proper indentation
            storage_json = _indent_json(storage_json)
            return render_template("storage.html", {"storage_json": storage_json})
        except Exception as e:
            return "Error: {}".format(str(e)), 500


class SetupView(View):
    """Display the setup page."""

    def get(self) -> str:
        """Return the setup page."""

        named_ranges = lights.settings.get("named_ranges", {})
        return render_template(
            "setup.html",
            {
                "led_count": lights.leds.count,
                "named_range_names": list(named_ranges.keys()),
            },
        )


class NamedRangeView(View):
    """Handle LED identification and saving of named ranges."""

    def _parse_selected(self, query_string: str) -> list:
        """Parse led=N&led=M query string into a list of integers."""

        selected = []
        for part in query_string.split("&"):
            if part.startswith("led="):
                try:
                    selected.append(int(part[4:]))
                except ValueError:
                    pass

        return selected

    def _build_context(self, selected: list) -> dict:
        """Build template context for the LED picker."""

        selected_set = set(selected)
        led_list = [{"index": i, "selected": i in selected_set} for i in range(lights.leds.count)]
        named_range_names = list(lights.settings.get("named_ranges", {}).keys())

        return {
            "led_list": led_list,
            "selected_str": ",".join(str(i) for i in selected),
            "named_range_names": named_range_names,
        }

    def get(self) -> str:
        """Pause animation, light selected LEDs, return updated picker."""

        selected = self._parse_selected(self.request.query)

        if lights.animation.running:
            lights.animation.pause()

        if selected:
            lights.leds.identify(selected)
        else:
            lights.leds.clear()
            lights.leds.show()

        return render_template("setup/led_picker.html", self._build_context(selected))

    def post(self) -> str:
        """Save the named range and resume animation."""

        range_name = self.request.form_data.get("range_name", "").strip()
        selected_raw = self.request.form_data.get("selected", "")

        selected = []
        for part in selected_raw.split(","):
            part = part.strip()
            if part:
                try:
                    selected.append(int(part))
                except ValueError:
                    pass

        if range_name and selected:
            lights.settings["named_ranges"][range_name] = selected
            lights.settings_object.store()

        lights.animation.resume()
        lights.leds.clear()
        lights.leds.show()

        return render_template("setup/led_picker.html", self._build_context([]))


class StatusView(View):
    """Display ESP32 system status."""

    def get(self) -> str:
        """Return the status page with memory, system, WiFi, and animation info."""

        gc.collect()
        mem_free = gc.mem_free()
        mem_alloc = gc.mem_alloc()
        mem_total = mem_free + mem_alloc
        mem_pct = int(mem_alloc * 100 // mem_total) if mem_total else 0

        try:
            cpu_freq_mhz = machine.freq() // 1000000
        except Exception:
            cpu_freq_mhz = "N/A"

        if _wlan is not None and _wlan.isconnected():
            wifi_connected = "Yes"
            ip_address = _wlan.ifconfig()[0]
        else:
            wifi_connected = "No"
            ip_address = "N/A"

        return render_template(
            "status.html",
            {
                "mem_free": mem_free,
                "mem_alloc": mem_alloc,
                "mem_total": mem_total,
                "mem_pct": mem_pct,
                "cpu_freq_mhz": cpu_freq_mhz,
                "upy_version": sys.version,
                "platform": sys.platform,
                "wifi_connected": wifi_connected,
                "ip_address": ip_address,
                "wifi_ssid": settings.WIFI["SSID"],
                "animation_running": str(lights.animation.running),
                "current_scene": lights.scene_name,
                "tick_number": lights.animation.tick_number,
            },
        )
