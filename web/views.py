from webserver import View, render_template, Response
from storage import PersistentDict
from lighting import Lighting
import gc
import io
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

        context = {"message": "Lighting", "page_title": "Home"}
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

        try:
            storage = PersistentDict()
            storage_dict = {k: v for k, v in storage.items()}
            storage_json = _pretty_json(storage_dict)

            return render_template("storage.html", {"storage_json": storage_json, "page_title": "Storage"})

        except Exception as e:
            buf = io.StringIO()
            sys.print_exception(e, buf)
            traceback_text = buf.getvalue()
            return Response(status=500, reason="Internal Server Error", body="<pre>{}</pre>".format(traceback_text))


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
                "page_title": "Setup",
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
        "range_name": _editing_range_name,
    }


class NamedRangeView(View):
    """Manage LED naming - create new or edit existing ranges."""

    def get(self) -> str:
        """Show LED picker for creating or editing a range."""

        global _selected_leds, _editing_range_name
        range_name = self.request.query_params.get("name")

        # Load existing range if editing, otherwise start fresh
        if range_name and range_name in lights.settings.get("named_ranges", {}):
            _editing_range_name = range_name
            _selected_leds = list(lights.settings["named_ranges"][range_name])
        else:
            _selected_leds = []
            _editing_range_name = None

        if lights.animation.running:
            lights.animation.pause()

        lights.leds.clear()
        if _selected_leds:
            lights.leds.identify(_selected_leds)
        lights.leds.show()

        context = _named_range_context()
        print(context["range_name"])
        print(context["named_range_names"])
        context["page_title"] = "Named Ranges"
        return render_template("setup/led_picker.html", context)

    def post(self) -> str:
        """Save or delete a named range."""

        global _selected_leds, _editing_range_name
        action = self.request.form_data.get("action", "save").strip()
        old_name = self.request.form_data.get("old_name", "").strip()
        range_name = self.request.form_data.get("range_name", "").strip()
        selected_leds_str = self.request.form_data.get("selected_leds", "").strip()

        selected_leds = []
        if selected_leds_str:
            try:
                selected_leds = [int(x) for x in selected_leds_str.split(",") if x]
            except ValueError:
                pass

        # Determine if saving new or editing existing
        if action == "delete" and old_name and old_name in lights.settings.get("named_ranges", {}):
            del lights.settings["named_ranges"][old_name]
        elif range_name and selected_leds:
            lights.settings["named_ranges"][range_name] = selected_leds
            if old_name != range_name and old_name in lights.settings.get("named_ranges", {}):
                del lights.settings["named_ranges"][old_name]

        lights.settings_object.store()

        _selected_leds = []
        _editing_range_name = None
        lights.animation.resume()
        lights.leds.clear()
        lights.leds.show()

        context = _named_range_context()
        context["page_title"] = "Named Ranges"
        return render_template("setup/led_picker.html", context)


class NamedRangeSetView(View):
    """Set the current LED selection without returning HTML."""

    def post(self) -> None:
        """Update _selected_leds from form data and light up hardware. Returns 204 (no content)."""

        global _selected_leds
        selected_leds_str = self.request.form_data.get("selected_leds", "").strip()

        selected_leds = []
        if selected_leds_str:
            try:
                selected_leds = [int(x) for x in selected_leds_str.split(",") if x]
            except ValueError:
                pass

        _selected_leds = selected_leds

        if _selected_leds:
            lights.leds.identify(_selected_leds)
        else:
            lights.leds.clear()
            lights.leds.show()

        return None


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
                "page_title": "Status",
            },
        )


def _custom_colors_response(edit_name: str = "", edit_hex: str = "") -> str:
    """Build and render the custom colors template with current settings.

    Pass edit_name and edit_hex to pre-fill the form for editing an existing color.
    """

    custom_colors_list = [
        (name, tuple(value) if isinstance(value, (list, tuple)) else value)
        for name, value in lights.settings.get("custom_colors", {}).items()
    ]
    return render_template(
        "setup/custom_colors.html",
        {
            "custom_colors": custom_colors_list,
            "page_title": "Custom Colors",
            "edit_name": edit_name,
            "edit_hex": edit_hex,
        },
    )


