from webserver import View, render_template, Response
from storage import PersistentDict
from lighting import Lighting
import gc
import json
import os
import sys
import machine
import settings


def _fmt_bytes(num_bytes: int) -> str:
    """Format a byte count as a human-readable string (KB or MB)."""

    if num_bytes >= 1024 * 1024:
        return "{:.1f} MB".format(num_bytes / (1024 * 1024))
    elif num_bytes >= 1024:
        return "{:.1f} KB".format(num_bytes / 1024)
    else:
        return "{} B".format(num_bytes)


def _pretty_json(obj: dict, indent: int = 0) -> str:
    """Pretty-print a JSON object with indentation."""

    if isinstance(obj, dict):
        if not obj:
            return "{}"
        items = []
        for k, v in obj.items():
            value_str = _pretty_json(v, indent + 2)
            items.append("  " * (indent // 2 + 1) + json.dumps(k) + ": " + value_str)
        return "{\n" + ",\n".join(items) + "\n" + "  " * (indent // 2) + "}"
    elif isinstance(obj, list):
        if not obj:
            return "[]"
        items = []
        for item in obj:
            item_str = _pretty_json(item, indent + 2)
            items.append("  " * (indent // 2 + 1) + item_str)
        return "[\n" + ",\n".join(items) + "\n" + "  " * (indent // 2) + "]"
    else:
        return json.dumps(obj)


try:
    from network import WLAN, STA_IF

    _wlan = WLAN(STA_IF)
except Exception:
    _wlan = None

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


class StorageView(View):
    """Display the persistent storage dictionary as pretty-printed JSON."""

    def get(self):
        """Return the storage contents as JSON."""

        import io

        try:
            storage = PersistentDict()
            storage_dict = {k: v for k, v in storage.items()}
            storage_json = _pretty_json(storage_dict)

            return render_template("storage.html", {"storage_json": storage_json})

        except Exception as e:
            buf = io.StringIO()
            sys.print_exception(e, buf)
            traceback_text = buf.getvalue()
            return "<pre>{}</pre>".format(traceback_text), 500


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


# Server-side selection state for the LED naming tool
_selected_leds = []
_editing_range_name = None


def _named_range_context() -> dict:
    """Build template context for the LED picker from current server-side selection state."""

    selected_set = set(_selected_leds)
    led_list = [
        {
            "index": i,
            "css_class": "btn-warning" if i in selected_set else "btn-outline-secondary",
        }
        for i in range(lights.leds.count)
    ]
    named_range_names = list(lights.settings.get("named_ranges", {}).keys())

    return {
        "led_list": led_list,
        "named_range_names": named_range_names,
    }


class NamedRangeView(View):
    """Enter LED naming mode and save named ranges."""

    def get(self) -> str:
        """Pause animation and show the LED picker."""

        global _selected_leds, _editing_range_name
        _selected_leds = []
        _editing_range_name = None
        _editing_range_name = None

        if lights.animation.running:
            lights.animation.pause()

        lights.leds.clear()
        lights.leds.show()

        return render_template("setup/led_picker.html", _named_range_context())

    def post(self) -> str:
        """Save the current selection as a named range and resume animation."""

        global _selected_leds, _editing_range_name
        range_name = self.request.form_data.get("range_name", "").strip()
        selected_leds_str = self.request.form_data.get("selected_leds", "").strip()

        selected_leds = []
        if selected_leds_str:
            try:
                selected_leds = [int(x) for x in selected_leds_str.split(",") if x]
            except ValueError:
                pass

        if range_name and selected_leds:
            lights.settings["named_ranges"][range_name] = selected_leds
            lights.settings_object.store()

        _selected_leds = []
        _editing_range_name = None
        lights.animation.resume()
        lights.leds.clear()
        lights.leds.show()

        return render_template("setup/led_picker.html", _named_range_context())


class NamedRangeToggleView(View):
    """Toggle a single LED in/out of the current selection."""

    def get(self) -> str:
        """Toggle the LED index given as ?led=N and return updated picker."""

        global _selected_leds, _editing_range_name
        led_index = None

        for part in self.request.query.split("&"):
            if part.startswith("led="):
                try:
                    led_index = int(part[4:])
                except ValueError:
                    pass

        print(f"[TOGGLE] led_index={led_index}, before={_selected_leds}")

        if led_index is not None:
            if led_index in _selected_leds:
                _selected_leds.remove(led_index)
            else:
                _selected_leds.append(led_index)

        print(f"[TOGGLE] after={_selected_leds}")

        if _selected_leds:
            print(f"[TOGGLE] identify({_selected_leds})")
            lights.leds.identify(_selected_leds)
        else:
            print(f"[TOGGLE] clear & show")
            lights.leds.clear()
            lights.leds.show()

        if _editing_range_name:
            selected_set = set(_selected_leds)
            led_list = [
                {
                    "index": i,
                    "css_class": "btn-warning" if i in selected_set else "btn-outline-secondary",
                }
                for i in range(lights.leds.count)
            ]
            named_range_names = list(lights.settings.get("named_ranges", {}).keys())
            return render_template(
                "setup/led_picker_edit.html",
                {
                    "range_name": _editing_range_name,
                    "led_list": led_list,
                    "named_range_names": named_range_names,
                },
            )
        else:
            return render_template("setup/led_picker.html", _named_range_context())


class NamedRangeSetView(View):
    """Set the current LED selection without returning HTML."""

    def post(self) -> None:
        """Update _selected_leds from form data and light up hardware. Returns 204 (no content)."""

        global _selected_leds, _editing_range_name
        selected_leds_str = self.request.form_data.get("selected_leds", "").strip()

        selected_leds = []
        if selected_leds_str:
            try:
                selected_leds = [int(x) for x in selected_leds_str.split(",") if x]
            except ValueError:
                pass

        _selected_leds = selected_leds
        print(f"[SET] selected_leds updated to {_selected_leds}")

        if _selected_leds:
            lights.leds.identify(_selected_leds)
        else:
            lights.leds.clear()
            lights.leds.show()

        return None


class NamedRangeClearView(View):
    """Clear the current LED selection."""

    def get(self) -> str:
        """Clear all selected LEDs and return updated picker."""

        global _selected_leds, _editing_range_name
        _selected_leds = []
        _editing_range_name = None
        lights.leds.clear()
        lights.leds.show()

        return render_template("setup/led_picker.html", _named_range_context())


class NamedRangeEditView(View):
    """Edit an existing named range."""

    def get(self) -> str:
        """Load a named range for editing."""

        global _selected_leds, _editing_range_name
        range_name = self.request.query_params.get("name")

        if not range_name or range_name not in lights.settings.get("named_ranges", {}):
            return "", 404

        _editing_range_name = range_name
        _selected_leds = list(lights.settings["named_ranges"][range_name])

        if lights.animation.running:
            lights.animation.pause()

        lights.leds.clear()
        if _selected_leds:
            lights.leds.identify(_selected_leds)
        lights.leds.show()

        selected_set = set(_selected_leds)
        led_list = [
            {
                "index": i,
                "css_class": "btn-warning" if i in selected_set else "btn-outline-secondary",
            }
            for i in range(lights.leds.count)
        ]

        return render_template(
            "setup/led_picker_edit.html",
            {
                "range_name": range_name,
                "led_list": led_list,
                "named_range_names": list(lights.settings.get("named_ranges", {}).keys()),
            },
        )

    def post(self) -> str:
        """Update or delete a named range."""

        global _selected_leds, _editing_range_name
        old_name = self.request.form_data.get("old_name", "").strip()
        new_name = self.request.form_data.get("new_name", "").strip()
        action = self.request.form_data.get("action", "").strip()
        selected_leds_str = self.request.form_data.get("selected_leds", "").strip()

        if not old_name or old_name not in lights.settings.get("named_ranges", {}):
            _selected_leds = []
            _editing_range_name = None
            lights.animation.resume()
            return render_template("setup/led_picker.html", _named_range_context())

        selected_leds = []
        if selected_leds_str:
            try:
                selected_leds = [int(x) for x in selected_leds_str.split(",") if x]
            except ValueError:
                pass

        if action == "delete":
            del lights.settings["named_ranges"][old_name]
        elif action == "save" and new_name:
            lights.settings["named_ranges"][new_name] = selected_leds
            if new_name != old_name:
                del lights.settings["named_ranges"][old_name]

        lights.settings_object.store()

        _selected_leds = []
        _editing_range_name = None
        lights.animation.resume()
        lights.leds.clear()
        lights.leds.show()

        return render_template("setup/led_picker.html", _named_range_context())


class StatusView(View):
    """Display ESP32 system status."""

    def get(self) -> str:
        """Return the status page with memory, system, WiFi, and animation info."""

        gc.collect()
        mem_free_bytes = gc.mem_free()
        mem_alloc_bytes = gc.mem_alloc()
        mem_total_bytes = mem_free_bytes + mem_alloc_bytes
        mem_pct = int(mem_alloc_bytes * 100 // mem_total_bytes) if mem_total_bytes else 0

        try:
            vfs = os.statvfs("/")
            storage_block_size = vfs[0]
            storage_total_bytes = vfs[2] * storage_block_size
            storage_free_bytes = vfs[3] * storage_block_size
            storage_used_bytes = storage_total_bytes - storage_free_bytes
            storage_pct = int(storage_used_bytes * 100 // storage_total_bytes) if storage_total_bytes else 0
            storage_total = _fmt_bytes(storage_total_bytes)
            storage_free = _fmt_bytes(storage_free_bytes)
            storage_used = _fmt_bytes(storage_used_bytes)
        except Exception:
            storage_total = storage_free = storage_used = "N/A"
            storage_pct = 0

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
                "mem_free": _fmt_bytes(mem_free_bytes),
                "mem_alloc": _fmt_bytes(mem_alloc_bytes),
                "mem_total": _fmt_bytes(mem_total_bytes),
                "mem_pct": mem_pct,
                "storage_total": storage_total,
                "storage_free": storage_free,
                "storage_used": storage_used,
                "storage_pct": storage_pct,
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
