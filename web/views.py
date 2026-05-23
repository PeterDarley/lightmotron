from webserver import View, render_template, Response
from storage import PersistentDict
from lighting import Lighting
from comms import WIFIManager
from ota_update import OTAUpdater
import gc
import io
import json
import os
import sys
import machine
import settings
from web import views_named_ranges


def _fmt_bytes(num_bytes: int) -> str:
    """Format a byte count as a human-readable string (KB or MB)."""

    if num_bytes >= 1024 * 1024:
        return "{:.1f} MB".format(num_bytes / (1024 * 1024))
    elif num_bytes >= 1024:
        return "{:.1f} KB".format(num_bytes / 1024)
    else:
        return "{} B".format(num_bytes)


def summarize_led_list(led_indices: list) -> str:
    """Return a compact human-readable summary of a list of LED indices.

    A single contiguous run returns "x-y" (or "x" for a single LED).
    Two contiguous runs return "a-b, x-y".
    Three or more runs (or an otherwise complex set) return "x to y".
    An empty list returns "none".
    """

    if not led_indices:
        return "none"

    sorted_indices = sorted(set(led_indices))

    segments = []
    start = sorted_indices[0]
    end = sorted_indices[0]

    for idx in sorted_indices[1:]:
        if idx == end + 1:
            end = idx
        else:
            segments.append((start, end))
            start = idx
            end = idx

    segments.append((start, end))

    def _fmt_seg(s: int, e: int) -> str:
        """Format a single contiguous segment as 'x' or 'x-y'."""

        if s == e:
            return str(s)
        return "{}-{}".format(s, e)

    if len(segments) == 1:
        return _fmt_seg(segments[0][0], segments[0][1])
    elif len(segments) == 2:
        return "{}, {}".format(_fmt_seg(*segments[0]), _fmt_seg(*segments[1]))
    else:
        return "{} to {}".format(sorted_indices[0], sorted_indices[-1])


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
_OTA_REPO_CHARS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")
_ota_last_check: dict = {}
_OTA_TRACKED_ROOT_PREFIXES: tuple = (
    "lib/",
    "web/",
    "templates/",
    "www/",
)
_OTA_TRACKED_ROOT_FILES: tuple = (
    "boot.py",
    "main.py",
)
_OTA_EXCLUDED_PATH_PREFIXES: tuple = (
    ".git/",
    ".github/",
    "copilot_working/",
    "deployment/",
    "external_resources/",
)
_OTA_EXCLUDED_PATHS: tuple = (
    "upload.ps1",
    "repl.ps1",
    "upload.sh",
    "index.html",
    "requirements.txt",
    "lib/licence",
)
_OTA_EXCLUDED_LOCAL_DIRS: tuple = (
    ".github",
    "copilot_working",
    "deployment",
    "external_resources",
    "venv",
)


def _rename_named_range_refs(old_name: str, new_name: str) -> None:
    """Update all target fields that reference a named range by its old name."""

    old_ref: str = "named:" + old_name
    new_ref: str = "named:" + new_name
    for scene in lights.settings.get("scenes", {}).values():
        for entry in scene.values():
            if entry.get("target") == old_ref:
                entry["target"] = new_ref
    # Also update references inside other named ranges
    nr = lights.settings.get("named_ranges", {})
    for name, members in nr.items():
        if isinstance(members, list):
            for i, item in enumerate(members):
                if isinstance(item, str) and item == old_ref:
                    members[i] = new_ref


def _rename_effect_refs(old_name: str, new_name: str) -> None:
    """Update all scene entries that reference an effect by its old name."""

    for scene in lights.settings.get("scenes", {}).values():
        for entry in scene.values():
            if entry.get("effect") == old_name:
                entry["effect"] = new_name


def _rename_filter_refs(old_name: str, new_name: str) -> None:
    """Update all effects whose filter lists reference a filter by its old name."""

    for effect in lights.settings.get("effects", {}).values():
        filters: list = effect.get("filters")
        if isinstance(filters, list):
            for i, item in enumerate(filters):
                if item == old_name:
                    filters[i] = new_name


def _rename_scene_refs(old_name: str, new_name: str) -> None:
    """Update scene_settings key and kills lists that reference a scene by its old name."""

    scene_settings: dict = lights.settings.get("scene_settings", {})
    if old_name in scene_settings:
        scene_settings[new_name] = scene_settings.pop(old_name)

    for meta in scene_settings.values():
        kills: list = meta.get("kills")
        if isinstance(kills, list):
            for i, item in enumerate(kills):
                if item == old_name:
                    kills[i] = new_name


def _rename_scene_entry_after_refs(scene_dict: dict, old_entry_name: str, new_entry_name: str) -> None:
    """Update ``after`` fields within a scene when an entry is renamed."""

    for entry in scene_dict.values():
        if not isinstance(entry, dict):
            continue

        if entry.get("after") == old_entry_name:
            entry["after"] = new_entry_name


def _clear_scene_entry_after_refs(scene_dict: dict, removed_entry_name: str) -> None:
    """Remove dangling ``after`` references to a deleted scene entry."""

    for entry in scene_dict.values():
        if not isinstance(entry, dict):
            continue

        if entry.get("after") == removed_entry_name:
            del entry["after"]


def _rename_color_refs(old_name: str, new_name: str) -> None:
    """Update all effect color lists that reference a custom color by its old name."""

    old_ref: str = "custom:" + old_name
    new_ref: str = "custom:" + new_name
    for effect in lights.settings.get("effects", {}).values():
        colors_list: list = effect.get("colors")
        if isinstance(colors_list, list):
            for i, item in enumerate(colors_list):
                if item == old_ref:
                    colors_list[i] = new_ref


# Named range views are grouped into a dedicated module by feature area.
views_named_ranges.initialize_named_range_views(
    lights_instance=lights,
    rename_named_range_refs_func=_rename_named_range_refs,
    summarize_led_list_func=summarize_led_list,
)
NamedRangeSummaryView = views_named_ranges.NamedRangeSummaryView
NamedRangeView = views_named_ranges.NamedRangeView
NamedRangeSetView = views_named_ranges.NamedRangeSetView
NamedRangeRemoveSubrangeView = views_named_ranges.NamedRangeRemoveSubrangeView


def _animation_context() -> dict:
    """Return a context dict with the current animation running state."""

    running = lights.animation.running
    return {"animation_running": running, "animation_stopped": not running}


def _scenes_context() -> dict:
    """Return a context dict with the scenes list and active scenes."""

    scene_names = sorted(lights.settings["scenes"].keys())
    ongoing_scenes = [name for name in scene_names if lights.is_scene_ongoing(name)]
    immediate_scenes = [name for name in scene_names if not lights.is_scene_ongoing(name)]
    # Home panel only treats ongoing scenes as "active". Immediate scenes are
    # one-shot triggers and are not shown in the active-scenes label/list.
    active_scenes = [name for name in lights._active_scenes if name in ongoing_scenes]

    # Ensure label construction is robust: convert non-string entries to
    # strings so join() does not raise if a malformed value appears.
    active_label = ", ".join([str(s) for s in active_scenes]) if active_scenes else "—"

    return {
        "scenes": scene_names,
        "current_scene": lights.scene_name,
        "active_scenes": active_scenes,
        "active_scenes_label": active_label,
        "ongoing_scenes": ongoing_scenes,
        "immediate_scenes": immediate_scenes,
    }


def _models_context() -> dict:
    """Return a context dict describing available models and the current model."""

    return {"models": lights.get_model_names(), "current_model": lights.current_model_name}


class ModelsSummaryView(View):
    """Return a small summary snippet for the Models card in Setup."""

    def get(self) -> str:
        ctx = _models_context()
        return render_template(
            "setup/models_summary.html", {"current_model": ctx["current_model"], "model_count": len(ctx["models"])}
        )


