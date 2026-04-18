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

    scene_names = sorted(lights.settings["scenes"].keys())
    ongoing_scenes = [name for name in scene_names if lights.is_scene_ongoing(name)]
    immediate_scenes = [name for name in scene_names if not lights.is_scene_ongoing(name)]

    return {
        "scenes": scene_names,
        "current_scene": lights.scene_name,
        "ongoing_scenes": ongoing_scenes,
        "immediate_scenes": immediate_scenes,
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
        """Set the current lighting scene based on the POST data.

        Any form fields other than ``scene`` are forwarded as keyword arguments
        to ``set_scene``, allowing scene functions to receive extra parameters.
        """

        scene_name = self.request.form_data.get("scene")
        if scene_name in lights.settings["scenes"]:
            extra_kwargs = {key: value for key, value in self.request.form_data.items() if key != "scene"}
            lights.set_scene(scene_name, **extra_kwargs)
            return None
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

        return render_template("setup.html", {"page_title": "Setup"})


class NamedRangeSummaryView(View):
    """Return a summary snippet of named ranges for the setup card."""

    def get(self) -> str:
        """Return named ranges summary HTML fragment."""

        named_ranges = lights.settings.get("named_ranges", {})
        return render_template(
            "setup/named_ranges_summary.html",
            {"named_range_names": sorted(named_ranges.keys())},
        )


class CustomColorsSummaryView(View):
    """Return a summary snippet of custom colors for the setup card."""

    def get(self) -> str:
        """Return custom colors summary HTML fragment."""

        custom_colors_list = sorted(
            [
                (name, tuple(value) if isinstance(value, (list, tuple)) else value)
                for name, value in lights.settings.get("custom_colors", {}).items()
            ],
            key=lambda item: item[0],
        )
        return render_template(
            "setup/custom_colors_summary.html",
            {"custom_colors": custom_colors_list},
        )


class ScenesSummaryView(View):
    """Return a summary snippet of scenes for the setup card."""

    def get(self) -> str:
        """Return scenes summary HTML fragment."""

        return render_template(
            "setup/scenes_summary.html",
            {"scenes": _scenes_list(lights.settings.get("scenes", {}))},
        )


class EffectsSummaryView(View):
    """Return a summary snippet of effects for the setup card."""

    def get(self) -> str:
        """Return effects summary HTML fragment."""

        effect_names = sorted(lights.settings.get("effects", {}).keys())
        return render_template(
            "setup/effects_summary.html",
            {"effect_names": effect_names},
        )


class FiltersSummaryView(View):
    """Return a summary snippet of filters for the setup card."""

    def get(self) -> str:
        """Return filters summary HTML fragment."""

        filter_names = sorted(lights.settings.get("filters", {}).keys())
        return render_template(
            "setup/filters_summary.html",
            {"filter_names": filter_names},
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
    named_range_names = sorted(lights.settings.get("named_ranges", {}).keys())

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

        # Check for restore result from redirect.
        restore_status: str = self.request.query_params.get("restore", "")
        restore_message: str = ""
        restore_class: str = ""
        if restore_status == "ok":
            restore_message = "Storage restored successfully."
            restore_class = "success"

        elif restore_status == "invalid":
            restore_message = "Restore failed: invalid JSON data."
            restore_class = "danger"

        elif restore_status == "empty":
            restore_message = "Restore failed: no data provided."
            restore_class = "danger"

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
                "restore_message": restore_message,
                "restore_class": restore_class,
                "page_title": "Status",
            },
        )


class BackupView(View):
    """Serve the persistent storage as a downloadable JSON file."""

    def get(self) -> Response:
        """Return the storage dict as a JSON download."""

        storage = PersistentDict()
        storage_dict: dict = {k: v for k, v in storage.items()}
        json_body: str = json.dumps(storage_dict)

        return Response(
            body=json_body,
            content_type="application/json",
            headers={"Content-Disposition": 'attachment; filename="lightmotron_backup.json"'},
        )