class CustomColorsView(View):
    """Manage custom colors - create new or delete existing colors."""

    def get(self) -> str:
        """Show custom colors form and list of current colors."""

        return _custom_colors_response()

    def post(self) -> str:
        """Add or delete a custom color."""

        action = self.request.form_data.get("action", "add").strip()
        color_name = self.request.form_data.get("color_name", "").strip()
        color_value = self.request.form_data.get("color_value", "").strip()

        if "custom_colors" not in lights.settings:
            lights.settings["custom_colors"] = {}

        if action == "delete" and color_name and color_name in lights.settings["custom_colors"]:
            del lights.settings["custom_colors"][color_name]
        elif action in ("add", "update") and color_name and color_value:
            try:
                hex_color = color_value.lstrip("#")
                r = int(hex_color[0:2], 16)
                g = int(hex_color[2:4], 16)
                b = int(hex_color[4:6], 16)
                lights.settings["custom_colors"][color_name] = (r, g, b)
            except (ValueError, IndexError):
                pass
        elif action == "edit_form" and color_name and color_name in lights.settings["custom_colors"]:
            rgb = lights.settings["custom_colors"][color_name]
            edit_hex = "#{:02X}{:02X}{:02X}".format(int(rgb[0]), int(rgb[1]), int(rgb[2]))
            return _custom_colors_response(edit_name=color_name, edit_hex=edit_hex)
        elif action == "cancel":
            return _custom_colors_response()

        lights.settings_object.store()
        return _custom_colors_response()


def _scene_name_id(scene_name: str) -> str:
    """Convert a scene name to a safe string suitable for use in DOM element IDs."""

    result: str = ""
    for char in scene_name:
        if char.isalpha() or char.isdigit() or char in ("-", "_"):
            result += char.lower()
        else:
            result += "-"

    return result


def _scenes_list(scenes_dict: dict) -> list:
    """Build a list of scene summary dicts for template rendering."""

    result: list = []
    for scene_name, scene_effects in scenes_dict.items():
        result.append(
            {
                "name": scene_name,
                "name_id": _scene_name_id(scene_name),
                "effect_count": len(scene_effects),
            }
        )

    return result


_STANDARD_COLORS: dict = {
    "white": (255, 255, 255),
    "black": (0, 0, 0),
    "red": (255, 0, 0),
    "green": (0, 255, 0),
    "blue": (0, 0, 255),
}


def _color_name_to_hex(color_name: str, custom_colors: dict) -> str:
    """Return the hex string for a named color (standard or custom).

    Falls back to '#FF0000' if the name is not found.
    """

    if color_name in _STANDARD_COLORS:
        r, g, b = _STANDARD_COLORS[color_name]
        return "#{:02X}{:02X}{:02X}".format(r, g, b)

    if color_name in custom_colors:
        rgb = custom_colors[color_name]
        return "#{:02X}{:02X}{:02X}".format(int(rgb[0]), int(rgb[1]), int(rgb[2]))

    return "#FF0000"


def _pattern_params_context(pattern: str, existing_effect: dict = None) -> dict:
    """Build the template context needed to render the pattern params fragment.

    If existing_effect is provided, pre-fills all fields with its current values.
    """

    pattern_metadata: dict = lights.get_pattern_metadata()
    pattern_info: dict = pattern_metadata[pattern]
    color_count: int = pattern_info["color_count"]
    existing_colors: list = existing_effect.get("colors") if existing_effect else None
    custom_colors: dict = lights.settings.get("custom_colors", {})

    color_hex: list = []
    color_names: list = []
    color_selected: list = []
    color_is_named: list = []
    color_is_picker: list = []

    for i in range(color_count):
        existing = existing_colors[i] if existing_colors and i < len(existing_colors) else None

        if isinstance(existing, str):
            # Resolve display and selection values
            if existing.startswith("custom:"):
                raw_name: str = existing[7:]
                hex_val: str = _color_name_to_hex(raw_name, custom_colors)
                # Dropdown option value includes the prefix
                dropdown_val: str = existing
            else:
                hex_val = _color_name_to_hex(existing, custom_colors)
                dropdown_val = existing
            color_hex.append(hex_val)
            color_names.append(existing)
            color_selected.append(dropdown_val)
            color_is_named.append(True)
            color_is_picker.append(False)
        elif isinstance(existing, (list, tuple)) and len(existing) == 3:
            color_hex.append("#{:02X}{:02X}{:02X}".format(int(existing[0]), int(existing[1]), int(existing[2])))
            color_names.append("")
            color_selected.append("__picker__")
            color_is_named.append(False)
            color_is_picker.append(True)
        else:
            color_hex.append("#FF0000")
            color_names.append("")
            color_selected.append("")
            color_is_named.append(False)
            color_is_picker.append(False)

    context: dict = {
        "pattern": pattern,
        "pattern_info": pattern_info,
        "color_count": color_count,
        "color_hex": color_hex,
        "color_names": color_names,
        "color_display_names": [n[7:] if n.startswith("custom:") else n for n in color_names],
        "color_selected": color_selected,
        "color_is_named": color_is_named,
        "color_is_picker": color_is_picker,
        "named_ranges": lights.settings.get("named_ranges", {}),
        "custom_colors": custom_colors,
    }

    # Add pre-fill values for optional numeric/boolean params as param_val_<name>
    if existing_effect:
        for param_name in pattern_info["optional"]:
            if param_name in existing_effect:
                val = existing_effect[param_name]
                context["param_val_" + param_name] = "true" if val is True else str(val)

        # Pre-fill target
        if "target" in existing_effect:
            context["param_val_target"] = str(existing_effect["target"])

    return context