class ModelsView(View):
    """Full modal view for managing models."""

    def get(self) -> str:
        ctx = _models_context()
        return render_template(
            "setup/models.html",
            {"models": ctx["models"], "current_model": ctx["current_model"], "page_title": "Models"},
        )

    def post(self) -> str:
        action = self.request.form_data.get("action", "set").strip()
        name = self.request.form_data.get("name", "").strip()
        new_name = self.request.form_data.get("new_name", "").strip()

        if action == "set" and name:
            try:
                lights.set_current_model(name)
                # Instruct HTMX to reload the setup page so summaries update
                return Response(status=200, reason="OK", body="", headers={"HX-Redirect": "/setup"})
            except Exception as e:
                return str(e), 400
        elif action == "create" and name:
            try:
                lights.create_model(name)
            except Exception as e:
                return str(e), 400
        elif action == "delete" and name:
            try:
                lights.delete_model(name)
            except Exception as e:
                return str(e), 400
        elif action == "rename" and name and new_name:
            try:
                lights.rename_model(name, new_name)
            except Exception as e:
                return str(e), 400

        # Return updated modal
        return self.get()


class ModelsSetView(View):
    def post(self) -> str:
        name = self.request.form_data.get("model", "").strip()
        if name:
            try:
                lights.set_current_model(name)
            except Exception as e:
                return str(e), 400

        # Ask the client to reload the setup page so the UI reflects the new active model.
        return Response(status=200, reason="OK", body="", headers={"HX-Redirect": "/setup"})


class ModelsWrapView(View):
    """Wrap legacy top-level lighting_settings into a model called 'Model'."""

    def post(self) -> str:
        try:
            lights.wrap_current_settings_into_model("Model")
            return self.get()
        except Exception as e:
            return str(e), 400


class HomeView(View):

    def get(self) -> str:
        """Handle GET requests for the home route."""

        context = {"message": "Lighting", "page_title": "Home"}
        context.update(_scenes_context())
        context.update(_animation_context())
        context.update(_sounds_context(include_playing=True, home_only=True))

        # Get current master volume from persistent storage
        storage: PersistentDict = PersistentDict()
        system_settings: dict = storage.get("system_settings", {})
        current_volume = system_settings.get("master_volume", 20)
        context["current_volume"] = current_volume

        return render_template("home.html", context)


class AudioVolumeView(View):
    """Handle volume control for audio players."""

    def post(self) -> str:
        """Set the master volume for all audio players (0-30).

        Reads the volume value from the form data (typically from a range input).
        Sends the volume command to all configured audio players and persists
        the setting for future power-ups.
        """

        try:
            volume_str = self.request.form_data.get("master-volume", "20")
            volume = int(volume_str)
            volume = max(0, min(30, volume))  # Clamp to valid range
        except (ValueError, TypeError):
            volume = 20

        # Persist the volume setting to disk
        storage: PersistentDict = PersistentDict()
        system_settings: dict = storage.get("system_settings", {})
        system_settings["master_volume"] = volume
        storage["system_settings"] = system_settings
        storage.store()  # Explicitly persist to disk

        # Send volume command to all active players and update the cached master volume
        try:
            from audio import AudioPlayer

            player = AudioPlayer()
            player.master_volume = volume
            for p in player.players:
                p.set_volume(volume)
        except Exception as e:
            print(f"AudioVolumeView: failed to set volume: {e}")

        return f"{volume}/30"


class SetSceneView(View):
    """Handle POST requests to set or modify the current lighting scene(s)."""

    def post(self) -> str | tuple | None:
        """Set, add, or remove an active scene.

        action=set (default): replace all active scenes with the given scene.
        action=add: add the scene to the active set without clearing others.
        action=remove: remove the scene from the active set.
        """

        scene_name = self.request.form_data.get("scene")
        action = self.request.form_data.get("action", "set")

        if scene_name not in lights.settings["scenes"]:
            return "Invalid scene", 400

        if action == "add":
            lights.add_scene(scene_name)
        elif action == "remove":
            lights.remove_scene(scene_name)
        else:
            extra_kwargs = {
                key: value for key, value in self.request.form_data.items() if key not in ("scene", "action")
            }
            lights.set_scene(scene_name, **extra_kwargs)

        return render_template("scenes/scene_panel.html", _scenes_context())


class AnimationView(View):
    """Handle POST requests to start or stop the lighting animation."""

    def post(self) -> str | tuple:
        """Start or stop the animation based on the POST data."""
        action = self.request.form_data.get("action")
        if action == "start":
            lights.animation.start()
        elif action == "stop":
            lights.animation.stop()
        else:
            return "Invalid action", 400

        return render_template("animation/buttons.html", _animation_context())


def _redact_system_settings(storage_dict: dict) -> dict:
    """Return a copy of *storage_dict* with the WiFi password replaced by ``***``.

    Operates on a shallow copy to avoid mutating the live storage object.
    """

    result = {k: v for k, v in storage_dict.items()}
    sys_settings = result.get("system_settings")
    if isinstance(sys_settings, dict):
        wifi = sys_settings.get("wifi")
        if isinstance(wifi, dict) and "password" in wifi:
            redacted_wifi = {k: ("***" if k == "password" else v) for k, v in wifi.items()}
            redacted_sys = {k: (redacted_wifi if k == "wifi" else v) for k, v in sys_settings.items()}
            result["system_settings"] = redacted_sys

    return result


class StorageView(View):
    """Display the persistent storage dictionary as pretty-printed JSON."""

    def get(self) -> str | tuple:
        """Return the storage contents as JSON."""

        try:
            storage = PersistentDict()
            storage_dict = _redact_system_settings({k: v for k, v in storage.items()})
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
        """Return the setup page with all summary data rendered inline."""

        context: dict = {"page_title": "Setup"}
        context.update(_system_settings_context())
        context.update(_ota_summary_context())

        # Include all summary card data so the browser receives everything in one
        # request rather than firing 5+ lazy HTMX loads after page paint.
        context["named_ranges"] = views_named_ranges.get_named_range_summary_entries("name")
        context["named_range_names"] = [range_entry["name"] for range_entry in context["named_ranges"]]

        custom_colors_dict: dict = lights.settings.get("custom_colors", {})
        context["custom_colors"] = sorted(
            [
                (name, tuple(value) if isinstance(value, (list, tuple)) else value)
                for name, value in custom_colors_dict.items()
            ],
            key=lambda item: item[0],
        )

        context["scenes"] = _scenes_list(lights.settings.get("scenes", {}))
        context["effect_names"] = sorted(lights.settings.get("effects", {}).keys())
        context["filter_names"] = sorted(lights.settings.get("filters", {}).keys())
        context.update(_sounds_context())

        return render_template("setup.html", context)


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
        # Gather audio health from the SoundManager (if available)
        audio_health_list = []
        audio_health_summary = "No audio modules configured"
        try:
            from sounds import SoundManager

            sm = SoundManager()
            raw_health = sm.get_last_health() or {}
            # Convert to sorted list for templating
            keys = sorted(raw_health.keys())
            for k in keys:
                info = raw_health.get(k, {})
                audio_health_list.append(
                    {
                        "index": k,
                        "uart": info.get("uart"),
                        "ok": bool(info.get("ok")),
                        "state": info.get("state"),
                    }
                )
            if audio_health_list:
                healthy = sum(1 for it in audio_health_list if it.get("ok"))
                audio_health_summary = f"{healthy}/{len(audio_health_list)} responsive"
        except Exception:
            audio_health_list = []
            audio_health_summary = "Audio health not available"

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
                "wifi_ssid": PersistentDict().get("system_settings", {}).get("wifi", {}).get("ssid", ""),
                "animation_running": str(lights.animation.running),
                "current_scene": lights.scene_name,
                "active_model": getattr(lights, "current_model_name", None),
                "tick_number": lights.animation.tick_number,
                "restore_message": restore_message,
                "restore_class": restore_class,
                "audio_health": audio_health_list,
                "audio_health_summary": audio_health_summary,
                "page_title": "Status",
            },
        )


class BackupView(View):
    """Serve the persistent storage as a downloadable JSON file."""

    def get(self) -> Response:
        """Return the storage dict as a JSON download.

        WiFi SSID and password are excluded from the download so that
        restoring a backup on a different device does not overwrite its
        network credentials.
        """

        storage = PersistentDict()
        storage_dict: dict = {k: v for k, v in storage.items()}

        # Strip WiFi credentials so they are not overwritten by a restore
        sys_settings: dict = storage_dict.get("system_settings")
        if isinstance(sys_settings, dict) and "wifi" in sys_settings:
            scrubbed_sys: dict = {k: v for k, v in sys_settings.items() if k != "wifi"}
            storage_dict["system_settings"] = scrubbed_sys

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
        try:
            lights._load_lighting_root()
        except Exception:
            pass

        return Response(
            status=302,
            reason="Found",
            headers={"Location": "/status?restore=ok"},
        )