class RestoreView(View):
    """Restore persistent storage from uploaded JSON."""

    def post(self) -> Response:
        """Parse JSON from the form textarea and overwrite storage."""

        json_text: str = self.request.form_data.get("backup_json", "").strip()
        if not json_text:
            return Response(
                status=302,
                reason="Found",
                headers={"Location": "/status?restore=empty"},
            )

        try:
            restored_data: dict = json.loads(json_text)
        except (ValueError, TypeError):
            return Response(
                status=302,
                reason="Found",
                headers={"Location": "/status?restore=invalid"},
            )

        if not isinstance(restored_data, dict):
            return Response(
                status=302,
                reason="Found",
                headers={"Location": "/status?restore=invalid"},
            )

        storage = PersistentDict()
        storage.clear()
        storage.update(restored_data)
        storage.store()

        # Reload lighting settings from the restored data.
        lights.settings = storage["lighting_settings"]

        return Response(
            status=302,
            reason="Found",
            headers={"Location": "/status?restore=ok"},
        )


def _custom_colors_response(edit_name: str = "", edit_hex: str = "") -> str:
    """Build and render the custom colors template with current settings.

    Pass edit_name and edit_hex to pre-fill the form for editing an existing color.
    """

    custom_colors_list = sorted(
        [
            (name, tuple(value) if isinstance(value, (list, tuple)) else value)
            for name, value in lights.settings.get("custom_colors", {}).items()
        ],
        key=lambda item: item[0],
    )
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
    """Build a list of scene summary dicts for template rendering, sorted alphabetically."""

    result: list = []
    for scene_name in sorted(scenes_dict.keys()):
        result.append(
            {
                "name": scene_name,
                "name_id": _scene_name_id(scene_name),
                "effect_count": len(scenes_dict[scene_name]),
            }
        )

    return result


def _effects_list(effects_dict: dict) -> list:
    """Build a list of effect summary dicts for template rendering, sorted alphabetically."""

    result: list = []
    for effect_name in sorted(effects_dict.keys()):
        effect = effects_dict[effect_name]
        result.append(
            {
                "name": effect_name,
                "pattern": effect.get("pattern", ""),
            }
        )

    return result