def _scene_edit_context(scene_name: str, edit_effect_name: str = None) -> dict:
    """Build template context for the scene effect editor."""

    context: dict = {
        "scene_name": scene_name,
        "scene_name_id": _scene_name_id(scene_name),
        "scene_effects": lights.settings["scenes"].get(scene_name, {}),
        "pattern_metadata": lights.get_pattern_metadata(),
        "named_ranges": lights.settings.get("named_ranges", {}),
        "custom_colors": lights.settings.get("custom_colors", {}),
        "page_title": "Edit Scene",
    }

    if edit_effect_name:
        effect_dict: dict = lights.settings["scenes"].get(scene_name, {}).get(edit_effect_name, {})
        pattern: str = effect_dict.get("pattern", "")
        context["edit_effect_name"] = edit_effect_name
        context["edit_effect_pattern"] = pattern
        context["old_effect_name"] = edit_effect_name
        if pattern and pattern in lights.get_pattern_metadata():
            context.update(_pattern_params_context(pattern, effect_dict))

    return context


def _parse_effect_from_form(form_data: dict, pattern: str) -> dict:
    """Build an effect dict from form data, handling colors and typed parameters."""

    job_dict: dict = {"pattern": pattern}

    # Collect indexed color fields
    colors_list: list = []
    color_index = 0
    while True:
        color_key = f"param_color_{color_index}"

        if color_key not in form_data:
            break

        color_value: str = form_data.get(color_key, "").strip()

        if not color_value:
            pass
        elif color_value.startswith("#") or (
            len(color_value) == 6 and all(c in "0123456789ABCDEFabcdef" for c in color_value)
        ):
            # Hex color value - convert to tuple
            try:
                hex_color: str = color_value.lstrip("#")
                r: int = int(hex_color[0:2], 16)
                g: int = int(hex_color[2:4], 16)
                b: int = int(hex_color[4:6], 16)
                colors_list.append((r, g, b))
            except (ValueError, IndexError):
                colors_list.append(color_value)
        else:
            # Color name (possibly with "custom:" prefix)
            colors_list.append(color_value)

        color_index += 1

    if colors_list:
        job_dict["colors"] = colors_list

    # Collect other param_ fields
    for key in form_data.keys():
        if key.startswith("param_") and not key.startswith("param_color"):
            param_name: str = key[6:]
            param_value: str = form_data.get(key, "").strip()
            if param_value:
                if param_value.lower() == "true":
                    job_dict[param_name] = True
                else:
                    try:
                        job_dict[param_name] = float(param_value) if "." in param_value else int(param_value)
                    except ValueError:
                        job_dict[param_name] = param_value

    return job_dict


class ScenesView(View):
    """List scenes and create or delete scenes."""

    def get(self) -> str:
        """Show scene list and create-scene form."""

        context: dict = {
            "scenes": _scenes_list(lights.settings.get("scenes", {})),
            "page_title": "Scenes",
        }
        return render_template("setup/scenes.html", context)

    def post(self) -> str:
        """Create or delete a scene."""

        action: str = self.request.form_data.get("action", "").strip()
        scene_name: str = self.request.form_data.get("scene_name", "").strip()

        if "scenes" not in lights.settings:
            lights.settings["scenes"] = {}

        if action == "create_scene" and scene_name:
            if scene_name not in lights.settings["scenes"]:
                lights.settings["scenes"][scene_name] = {}
            lights.settings_object.store()

        elif action == "delete_scene" and scene_name and scene_name in lights.settings["scenes"]:
            del lights.settings["scenes"][scene_name]
            lights.settings_object.store()

        context: dict = {
            "scenes": _scenes_list(lights.settings.get("scenes", {})),
            "page_title": "Scenes",
        }
        return render_template("setup/scenes.html", context)