class RestoreConfirmView(View):
    """Return a small confirmation fragment for restoring settings."""

    def post(self) -> str:
        backup_json = ""

        backup_file = self.request.files.get("backup_file", {})
        if backup_file and isinstance(backup_file, dict):
            try:
                backup_json = backup_file.get("data", b"").decode("utf-8").strip()
            except Exception:
                backup_json = ""

        if not backup_json:
            backup_json = self.request.form_data.get("backup_json", "").strip()

        if not backup_json:
            return '<div class="alert alert-warning small py-2 mb-2">No backup JSON file selected.</div>'

        try:
            parsed_json = json.loads(backup_json)
        except (ValueError, TypeError):
            return '<div class="alert alert-danger small py-2 mb-2">Selected file does not contain valid JSON.</div>'

        if not isinstance(parsed_json, dict):
            return '<div class="alert alert-danger small py-2 mb-2">Backup JSON must contain a top-level object.</div>'

        return render_template("setup/restore_confirm.html", {"backup_json": backup_json})


class ConfirmView(View):
    """Generic confirmation fragment for destructive actions.

    Expects POST fields:
    - object_type: human-friendly type (e.g., 'Model', 'Named Range')
    - object_name: name of the object
    - action_path: URL to POST the final delete to (e.g., '/models')
    Any additional fields sent are preserved and emitted as hidden inputs
    in the confirmation form so the final POST contains the same data.
    """

    def post(self) -> str:
        form = self.request.form_data
        object_type = form.get("object_type", "item")
        object_name = form.get("object_name", "")
        action_path = form.get("action_path", "")

        # Fallback guesses for object_name if not provided
        if not object_name:
            for key in ("name", "range_name", "old_name", "color_name", "theme"):
                if key in form:
                    object_name = form.get(key)
                    break

        if not action_path:
            # Infer a reasonable default mapping
            mapping = {"Model": "/models", "Named Range": "/named_range", "named_range": "/named_range"}
            action_path = mapping.get(object_type, "/")

        # Collect remaining fields to re-post on confirmation
        fields = {}
        for k, v in form.items():
            if k in ("object_type", "object_name", "action_path"):
                continue
            fields[k] = v

        message = "Delete {} '{}' ? This action cannot be undone.".format(object_type, object_name)
        cancel_url = action_path

        return render_template(
            "setup/confirm_delete.html",
            {"message": message, "action_url": action_path, "fields": fields, "cancel_url": cancel_url},
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
                old_color_name: str = self.request.form_data.get("old_color_name", "").strip()
                if action == "update" and old_color_name and old_color_name != color_name:
                    if old_color_name in lights.settings["custom_colors"]:
                        del lights.settings["custom_colors"][old_color_name]
                    _rename_color_refs(old_color_name, color_name)
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


def _list_theme_files() -> list:
    """Return sorted list of CSS filenames found in www/themes/.

    Returns an empty list if the directory does not exist.
    """

    try:
        entries = os.listdir("www/themes")
    except OSError:
        return []

    return sorted(name for name in entries if name.endswith(".css"))


def _theme_display_name(filename: str) -> str:
    """Convert a theme filename to a human-readable display name.

    Strips the ``.css`` extension and replaces underscores with spaces.
    """

    name = filename
    if name.endswith(".css"):
        name = name[:-4]

    return name.replace("_", " ")


def _theme_response(message: str = "", error: str = "") -> str:
    """Render the theme picker fragment with current theme and available files.

    Optional *message* or *error* strings are passed to the template for
    inline feedback after actions like upload or delete.
    """

    current_theme: str = PersistentDict().get("system_settings", {}).get("theme", "")
    theme_pairs: list = [[filename, _theme_display_name(filename)] for filename in _list_theme_files()]
    return render_template(
        "setup/theme.html",
        {
            "theme_files": theme_pairs,
            "current_theme": current_theme,
            "message": message,
            "error": error,
        },
    )


class ThemeView(View):
    """Display theme picker and save theme selection to persistent storage."""

    def get(self) -> str:
        """Return the theme picker fragment."""

        return _theme_response()

    def post(self) -> str:
        """Save the selected theme and return the updated picker fragment."""

        theme_filename: str = self.request.form_data.get("theme", "").strip()

        storage: PersistentDict = PersistentDict()
        if "system_settings" not in storage:
            storage["system_settings"] = {}

        storage["system_settings"]["theme"] = theme_filename
        storage.store()

        return Response(status=200, reason="OK", body="", headers={"HX-Redirect": "/setup"})


class ThemeDeleteView(View):
    """Delete a theme CSS file from www/themes/."""

    def post(self) -> str:
        """Delete the named theme file and return the updated picker fragment.

        Refuses to delete the currently active theme or any filename that
        contains path separators (basic path-traversal guard).
        """

        theme_filename: str = self.request.form_data.get("theme", "").strip()

        if not theme_filename or not theme_filename.endswith(".css"):
            return Response(status=400, reason="Bad Request", body="Invalid theme filename.")

        if "/" in theme_filename or "\\" in theme_filename or ".." in theme_filename:
            return Response(status=400, reason="Bad Request", body="Invalid theme filename.")

        current_theme: str = PersistentDict().get("system_settings", {}).get("theme", "")
        if theme_filename == current_theme:
            return Response(status=400, reason="Bad Request", body="Cannot delete the active theme.")

        file_path: str = "www/themes/" + theme_filename
        try:
            os.remove(file_path)
        except OSError:
            return Response(status=404, reason="Not Found", body="Theme file not found.")

        return _theme_response()


class ThemeUploadView(View):
    """Handle theme CSS and font file uploads."""

    _ALLOWED_EXTENSIONS: tuple = (".css", ".ttf", ".woff", ".woff2", ".otf")
    _FONT_EXTENSIONS: tuple = (".ttf", ".woff", ".woff2", ".otf")
    _MAX_FILE_SIZE: int = 512 * 1024

    def post(self) -> str:
        """Save an uploaded theme or font file and return the updated picker fragment.

        Validates the file extension, rejects path traversal attempts, and
        saves CSS files to ``www/themes/`` and font files to ``www/themes/fonts/``.
        """

        uploaded: dict = self.request.files.get("file")
        if not uploaded:
            return _theme_response(error="No file selected.")

        filename: str = uploaded["filename"].strip()
        if not filename:
            return _theme_response(error="No filename provided.")

        # Security: reject path separators and directory traversal
        if "/" in filename or "\\" in filename or ".." in filename:
            return _theme_response(error="Invalid filename.")

        # Validate extension
        lower_name: str = filename.lower()
        if not any(lower_name.endswith(ext) for ext in self._ALLOWED_EXTENSIONS):
            return _theme_response(error="Only .css, .ttf, .woff, .woff2, and .otf files are allowed.")

        # Check file size
        file_data: bytes = uploaded["data"]
        if len(file_data) > self._MAX_FILE_SIZE:
            return _theme_response(error="File too large (max 512 KB).")

        # Determine target directory
        if any(lower_name.endswith(ext) for ext in self._FONT_EXTENSIONS):
            target_dir = "www/themes/fonts"
        else:
            target_dir = "www/themes"

        # Ensure target directory exists
        try:
            os.stat(target_dir)
        except OSError:
            os.mkdir(target_dir)

        # Write file to disk
        file_path: str = target_dir + "/" + filename
        with open(file_path, "wb") as destination:
            destination.write(file_data)

        return _theme_response(message="Uploaded " + filename)


_HOSTNAME_RE_CHARS = set("abcdefghijklmnopqrstuvwxyz0123456789-")


def _hostname_response(message: str = "", error: str = "") -> str:
    """Render the hostname form fragment."""

    hostname: str = PersistentDict().get("system_settings", {}).get("hostname", "")
    return render_template(
        "setup/hostname.html",
        {
            "hostname": hostname,
            "message": message,
            "error": error,
        },
    )


class HostnameView(View):
    """Display and save the local mDNS hostname."""

    def get(self) -> str:
        """Return the hostname form fragment."""

        return _hostname_response()

    def post(self) -> str:
        """Validate, save, apply the hostname, and restart the network.

        After saving, the mDNS hostname is applied immediately and WiFi is
        restarted in a background thread.  The browser receives a short
        countdown page that redirects to the new ``<hostname>.local`` URL
        after five seconds.
        """

        raw_hostname: str = self.request.form_data.get("hostname", "").strip().lower()

        if not raw_hostname:
            storage: PersistentDict = PersistentDict()
            if "system_settings" not in storage:
                storage["system_settings"] = {}

            storage["system_settings"]["hostname"] = ""
            storage.store()
            raw_hostname = "lightmotron"
            self._apply_and_restart(raw_hostname)
            return self._redirect_response(raw_hostname)

        if len(raw_hostname) > 32:
            return _hostname_response(error="Hostname must be 32 characters or fewer.")

        if raw_hostname.startswith("-") or raw_hostname.endswith("-"):
            return _hostname_response(error="Hostname cannot start or end with a hyphen.")

        if not all(character in _HOSTNAME_RE_CHARS for character in raw_hostname):
            return _hostname_response(error="Only letters, numbers, and hyphens are allowed.")

        storage: PersistentDict = PersistentDict()
        if "system_settings" not in storage:
            storage["system_settings"] = {}

        storage["system_settings"]["hostname"] = raw_hostname
        storage.store()
        self._apply_and_restart(raw_hostname)
        return self._redirect_response(raw_hostname)

    @staticmethod
    def _apply_and_restart(hostname: str) -> None:
        """Set the mDNS hostname and restart WiFi in a background thread."""

        import network
        import _thread
        from time import sleep

        network.hostname(hostname)

        def _restart_wifi() -> None:
            """Disconnect and reconnect WiFi so mDNS picks up the new name."""

            sleep(0.5)
            wifi = WIFIManager()
            wifi.sta_if.disconnect()
            sleep(1)
            wifi.sta_if.connect(wifi.ssid, wifi.password)

        _thread.start_new_thread(_restart_wifi, ())

    @staticmethod
    def _redirect_response(hostname: str) -> str:
        """Return an HTML fragment that redirects to the new hostname after five seconds."""

        new_url: str = "http://" + hostname + ".local/setup"
        return (
            '<div class="alert alert-info small py-2">'
            "Hostname updated. Reconnecting network&hellip;"
            "</div>"
            '<p class="small text-muted">Redirecting to <strong>' + new_url + "</strong> in 5 seconds.</p>"
            '<script>setTimeout(function(){window.location.href="' + new_url + '";},5000);</script>'
        )


_COLOR_ORDERS: list = ["GRB", "RGB", "BGR", "BRG", "RBG", "GBR", "GRBW", "RGBW"]


def _parse_neopixels_storage(raw) -> list:
    """Normalise whatever is in storage for 'neopixels' into a list of strip dicts.

    Accepts the legacy single-dict format ``{pin, num, brightness_curve}`` or the
    new list-of-dicts format ``[{pin, num, color_order, brightness_curve}, ...]``.
    Always returns a non-empty list.
    """

    if isinstance(raw, list) and raw:
        strips = []
        for item in raw:
            if isinstance(item, dict) and "pin" in item and "num" in item:
                strips.append(
                    {
                        "pin": int(item.get("pin", 4)),
                        "num": int(item.get("num", 30)),
                        "color_order": item.get("color_order", "GRB").upper(),
                        "brightness_curve": bool(item.get("brightness_curve", True)),
                    }
                )
        if strips:
            return strips

    if isinstance(raw, dict):
        return [
            {
                "pin": int(raw.get("pin", 4)),
                "num": int(raw.get("num", 144)),
                "color_order": raw.get("color_order", "GRB").upper(),
                "brightness_curve": bool(raw.get("brightness_curve", True)),
            }
        ]

    # Fallback default
    return [{"pin": 4, "num": 144, "color_order": "GRB", "brightness_curve": True}]


def _system_settings_context() -> dict:
    """Build template context from current system_settings in persistent storage."""

    sys_settings: dict = PersistentDict().get("system_settings", {})
    wifi: dict = sys_settings.get("wifi", {})
    strips: list = _parse_neopixels_storage(sys_settings.get("neopixels"))
    audio_players: list = sys_settings.get("audio_players", [])

    return {
        "ss_wifi_ssid": wifi.get("ssid", ""),
        "ss_hostname": sys_settings.get("hostname", ""),
        "ss_strips": strips,
        "ss_color_orders": _COLOR_ORDERS,
        "ss_color_orders_json": json.dumps(_COLOR_ORDERS),
        "ss_audio_players": audio_players,
    }


def _ota_repo_settings() -> dict:
    """Return OTA repository settings from persistent storage."""

    system_settings: dict = PersistentDict().get("system_settings", {})
    ota_settings: dict = system_settings.get("ota", {})

    repo_owner: str = ota_settings.get("repo_owner", "PeterDarley")
    repo_name: str = ota_settings.get("repo_name", "lightmotron")

    if not repo_owner:
        repo_owner = "PeterDarley"
    if not repo_name:
        repo_name = "lightmotron"

    return {"repo_owner": repo_owner, "repo_name": repo_name}


def _ota_summary_context() -> dict:
    """Return setup card summary context for OTA updates."""

    repo_settings = _ota_repo_settings()
    pending_count: int = 0
    if _ota_last_check and _ota_last_check.get("has_updates"):
        pending_count = len(_ota_last_check.get("updates", []))

    return {
        "repo_owner": repo_settings["repo_owner"],
        "repo_name": repo_settings["repo_name"],
        "pending_update_count": pending_count,
    }


def _load_last_deploy_commit() -> dict:
    """Load the last deployed commit SHA and branch from .ota_deployed_commit.json."""

    try:
        import json

        with open(".ota_deployed_commit.json", "r") as file_handle:
            data: dict = json.load(file_handle)
            commit_sha: str = data.get("commit_sha", "")
            # Shorten to 8 characters like git does
            short_sha: str = commit_sha[:8] if commit_sha else ""
            return {
                "commit_sha": short_sha,
                "branch": data.get("branch", ""),
            }
    except Exception:
        return {"commit_sha": "", "branch": ""}


def _validate_repo_part(value: str) -> bool:
    """Validate owner/repository segment characters for GitHub slugs."""

    if not value:
        return False

    if len(value) > 64:
        return False

    if value.startswith(".") or value.endswith("."):
        return False

    return all(character in _OTA_REPO_CHARS for character in value)


def _updates_context(
    message: str = "",
    error: str = "",
    check_result: dict = None,
    apply_result: dict = None,
    remove_deleted: bool = False,
) -> dict:
    """Build template context for the OTA updates modal."""

    repo_settings = _ota_repo_settings()
    result = check_result if check_result is not None else _ota_last_check

    if result:
        updates = result.get("updates", [])
        has_updates = bool(result.get("has_updates"))
        checked_repository = result.get("repository", repo_settings["repo_owner"] + "/" + repo_settings["repo_name"])
        branch = result.get("branch", "")
    else:
        updates = []
        has_updates = False
        checked_repository = ""
        branch = ""

    # Load last deployed commit info
    last_deploy_info = _load_last_deploy_commit()

    return {
        "repo_owner": repo_settings["repo_owner"],
        "repo_name": repo_settings["repo_name"],
        "checked_repository": checked_repository,
        "checked_branch": branch,
        "has_checked": bool(result),
        "has_updates": has_updates,
        "updates": updates,
        "update_count": len(updates),
        "message": message,
        "error": error,
        "apply_result": apply_result or {},
        "remove_deleted": bool(remove_deleted),
        "last_deploy_commit": last_deploy_info.get("commit_sha", ""),
        "last_deploy_branch": last_deploy_info.get("branch", ""),
    }


class UpdatesSummaryView(View):
    """Return a summary snippet of OTA update settings for the setup card."""

    def get(self) -> str:
        """Return updates summary HTML fragment."""

        return render_template("setup/updates_summary.html", _ota_summary_context())


class UpdatesView(View):
    """Display and execute OTA update checks and update application."""

    def get(self) -> str:
        """Return the updates management fragment."""

        return render_template("setup/updates.html", _updates_context())

    def post(self) -> str:
        """Handle repository changes, update checks, and update application."""

        global _ota_last_check

        action: str = self.request.form_data.get("action", "check").strip()

        if action == "save_repo":
            repo_owner: str = self.request.form_data.get("repo_owner", "").strip()
            repo_name: str = self.request.form_data.get("repo_name", "").strip()

            if not _validate_repo_part(repo_owner) or not _validate_repo_part(repo_name):
                return render_template(
                    "setup/updates.html",
                    _updates_context(error="Repository must use only letters, numbers, dash, underscore, or period."),
                )

            storage: PersistentDict = PersistentDict()
            if "system_settings" not in storage:
                storage["system_settings"] = {}

            existing_settings: dict = dict(storage.get("system_settings", {}))
            existing_ota: dict = dict(existing_settings.get("ota", {}))
            existing_ota.update({"repo_owner": repo_owner, "repo_name": repo_name})
            existing_settings["ota"] = existing_ota
            storage["system_settings"] = existing_settings
            storage.store()
            _ota_last_check = {}

            return render_template(
                "setup/updates.html",
                _updates_context(message="Repository updated to {}/{}".format(repo_owner, repo_name)),
            )

        repo_settings = _ota_repo_settings()
        ota_settings: dict = PersistentDict().get("system_settings", {}).get("ota", {})
        track_submodules: bool = bool(ota_settings.get("track_submodules", True))
        debug_logging: bool = bool(ota_settings.get("debug_logging", True))  # Enabled by default for troubleshooting

        updater = OTAUpdater(
            repo_owner=repo_settings["repo_owner"],
            repo_name=repo_settings["repo_name"],
            tracked_root_prefixes=_OTA_TRACKED_ROOT_PREFIXES,
            tracked_root_files=_OTA_TRACKED_ROOT_FILES,
            excluded_path_prefixes=_OTA_EXCLUDED_PATH_PREFIXES,
            excluded_paths=_OTA_EXCLUDED_PATHS,
            excluded_local_dirs=_OTA_EXCLUDED_LOCAL_DIRS,
            user_agent="lightmotron-ota",
            track_submodules=track_submodules,
            debug_logging=debug_logging,
        )

        if action == "check":
            try:
                check_result = updater.check_for_updates()
                _ota_last_check = check_result
                if check_result.get("has_updates"):
                    message = "Found {} changed file(s).".format(len(check_result.get("updates", [])))
                else:
                    message = "No updates available."

                return render_template(
                    "setup/updates.html", _updates_context(message=message, check_result=check_result)
                )
            except Exception as error:
                return render_template(
                    "setup/updates.html",
                    _updates_context(error="Update check failed: {}".format(str(error))),
                )

        if action == "apply":
            if not _ota_last_check:
                return render_template(
                    "setup/updates.html",
                    _updates_context(error="Run a check first so changed files can be reviewed."),
                )

            remove_deleted: bool = self.request.form_data.get("remove_deleted", "") == "1"

            try:
                apply_result = updater.apply_updates(
                    _ota_last_check.get("branch", "main"),
                    _ota_last_check.get("updates", []),
                    remove_deleted=remove_deleted,
                    commit_sha=_ota_last_check.get("commit_sha", ""),
                )

                check_result = dict(_ota_last_check)
                check_result["updates"] = []
                check_result["has_updates"] = False
                _ota_last_check = check_result

                success_count = len(apply_result.get("applied_files", []))
                removed_count = len(apply_result.get("removed_files", []))
                failed_count = len(apply_result.get("failed_files", []))

                message_parts = ["Applied {} file(s).".format(success_count)]
                if removed_count:
                    message_parts.append("Removed {} file(s).".format(removed_count))
                if failed_count:
                    message_parts.append("{} file(s) failed.".format(failed_count))

                if not failed_count:
                    import _thread
                    from time import sleep

                    def _delayed_reset() -> None:
                        """Wait briefly, then reboot the MCU to load updated files."""

                        sleep(0.75)
                        machine.reset()

                    _thread.start_new_thread(_delayed_reset, ())
                    message_parts.append("Rebooting now to apply changes.")

                return render_template(
                    "setup/updates.html",
                    _updates_context(
                        message=" ".join(message_parts),
                        check_result=check_result,
                        apply_result=apply_result,
                        remove_deleted=remove_deleted,
                    ),
                )
            except Exception as error:
                return render_template(
                    "setup/updates.html",
                    _updates_context(
                        error="Update apply failed: {}".format(str(error)),
                        remove_deleted=remove_deleted,
                    ),
                )

        return render_template("setup/updates.html", _updates_context(error="Unknown action."))


class SystemSettingsSummaryView(View):
    """Return a summary snippet of system settings for the setup card."""

    def get(self) -> str:
        """Return system settings summary HTML fragment."""

        return render_template("setup/system_settings_summary.html", _system_settings_context())


class SystemSettingsView(View):
    """Display and edit all system settings stored under ``system_settings``."""

    def get(self) -> str:
        """Return the system settings edit form fragment."""

        return render_template("setup/system_settings.html", _system_settings_context())

    def post(self) -> str:
        """Validate and save all system settings fields.

        Fields received from the form are validated before being written to
        persistent storage.  Unknown or out-of-range values are silently
        ignored; all other fields are saved.  Returns the updated form
        fragment on success or the form with error messages on validation
        failure.
        """

        fd = self.request.form_data
        error: str = ""

        # --- WiFi ---
        ssid: str = fd.get("wifi_ssid", "").strip()
        submitted_password: str = fd.get("wifi_password", "").strip()
        # Preserve the existing password when the field is left blank
        if submitted_password:
            password: str = submitted_password
        else:
            password: str = PersistentDict().get("system_settings", {}).get("wifi", {}).get("password", "")

        # --- Hostname ---
        raw_hostname: str = fd.get("hostname", "").strip().lower()
        if raw_hostname and (
            len(raw_hostname) > 32
            or raw_hostname.startswith("-")
            or raw_hostname.endswith("-")
            or not all(c in _HOSTNAME_RE_CHARS for c in raw_hostname)
        ):
            error = "Hostname: only letters, numbers, hyphens; max 32 chars; no leading/trailing hyphens."

        # --- NeoPixel strips (multi-strip: parallel arrays from repeated fields) ---
        def _as_list(val, default) -> list:
            if val is None:
                return [default]
            return val if isinstance(val, list) else [val]

        strip_pins = _as_list(fd.get("strip_pin"), "4")
        strip_nums = _as_list(fd.get("strip_num"), "144")
        strip_orders = _as_list(fd.get("strip_order"), "GRB")
        strip_bc = _as_list(fd.get("strip_brightness_curve"), "")

        # Pad shorter lists so all are the same length as strip_pins
        n_strips: int = max(1, len(strip_pins))

        def _pad(lst, length, default) -> list:
            return list(lst) + [default] * (length - len(lst))

        strip_nums = _pad(strip_nums, n_strips, "30")
        strip_orders = _pad(strip_orders, n_strips, "GRB")
        strip_bc = _pad(strip_bc, n_strips, "")

        strips: list = []
        for i in range(n_strips):
            try:
                pin_val: int = int(strip_pins[i])
            except (ValueError, IndexError):
                pin_val = 4
            try:
                num_val: int = max(1, int(strip_nums[i]))
            except (ValueError, IndexError):
                num_val = 30
            order_val: str = (strip_orders[i] if i < len(strip_orders) else "GRB").upper()
            if order_val not in _COLOR_ORDERS:
                order_val = "GRB"
            bc_val: bool = (strip_bc[i] if i < len(strip_bc) else "") == "1"
            strips.append({"pin": pin_val, "num": num_val, "color_order": order_val, "brightness_curve": bc_val})

        if not strips:
            strips = [{"pin": 4, "num": 144, "color_order": "GRB", "brightness_curve": True}]

        # --- Audio players (multi-player: parallel arrays from repeated fields) ---
        audio_uarts = _as_list(fd.get("audio_uart"), None)
        audio_tx_pins = _as_list(fd.get("audio_tx_pin"), None)
        audio_rx_pins = _as_list(fd.get("audio_rx_pin"), None)
        audio_hq = _as_list(fd.get("audio_high_quality"), "")

        # Filter out incomplete entries (all three pins must be present)
        audio_players: list = []
        for i in range(max(len(audio_uarts), len(audio_tx_pins), len(audio_rx_pins))):
            try:
                # Coerce missing/None values to 0 before int() to avoid TypeError
                raw_uart = audio_uarts[i] if i < len(audio_uarts) else 0
                raw_tx = audio_tx_pins[i] if i < len(audio_tx_pins) else 0
                raw_rx = audio_rx_pins[i] if i < len(audio_rx_pins) else 0

                uart_val: int = int(raw_uart) if raw_uart is not None and raw_uart != "" else 0
                tx_val: int = int(raw_tx) if raw_tx is not None and raw_tx != "" else 0
                rx_val: int = int(raw_rx) if raw_rx is not None and raw_rx != "" else 0

                hq_val: bool = (audio_hq[i] if i < len(audio_hq) else "") == "1"
                if uart_val >= 0 and tx_val > 0 and rx_val > 0:
                    audio_players.append(
                        {"uart": uart_val, "tx_pin": tx_val, "rx_pin": rx_val, "high_quality": hq_val}
                    )
            except (ValueError, IndexError, TypeError):
                # Skip invalid/incomplete audio player entries
                pass

        if error:
            context = _system_settings_context()
            context["error"] = error
            return render_template("setup/system_settings.html", context)

        print("SystemSettings: saving {} strip(s)".format(len(strips)))
        for strip in strips:
            print("  strip:", strip)
        print("SystemSettings: saving {} audio player(s)".format(len(audio_players)))
        for player in audio_players:
            print("  player:", player)

        storage: PersistentDict = PersistentDict()
        # Preserve any keys not managed by this form (pins, billboard, etc.)
        existing: dict = dict(storage.get("system_settings", {}))
        existing.update(
            {
                "wifi": {
                    "ssid": ssid,
                    "password": password,
                    "blink_on_connect": True,
                    "print_on_connect": True,
                },
                "hostname": raw_hostname,
                "neopixels": strips,
                "audio_players": audio_players,
            }
        )
        storage["system_settings"] = existing
        storage.store()
        print("SystemSettings: storage saved OK")
        stored_neo = storage.get("system_settings", {}).get("neopixels", {})
        print("SystemSettings: verified neopixels in storage:", stored_neo)

        context = _system_settings_context()
        context["message"] = "Settings saved."
        return render_template("setup/system_settings.html", context)


class SystemRebootView(View):
    """Trigger a delayed device reboot so the HTTP response can be sent first."""

    def post(self) -> str:
        """Queue a reboot and return a status message fragment."""

        import _thread
        from time import sleep

        def _delayed_reset() -> None:
            """Wait briefly, then reboot the MCU."""

            sleep(0.75)
            machine.reset()

        _thread.start_new_thread(_delayed_reset, ())
        return (
            '<div class="alert alert-warning small py-2 mb-0">'
            "Rebooting now. Reconnect to the device in a few seconds."
            "</div>"
        )


class SystemRebootConfirmView(View):
    """Return a confirmation fragment for rebooting the system."""

    def post(self) -> str:
        return render_template("setup/system_reboot_confirm.html", {})


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


def _filters_list(filters_dict: dict, filter_order: list = None) -> list:
    """Build filter summary dicts for template rendering.

    If ``filter_order`` is provided, listed names are emitted first (in that
    order) followed by any remaining filters sorted alphabetically.
    """

    result: list = []
    seen: set = set()

    if filter_order:
        for filter_name in filter_order:
            if filter_name in filters_dict and filter_name not in seen:
                filter_def = filters_dict[filter_name]
                result.append(
                    {
                        "name": filter_name,
                        "filter_type": filter_def.get("filter", ""),
                    }
                )
                seen.add(filter_name)

    for filter_name in sorted(filters_dict.keys()):
        if filter_name in seen:
            continue

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

    # Build available named filters list with selection state.
    # These checkbox controls are used in scene editing only. Effect editing
    # has a dedicated ordered filter manager and should not emit checkbox fields.
    if show_target:
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
                    "after": entry.get("after", ""),
                    "inherit_target": entry.get("inherit_target", False),
                },
            )
        )

    # Build list of entry names that can be chained after (all entries except the one being edited).
    all_entry_names: list = sorted(scene_data.keys())
    chainable_entries: list = [n for n in all_entry_names if n != edit_entry_name]

    # Scene-level settings (kills list).
    scene_meta: dict = lights.settings.get("scene_settings", {}).get(scene_name, {})
    kills: list = scene_meta.get("kills", [])
    all_scene_names: list = sorted(lights.settings.get("scenes", {}).keys())
    killable_scenes: list = [n for n in all_scene_names if n != scene_name]

    context: dict = {
        "scene_name": scene_name,
        "scene_name_id": _scene_name_id(scene_name),
        "scene_entries": scene_entries,
        "available_effects": sorted(effects_dict.keys()),
        "named_ranges": lights.settings.get("named_ranges", {}),
        "named_range_names": sorted(lights.settings.get("named_ranges", {}).keys()),
        "chainable_entries": chainable_entries,
        "killable_scenes": killable_scenes,
        "scene_kills": kills,
        "scene_kills_csv": ",".join(kills),
        "scene_sound": scene_meta.get("sound", ""),
        "page_title": "Edit Scene",
    }

    # Add sounds context
    context.update(_sounds_context())

    if edit_entry_name:
        entry_dict: dict = scene_data.get(edit_entry_name, {})
        context["edit_entry_name"] = edit_entry_name
        context["edit_entry_effect"] = entry_dict.get("effect", "")
        context["edit_entry_target"] = str(entry_dict.get("target", ""))
        context["edit_entry_cycles"] = str(entry_dict["cycles"]) if "cycles" in entry_dict else ""
        context["edit_entry_after"] = entry_dict.get("after", "")
        context["edit_entry_inherit_target"] = entry_dict.get("inherit_target", False)
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
    filters_dict: dict = lights.settings.get("filters", {})
    context: dict = {
        "effects": _effects_list(effects_dict),
        "pattern_metadata": lights.get_pattern_metadata(),
        "all_filters": _filters_list(filters_dict),
        "page_title": "Effects",
    }

    if effect_name and effect_name in effects_dict:
        effect_dict: dict = effects_dict[effect_name]
        pattern: str = effect_dict.get("pattern", "")
        context["edit_effect_name"] = effect_name
        context["edit_effect_pattern"] = pattern
        context["edit_effect_cycles"] = str(effect_dict["cycles"]) if "cycles" in effect_dict else ""
        context["old_effect_name"] = effect_name

        # Keep storage/execution order in effect_filter_names and expose a
        # reversed list for UI display so the top-most filter executes last.
        effect_filters: list = effect_dict.get("filters", [])
        effect_filters_display: list = list(reversed(effect_filters))
        context["effect_filters"] = [
            {
                "name": fname,
                "filter_type": filters_dict.get(fname, {}).get("filter", ""),
            }
            for fname in effect_filters_display
            if fname in filters_dict
        ]
        context["effect_filter_names"] = effect_filters

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

        # Handle effect filter manager actions.
        if action in ["add_effect_filter", "remove_effect_filter", "move_effect_filter_up", "move_effect_filter_down"]:
            if not effect_name or effect_name not in lights.settings.get("effects", {}):
                return '<p class="text-danger small">Effect not found.</p>'

            effect_dict: dict = lights.settings["effects"][effect_name]
            if "filters" not in effect_dict:
                effect_dict["filters"] = []

            filter_name: str = self.request.form_data.get("filter_name", "").strip()

            if action == "add_effect_filter" and filter_name:
                if filter_name not in effect_dict["filters"]:
                    effect_dict["filters"].append(filter_name)
                    lights.settings_object.store()

            elif action == "remove_effect_filter" and filter_name:
                if filter_name in effect_dict["filters"]:
                    effect_dict["filters"].remove(filter_name)
                    lights.settings_object.store()

            elif action == "move_effect_filter_up" and filter_name:
                if filter_name in effect_dict["filters"]:
                    idx = effect_dict["filters"].index(filter_name)
                    if idx > 0:
                        effect_dict["filters"][idx - 1], effect_dict["filters"][idx] = (
                            effect_dict["filters"][idx],
                            effect_dict["filters"][idx - 1],
                        )
                        lights.settings_object.store()

            elif action == "move_effect_filter_down" and filter_name:
                if filter_name in effect_dict["filters"]:
                    idx = effect_dict["filters"].index(filter_name)
                    if idx < len(effect_dict["filters"]) - 1:
                        effect_dict["filters"][idx], effect_dict["filters"][idx + 1] = (
                            effect_dict["filters"][idx + 1],
                            effect_dict["filters"][idx],
                        )
                        lights.settings_object.store()

            # Return just the manager fragment for htmx replacement.
            return render_template("setup/effect_filters_manager.html", _effect_edit_context(effect_name))

        if action == "update_effect" and effect_name and pattern:
            old_effect_name: str = self.request.form_data.get("old_effect_name", "").strip()

            # Build effect dict without target (target lives in scenes)
            effect_dict: dict = _parse_effect_from_form(self.request.form_data, pattern)
            effect_dict.pop("target", None)

            # Preserve ordered managed filters from the existing effect. Filter
            # order is edited via the dedicated manager, not the pattern params form.
            source_name: str = old_effect_name or effect_name
            existing_effect: dict = lights.settings.get("effects", {}).get(source_name, {})
            if "filters" in existing_effect:
                effect_dict["filters"] = list(existing_effect.get("filters", []))

            # If renaming, delete the old entry first
            if old_effect_name and old_effect_name != effect_name and old_effect_name in lights.settings["effects"]:
                del lights.settings["effects"][old_effect_name]
                _rename_effect_refs(old_effect_name, effect_name)

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
    filter_order: list = lights.settings.get("filter_order", [])
    filter_metadata: dict = lights.get_filter_metadata()
    context: dict = {
        "filters": _filters_list(filters_dict, filter_order),
        "filter_metadata": filter_metadata,
        "page_title": "Filters",
    }

    if filter_name and filter_name in filters_dict:
        filter_def: dict = filters_dict[filter_name]
        filter_type: str = filter_def.get("filter", "")
        context["edit_filter_name"] = filter_name
        context["edit_filter_type"] = filter_type
        context["old_filter_name"] = filter_name

        if filter_type in ("sizzle", "scintillate"):
            variation_percent: float | None = None

            if "variation_percent" in filter_def:
                try:
                    variation_percent = float(filter_def.get("variation_percent", 20.0))
                except (TypeError, ValueError):
                    variation_percent = 20.0
            elif "variation" in filter_def:
                try:
                    legacy_variation = float(filter_def.get("variation", 0))
                except (TypeError, ValueError):
                    legacy_variation = 0.0
                variation_percent = (legacy_variation / 255.0) * 100.0

            if variation_percent is None:
                variation_percent = 20.0

            variation_percent = max(0.0, min(100.0, variation_percent))
            context["filter_val_variation_percent"] = str(round(variation_percent, 2))

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
        """Create or delete filters."""

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
            # Remove from any effects that reference this filter
            _rename_filter_refs(filter_name, None)
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

            if filter_type in ("sizzle", "scintillate"):
                variation_percent_value: str = self.request.form_data.get("filter_param_variation_percent", "").strip()
                if variation_percent_value:
                    try:
                        variation_percent = float(variation_percent_value)
                    except ValueError:
                        variation_percent = 20.0
                else:
                    variation_percent = 20.0

                variation_percent = max(0.0, min(100.0, variation_percent))
                filter_def["variation_percent"] = round(variation_percent, 2)
                filter_def.pop("variation", None)

            if old_filter_name and old_filter_name != filter_name and old_filter_name in lights.settings["filters"]:
                del lights.settings["filters"][old_filter_name]
                _rename_filter_refs(old_filter_name, filter_name)

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

        elif action == "rename_scene" and scene_name:
            new_scene_name: str = self.request.form_data.get("new_scene_name", "").strip()
            if (
                new_scene_name
                and new_scene_name != scene_name
                and scene_name in lights.settings["scenes"]
                and new_scene_name not in lights.settings["scenes"]
            ):
                lights.settings["scenes"][new_scene_name] = lights.settings["scenes"].pop(scene_name)
                if lights.settings.get("default_scene") == scene_name:
                    lights.settings["default_scene"] = new_scene_name
                _rename_scene_refs(scene_name, new_scene_name)
                lights.settings_object.store()
                scene_name = new_scene_name

        elif action == "copy_scene" and scene_name and scene_name in lights.settings["scenes"]:
            new_scene_name = self.request.form_data.get("new_scene_name", "").strip()
            if new_scene_name and new_scene_name not in lights.settings["scenes"]:
                lights.settings["scenes"][new_scene_name] = json.loads(
                    json.dumps(lights.settings["scenes"][scene_name])
                )
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
                    _rename_scene_entry_after_refs(
                        lights.settings["scenes"][scene_name],
                        old_entry_name,
                        entry_name,
                    )
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

                after_value: str = self.request.form_data.get("after", "").strip()
                if after_value:
                    entry_dict["after"] = after_value

                inherit_target_value: str = self.request.form_data.get("inherit_target", "").strip()
                if inherit_target_value == "1":
                    entry_dict["inherit_target"] = True

                lights.settings["scenes"][scene_name][entry_name] = entry_dict
                lights.settings_object.store()

            elif action == "delete_entry" and entry_name:
                if entry_name in lights.settings["scenes"][scene_name]:
                    _clear_scene_entry_after_refs(lights.settings["scenes"][scene_name], entry_name)
                    del lights.settings["scenes"][scene_name][entry_name]
                    lights.settings_object.store()

            elif action == "update_scene_settings":
                kills_raw: str = self.request.form_data.get("kills", "").strip()
                kills_list: list = [
                    s.strip()
                    for s in kills_raw.split(",")
                    if s.strip() and s.strip() in lights.settings.get("scenes", {})
                ]
                scene_sound: str = self.request.form_data.get("scene_sound", "").strip()

                if "scene_settings" not in lights.settings:
                    lights.settings["scene_settings"] = {}
                if scene_name not in lights.settings["scene_settings"]:
                    lights.settings["scene_settings"][scene_name] = {}

                if kills_list:
                    lights.settings["scene_settings"][scene_name]["kills"] = kills_list
                elif "kills" in lights.settings["scene_settings"].get(scene_name, {}):
                    del lights.settings["scene_settings"][scene_name]["kills"]

                if scene_sound:
                    lights.settings["scene_settings"][scene_name]["sound"] = scene_sound
                elif "sound" in lights.settings["scene_settings"].get(scene_name, {}):
                    del lights.settings["scene_settings"][scene_name]["sound"]

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