def _filters_list(filters_dict: dict) -> list:
    """Build a list of filter summary dicts for template rendering, sorted alphabetically."""

    result: list = []
    for filter_name in sorted(filters_dict.keys()):
        filter_def = filters_dict[filter_name]
        result.append(
            {
                "name": filter_name,
                "filter_type": filter_def.get("filter", ""),
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


def _pattern_params_context(pattern: str, existing_effect: dict = None, show_target: bool = True) -> dict:
    """Build the template context needed to render the pattern params fragment.

    If existing_effect is provided, pre-fills all fields with its current values.
    Set show_target=False to hide the target input (used for standalone effect editing).
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
                # Dropdown option conditions compare against raw name (custom_colors.keys())
                dropdown_val: str = raw_name
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
        "has_optional": bool(pattern_info["optional"]),
        "show_target": show_target,
        "color_select_url": "/scenes/color_select" if show_target else "/effects/color_select",
    }

    # Build available named filters list with selection state
    stored_filters: dict = lights.settings.get("filters", {})
    selected_filters: list = existing_effect.get("filters", []) if existing_effect else []
    available_filters: list = []
    for filter_name in sorted(stored_filters.keys()):
        filter_data: dict = stored_filters[filter_name]
        available_filters.append(
            {
                "name": filter_name,
                "filter_type": filter_data.get("filter", "?"),
                "selected": filter_name in selected_filters,
            }
        )

    if available_filters:
        context["available_filters"] = available_filters
        context["has_available_filters"] = True

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


def _scene_edit_context(scene_name: str, edit_entry_name: str = None) -> dict:
    """Build template context for the scene entry editor.

    Scene entries map an effect name to a target. Each entry is {effect: "name", target: "spec"}.
    """

    scene_data: dict = lights.settings["scenes"].get(scene_name, {})
    effects_dict: dict = lights.settings.get("effects", {})

    # Build entries list with resolved effect pattern for display
    scene_entries: list = []
    for entry_name in sorted(scene_data.keys()):
        entry = scene_data[entry_name]
        effect_name = entry.get("effect", "")
        effect_pattern = effects_dict.get(effect_name, {}).get("pattern", "?")
        scene_entries.append(
            (
                entry_name,
                {
                    "effect": effect_name,
                    "target": entry.get("target", "all"),
                    "pattern": effect_pattern,
                },
            )
        )

    context: dict = {
        "scene_name": scene_name,
        "scene_name_id": _scene_name_id(scene_name),
        "scene_entries": scene_entries,
        "available_effects": sorted(effects_dict.keys()),
        "named_ranges": lights.settings.get("named_ranges", {}),
        "page_title": "Edit Scene",
    }

    if edit_entry_name:
        entry_dict: dict = scene_data.get(edit_entry_name, {})
        context["edit_entry_name"] = edit_entry_name
        context["edit_entry_effect"] = entry_dict.get("effect", "")
        context["edit_entry_target"] = str(entry_dict.get("target", ""))
        context["edit_entry_cycles"] = str(entry_dict["cycles"]) if "cycles" in entry_dict else ""
        context["old_entry_name"] = edit_entry_name

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

    # Collect selected named filters (individual checkboxes named effect_filter_<name>)
    stored_filters: dict = lights.settings.get("filters", {})
    selected_filters: list = [
        filter_name
        for filter_name in sorted(stored_filters.keys())
        if form_data.get(f"effect_filter_{filter_name}") == "1"
    ]
    if selected_filters:
        job_dict["filters"] = selected_filters

    return job_dict


def _effect_edit_context(effect_name: str = None) -> dict:
    """Build template context for the effect editor.

    Effects are standalone pattern definitions without a target.
    """

    effects_dict: dict = lights.settings.get("effects", {})
    context: dict = {
        "effects": _effects_list(effects_dict),
        "pattern_metadata": lights.get_pattern_metadata(),
        "page_title": "Effects",
    }

    if effect_name and effect_name in effects_dict:
        effect_dict: dict = effects_dict[effect_name]
        pattern: str = effect_dict.get("pattern", "")
        context["edit_effect_name"] = effect_name
        context["edit_effect_pattern"] = pattern
        context["edit_effect_cycles"] = str(effect_dict["cycles"]) if "cycles" in effect_dict else ""
        context["old_effect_name"] = effect_name
        if pattern and pattern in lights.get_pattern_metadata():
            context.update(_pattern_params_context(pattern, effect_dict, show_target=False))

    return context


class EffectsView(View):
    """List effects and create or delete effects."""

    def get(self) -> str:
        """Show effect list and create-effect form."""

        context: dict = {
            "effects": _effects_list(lights.settings.get("effects", {})),
            "page_title": "Effects",
        }
        return render_template("setup/effects.html", context)

    def post(self) -> str:
        """Create or delete an effect."""

        action: str = self.request.form_data.get("action", "").strip()
        effect_name: str = self.request.form_data.get("effect_name", "").strip()

        if "effects" not in lights.settings:
            lights.settings["effects"] = {}

        if action == "create_effect" and effect_name:
            if effect_name not in lights.settings["effects"]:
                lights.settings["effects"][effect_name] = {"pattern": "solid"}
            lights.settings_object.store()
            # Go straight to editing the new effect
            return render_template("setup/effect_edit.html", _effect_edit_context(effect_name))

        elif action == "delete_effect" and effect_name and effect_name in lights.settings["effects"]:
            del lights.settings["effects"][effect_name]
            lights.settings_object.store()

        context: dict = {
            "effects": _effects_list(lights.settings.get("effects", {})),
            "page_title": "Effects",
        }
        return render_template("setup/effects.html", context)


class EffectEditView(View):
    """Edit an individual effect's pattern, colors, and parameters."""

    def get(self) -> str:
        """Show effect editor for the given effect."""

        effect_name: str = self.request.query_params.get("effect", "").strip()

        if not effect_name or effect_name not in lights.settings.get("effects", {}):
            return '<p class="text-danger small">Effect not found.</p>'

        return render_template("setup/effect_edit.html", _effect_edit_context(effect_name))

    def post(self) -> str:
        """Update an effect's pattern and parameters, or return pattern params fragment."""

        action: str = self.request.form_data.get("action", "").strip()
        effect_name: str = self.request.form_data.get("effect_name", "").strip()
        pattern: str = self.request.form_data.get("pattern", "").strip()

        # Pattern selected — return parameters fragment only (no target)
        if not action and pattern:
            if pattern in lights.get_pattern_metadata():
                return render_template(
                    "setup/pattern_params.html",
                    _pattern_params_context(pattern, show_target=False),
                )

            return '<p class="text-danger">Invalid pattern.</p>'

        if "effects" not in lights.settings:
            lights.settings["effects"] = {}

        if action == "update_effect" and effect_name and pattern:
            old_effect_name: str = self.request.form_data.get("old_effect_name", "").strip()

            # Build effect dict without target (target lives in scenes)
            effect_dict: dict = _parse_effect_from_form(self.request.form_data, pattern)
            effect_dict.pop("target", None)

            # If renaming, delete the old entry first
            if old_effect_name and old_effect_name != effect_name and old_effect_name in lights.settings["effects"]:
                del lights.settings["effects"][old_effect_name]

            lights.settings["effects"][effect_name] = effect_dict
            lights.settings_object.store()

        elif action == "delete_effect" and effect_name:
            if effect_name in lights.settings.get("effects", {}):
                del lights.settings["effects"][effect_name]
                lights.settings_object.store()

            return render_template(
                "setup/effects.html",
                {
                    "effects": _effects_list(lights.settings.get("effects", {})),
                    "page_title": "Effects",
                },
            )

        return render_template(
            "setup/effects.html",
            {
                "effects": _effects_list(lights.settings.get("effects", {})),
                "page_title": "Effects",
            },
        )


def _filter_edit_context(filter_name: str = None) -> dict:
    """Build template context for the filter editor."""

    filters_dict: dict = lights.settings.get("filters", {})
    filter_metadata: dict = lights.get_filter_metadata()
    context: dict = {
        "filters": _filters_list(filters_dict),
        "filter_metadata": filter_metadata,
        "page_title": "Filters",
    }

    if filter_name and filter_name in filters_dict:
        filter_def: dict = filters_dict[filter_name]
        filter_type: str = filter_def.get("filter", "")
        context["edit_filter_name"] = filter_name
        context["edit_filter_type"] = filter_type
        context["old_filter_name"] = filter_name
        # Pre-fill optional params
        if filter_type in filter_metadata:
            for param_name in filter_metadata[filter_type].get("optional", []):
                if param_name in filter_def:
                    context["filter_val_" + param_name] = str(filter_def[param_name])

    return context


class FiltersView(View):
    """List filters and create or delete filters."""

    def get(self) -> str:
        """Show filter list and create-filter form."""

        context: dict = {
            "filters": _filters_list(lights.settings.get("filters", {})),
            "page_title": "Filters",
        }
        return render_template("setup/filters.html", context)

    def post(self) -> str:
        """Create or delete a filter."""

        action: str = self.request.form_data.get("action", "").strip()
        filter_name: str = self.request.form_data.get("filter_name", "").strip()

        if "filters" not in lights.settings:
            lights.settings["filters"] = {}

        if action == "create_filter" and filter_name:
            if filter_name not in lights.settings["filters"]:
                lights.settings["filters"][filter_name] = {"filter": "sizzle"}
            lights.settings_object.store()
            return render_template("setup/filter_edit.html", _filter_edit_context(filter_name))

        elif action == "delete_filter" and filter_name and filter_name in lights.settings["filters"]:
            del lights.settings["filters"][filter_name]
            lights.settings_object.store()

        context: dict = {
            "filters": _filters_list(lights.settings.get("filters", {})),
            "page_title": "Filters",
        }
        return render_template("setup/filters.html", context)


class FilterEditView(View):
    """Edit an individual filter's type and parameters."""

    def get(self) -> str:
        """Show filter editor for the given filter."""

        filter_name: str = self.request.query_params.get("filter", "").strip()

        if not filter_name or filter_name not in lights.settings.get("filters", {}):
            return '<p class="text-danger small">Filter not found.</p>'

        return render_template("setup/filter_edit.html", _filter_edit_context(filter_name))

    def post(self) -> str:
        """Update a filter's type and parameters."""

        action: str = self.request.form_data.get("action", "").strip()
        filter_name: str = self.request.form_data.get("filter_name", "").strip()
        filter_type: str = self.request.form_data.get("filter_type", "").strip()

        if "filters" not in lights.settings:
            lights.settings["filters"] = {}

        if action == "update_filter" and filter_name and filter_type:
            old_filter_name: str = self.request.form_data.get("old_filter_name", "").strip()

            filter_def: dict = {"filter": filter_type}
            filter_metadata: dict = lights.get_filter_metadata()
            for param_name in filter_metadata.get(filter_type, {}).get("optional", []):
                param_value: str = self.request.form_data.get("filter_param_" + param_name, "").strip()
                if param_value:
                    try:
                        filter_def[param_name] = float(param_value) if "." in param_value else int(param_value)
                    except ValueError:
                        filter_def[param_name] = param_value

            if old_filter_name and old_filter_name != filter_name and old_filter_name in lights.settings["filters"]:
                del lights.settings["filters"][old_filter_name]

            lights.settings["filters"][filter_name] = filter_def
            lights.settings_object.store()

        elif action == "delete_filter" and filter_name:
            if filter_name in lights.settings.get("filters", {}):
                del lights.settings["filters"][filter_name]
                lights.settings_object.store()

            return render_template(
                "setup/filters.html",
                {
                    "filters": _filters_list(lights.settings.get("filters", {})),
                    "page_title": "Filters",
                },
            )

        return render_template(
            "setup/filters.html",
            {
                "filters": _filters_list(lights.settings.get("filters", {})),
                "page_title": "Filters",
            },
        )


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
    """Manage entries within a specific scene.

    Each scene entry maps a named effect to a target LED specification.
    """

    def get(self) -> str:
        """Show entry list for the given scene, optionally pre-loaded to edit an entry."""

        scene_name: str = self.request.query_params.get("scene", "").strip()

        if not scene_name or scene_name not in lights.settings.get("scenes", {}):
            return '<p class="text-danger small">Scene not found.</p>'

        edit_entry_name: str = self.request.query_params.get("edit_entry", "").strip()
        return render_template("setup/scene_edit.html", _scene_edit_context(scene_name, edit_entry_name or None))

    def post(self) -> str:
        """Add/update or delete a scene entry (effect + target mapping)."""

        action: str = self.request.form_data.get("action", "").strip()
        scene_name: str = self.request.form_data.get("scene_name", "").strip()
        entry_name: str = self.request.form_data.get("entry_name", "").strip()
        effect_name: str = self.request.form_data.get("effect_name", "").strip()
        target: str = self.request.form_data.get("target", "").strip()

        if scene_name and scene_name in lights.settings.get("scenes", {}):
            if action in ("add_entry", "update_entry") and entry_name and effect_name:
                old_entry_name: str = self.request.form_data.get("old_entry_name", "").strip()

                # If renaming, delete the old entry first
                if (
                    old_entry_name
                    and old_entry_name != entry_name
                    and old_entry_name in lights.settings["scenes"][scene_name]
                ):
                    del lights.settings["scenes"][scene_name][old_entry_name]

                entry_dict: dict = {
                    "effect": effect_name,
                    "target": target or "all",
                }

                cycles_value: str = self.request.form_data.get("cycles", "").strip()
                if cycles_value:
                    try:
                        entry_dict["cycles"] = int(cycles_value)
                    except ValueError:
                        pass

                lights.settings["scenes"][scene_name][entry_name] = entry_dict
                lights.settings_object.store()

            elif action == "delete_entry" and entry_name:
                if entry_name in lights.settings["scenes"][scene_name]:
                    del lights.settings["scenes"][scene_name][entry_name]
                    lights.settings_object.store()

        return render_template("setup/scene_edit.html", _scene_edit_context(scene_name))


class ColorSelectView(View):
    """Return an HTML fragment for the color display area when the color dropdown changes."""

    def post(self) -> str:
        """Return a pill, color picker, or empty based on the selected color."""

        color_index: str = self.request.form_data.get("color_index", "0").strip()
        color_value: str = self.request.form_data.get(f"param_color_{color_index}", "").strip()
        custom_colors: dict = lights.settings.get("custom_colors", {})

        is_named: bool = bool(color_value and color_value != "__picker__" and not color_value.startswith("#"))
        is_picker: bool = color_value == "__picker__" or color_value.startswith("#")

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
            hex_val = color_value if color_value.startswith("#") else "#FF0000"

        context: dict = {
            "color_index": color_index,
            "color_name": stored_name,
            "color_display_name": display_name,
            "color_hex_val": hex_val,
            "is_named": is_named,
            "is_picker": is_picker,
        }

        return render_template("setup/color_select.html", context)