class SceneEditView(View):
    """Manage effects within a specific scene."""

    def get(self) -> str:
        """Show effect body for the given scene, optionally pre-loaded to edit an effect."""

        scene_name: str = self.request.query_params.get("scene", "").strip()

        if not scene_name or scene_name not in lights.settings.get("scenes", {}):
            return '<p class="text-danger small">Scene not found.</p>'

        edit_effect_name: str = self.request.query_params.get("edit_effect", "").strip()
        params_only: str = self.request.query_params.get("params_only", "").strip()

        # If params_only flag is set, return just the pattern parameters fragment
        if params_only and edit_effect_name:
            return render_template(
                "setup/pattern_params.html",
                _pattern_params_context(
                    lights.settings["scenes"][scene_name][edit_effect_name]["pattern"],
                    lights.settings["scenes"][scene_name].get(edit_effect_name, {}),
                ),
            )

        return render_template("setup/scene_edit.html", _scene_edit_context(scene_name, edit_effect_name or None))

    def post(self) -> str:
        """Add/update or delete an effect, or return pattern parameter fields fragment."""

        action: str = self.request.form_data.get("action", "").strip()
        scene_name: str = self.request.form_data.get("scene_name", "").strip()
        effect_name: str = self.request.form_data.get("effect_name", "").strip()
        pattern: str = self.request.form_data.get("pattern", "").strip()

        # Pattern selected — return parameters fragment only
        if not action and pattern:
            if pattern in lights.get_pattern_metadata():
                return render_template("setup/pattern_params.html", _pattern_params_context(pattern))
            return '<p class="text-danger">Invalid pattern.</p>'

        if scene_name and scene_name in lights.settings.get("scenes", {}):
            if action in ("add_effect", "update_effect") and effect_name and pattern:
                old_effect_name: str = self.request.form_data.get("old_effect_name", "").strip()

                # If renaming, delete the old entry first
                if (
                    old_effect_name
                    and old_effect_name != effect_name
                    and old_effect_name in lights.settings["scenes"][scene_name]
                ):
                    del lights.settings["scenes"][scene_name][old_effect_name]

                lights.settings["scenes"][scene_name][effect_name] = _parse_effect_from_form(
                    self.request.form_data, pattern
                )
                lights.settings_object.store()

            elif action == "delete_effect" and effect_name:
                if effect_name in lights.settings["scenes"][scene_name]:
                    del lights.settings["scenes"][scene_name][effect_name]
                    lights.settings_object.store()

        return render_template("setup/scene_edit.html", _scene_edit_context(scene_name))


class ColorSelectView(View):
    """Return an HTML fragment for the color display area when the color dropdown changes."""

    def post(self) -> str:
        """Return a pill, color picker, or empty based on the selected color."""

        color_index: str = self.request.form_data.get("color_index", "0").strip()
        color_value: str = self.request.form_data.get(f"param_color_{color_index}", "").strip()
        custom_colors: dict = lights.settings.get("custom_colors", {})

        is_named: bool = bool(color_value and color_value != "__picker__")
        is_picker: bool = color_value == "__picker__"

        # Determine stored color name and hex for display
        if is_named:
            is_custom: bool = color_value.startswith("custom:")
            if is_custom:
                raw_name: str = color_value[7:]
                stored_name: str = color_value
                display_name: str = raw_name
            else:
                raw_name = color_value
                stored_name = color_value
                display_name = color_value
            hex_val: str = _color_name_to_hex(raw_name, custom_colors)
        else:
            stored_name = ""
            display_name = ""
            hex_val = "#FF0000"

        context: dict = {
            "color_index": color_index,
            "color_name": stored_name,
            "color_display_name": display_name,
            "color_hex_val": hex_val,
            "is_named": is_named,
            "is_picker": is_picker,
        }

        return render_template("setup/color_select.html", context)