def _sounds_list(sounds_dict: dict, playing_by_title: dict | None = None) -> list:
    """Build a list of sound summary dicts for template rendering, sorted alphabetically."""

    result: list = []
    active_by_title: dict = playing_by_title or {}
    for sound_title in sorted(sounds_dict.keys()):
        sound = sounds_dict[sound_title]
        module_idx = active_by_title.get(sound_title)
        result.append(
            {
                "title": sound_title,
                "file": sound.get("file", 0),
                "high_quality": bool(sound.get("high_quality", False)),
                "show_on_home": bool(sound.get("show_on_home", True)),
                "is_playing": module_idx is not None,
                "module_idx": module_idx if module_idx is not None else -1,
            }
        )

    return result


def _audio_debug_enabled() -> bool:
    """Return whether audio debug logging is enabled in system settings."""

    try:
        storage = PersistentDict()
        system_settings = storage.get("system_settings", {})
        return bool(system_settings.get("audio_debug_logging", False))
    except Exception:
        return False


def _playing_sounds_by_title() -> dict:
    """Return a mapping of currently playing sound title to module index."""

    result: dict = {}
    debug_enabled: bool = _audio_debug_enabled()

    try:
        from sounds import SoundManager

        manager: SoundManager = SoundManager()
        playing_state: dict = manager.get_playing_sounds()
        for module_idx, title in playing_state.items():
            if title:
                result[str(title)] = int(module_idx)
        if debug_enabled:
            print(f"audio-debug: sounds_status playing_state={playing_state} mapped={result}")
    except Exception as error:
        print(f"sounds-status: state mapping error: {error}")
        if debug_enabled:
            print(f"audio-debug: sounds_status failed error={error}")
        return {}

    return result


def _sounds_context(include_playing: bool = False, home_only: bool = False) -> dict:
    """Build template context from sounds in persistent storage."""
    storage: PersistentDict = PersistentDict()
    # Prefer per-model sounds when available
    lighting_root = storage.get("lighting_settings", {})
    sounds: dict = {}
    if isinstance(lighting_root, dict) and "models" in lighting_root:
        current = lighting_root.get("current_model")
        models = lighting_root.get("models", {})
        if current and current in models and isinstance(models[current], dict):
            sounds = models[current].get("sounds", {})
    if not sounds:
        sounds = storage.get("sounds", {})

    system_settings: dict = storage.get("system_settings", {})
    current_volume: int = int(system_settings.get("master_volume", 20))

    if home_only:
        sounds = {title: sound for title, sound in sounds.items() if bool(sound.get("show_on_home", True))}

    playing_by_title: dict = _playing_sounds_by_title() if include_playing else {}
    sounds_list: list = _sounds_list(sounds, playing_by_title=playing_by_title)
    any_sound_playing: bool = any(bool(sound.get("is_playing", False)) for sound in sounds_list)
    poll_interval_seconds: int = 5 if any_sound_playing else 30

    context: dict = {
        "sounds": sounds_list,
        "current_volume": current_volume,
        "sounds_poll_interval_seconds": poll_interval_seconds,
    }

    return context


class SoundsSummaryView(View):
    """Return a summary snippet of sounds for the setup card."""

    def get(self) -> str:
        """Return sounds summary HTML fragment."""

        return render_template("setup/sounds_summary.html", _sounds_context())


class SoundsView(View):
    """Display and edit sound titles, files, and metadata."""

    def get(self) -> str:
        """Return the sounds edit form fragment."""

        return render_template("setup/sounds.html", _sounds_context())

    def post(self) -> str:
        """Validate and save all sound definitions.

        Accepts arrays of sound_title[], sound_file[], sound_high_quality[],
        and sound_show_on_home[].
        """

        fd = self.request.form_data

        def _as_list(val, default) -> list:
            if val is None:
                return [default]
            return val if isinstance(val, list) else [val]

        titles = _as_list(fd.get("sound_title"), "")
        files = _as_list(fd.get("sound_file"), "1")
        hq_flags = _as_list(fd.get("sound_high_quality"), "")
        show_on_home_flags = _as_list(fd.get("sound_show_on_home"), "")

        sounds: dict = {}
        error: str = ""

        for i in range(
            max(
                len(titles),
                len(files),
                len(hq_flags),
                len(show_on_home_flags),
            )
        ):
            title = (titles[i] if i < len(titles) else "").strip()
            if not title:
                continue

            try:
                file_num: int = int(files[i] if i < len(files) else 1)
                if file_num < 1 or file_num > 9999:
                    file_num = 1
            except (ValueError, IndexError):
                file_num = 1

            hq: bool = (hq_flags[i] if i < len(hq_flags) else "") == "1"
            show_on_home: bool = (show_on_home_flags[i] if i < len(show_on_home_flags) else "") == "1"

            sounds[title] = {
                "file": file_num,
                "high_quality": hq,
                "show_on_home": show_on_home,
            }

        if error:
            context = _sounds_context()
            context["error"] = error
            return render_template("setup/sounds.html", context)

        print("Sounds: saving {} sound(s)".format(len(sounds)))
        for title, sound in sounds.items():
            print("  {}: {}".format(title, sound))

        storage: PersistentDict = PersistentDict()
        # Save into the current model if models container exists, otherwise top-level
        lighting_root = storage.get("lighting_settings", {})
        if isinstance(lighting_root, dict) and "models" in lighting_root:
            current = lighting_root.get("current_model")
            if current and current in lighting_root.get("models", {}):
                lighting_root["models"][current]["sounds"] = sounds
                storage["lighting_settings"] = lighting_root
                storage.store()
            else:
                storage["sounds"] = sounds
                storage.store()
        else:
            storage["sounds"] = sounds
            storage.store()
        print("Sounds: storage saved OK")

        context = _sounds_context()
        context["message"] = "Sounds saved."
        return render_template("setup/sounds.html", context)


class PlaySoundView(View):
    """Handle requests to play a sound by title."""

    def post(self) -> str:
        """Play a sound and return a stop button on success.

        Form data:
            title: Sound title to play
        """

        title: str = self.request.form_data.get("title", "").strip()

        if not title:
            return '<div class="alert alert-danger small py-2 mb-0">No sound title provided</div>'

        try:
            from sounds import SoundManager

            manager: SoundManager = SoundManager()
            sound_info: dict | None = None
            try:
                sound_info = manager.get_sound_by_title(title)
            except Exception:
                pass

            module_idx: int = manager.play_sound(title)
            file_number: int = sound_info.get("file", 0) if isinstance(sound_info, dict) else 0
            return (
                f'<form hx-post="/sounds/stop"'
                f' hx-target="#sound-{file_number}-result"'
                f' hx-swap="innerHTML">'
                f'<input type="hidden" name="module_idx" value="{module_idx}">'
                f'<input type="hidden" name="title" value="{title}">'
                f'<input type="hidden" name="file" value="{file_number}">'
                f'<button type="submit" class="btn btn-sm btn-danger theme-sound-stop-btn">'
                f"&#9632; {title}"
                f"</button>"
                f"</form>"
            )
        except ImportError:
            return '<div class="alert alert-warning small py-2 mb-0">Audio system not available</div>'
        except ValueError:
            return '<div class="alert alert-danger small py-2 mb-0">' f"Sound '{title}' not found" "</div>"
        except Exception as err:
            from audio import NoPlayersAvailable

            if isinstance(err, NoPlayersAvailable):
                return '<div class="alert alert-danger small py-2 mb-0">' "All audio modules are busy" "</div>"
            else:
                return '<div class="alert alert-danger small py-2 mb-0">' f"Error: {str(err)}" "</div>"


class StopSoundView(View):
    """Handle requests to stop a specific audio module."""

    def post(self) -> str:
        """Stop the given module and restore the play button.

        Form data:
            module_idx: Index of the audio module to stop
            title: Sound title (to restore the play button label)
            file: File number (used to restore the correct hx-target)
        """

        module_idx_str: str = self.request.form_data.get("module_idx", "")
        title: str = self.request.form_data.get("title", "").strip()
        file_str: str = self.request.form_data.get("file", "0")

        try:
            from audio import AudioPlayer

            audio_player: AudioPlayer = AudioPlayer()
            module_idx: int = int(module_idx_str)
            if 0 <= module_idx < len(audio_player.players):
                audio_player.players[module_idx].stop()
        except Exception as err:
            print(f"StopSoundView: error stopping module: {err}")

        try:
            file_number: int = int(file_str)
        except (ValueError, TypeError):
            file_number = 0

        return (
            f'<form hx-post="/sounds/play"'
            f' hx-target="#sound-{file_number}-result"'
            f' hx-swap="innerHTML">'
            f'<input type="hidden" name="title" value="{title}">'
            f'<button type="submit" class="btn btn-sm btn-outline-primary theme-sound-btn">'
            f"{title}"
            f"</button>"
            f"</form>"
        )


class StopAllSoundsView(View):
    """Handle requests to stop all audio modules."""

    def post(self) -> str:
        """Stop all audio modules and return the re-rendered sounds buttons section."""

        try:
            from audio import AudioPlayer

            audio_player: AudioPlayer = AudioPlayer()
            audio_player.stop_all()
        except Exception as err:
            print(f"StopAllSoundsView: error stopping all: {err}")

        return render_template("sounds/buttons.html", _sounds_context(include_playing=True, home_only=True))


class SoundsStatusView(View):
    """Return the home sounds controls fragment with current playback state."""

    def get(self) -> str:
        """Render sound buttons using current playing/not-playing state."""

        debug_enabled: bool = _audio_debug_enabled()
        print(f"sounds-status: endpoint hit debug_enabled={debug_enabled}")
        if debug_enabled:
            print("audio-debug: sounds_status endpoint hit")

        context: dict = _sounds_context(include_playing=True, home_only=True)
        if debug_enabled:
            try:
                sound_count: int = len(context.get("sounds", []))
            except Exception:
                sound_count = -1
            print(f"audio-debug: sounds_status render sound_count={sound_count}")

        return render_template("sounds/buttons.html", context)
