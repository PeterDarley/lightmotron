"""Views for system settings, theme, hostname, status, reboot, OTA updates, confirm, and setup."""

from webserver import View, render_template, Response
from storage import PersistentDict
from comms import WIFIManager
from ota_update import OTAUpdater
from utils import reset_cause_name
import gc
import json
import os
import sys
import machine

from web import views_named_ranges
from web.views_common import lights, _scenes_list, _soundscapes_context, _sounds_context


def _as_list(val, default) -> list:
    """Normalise a form value that may be a single item or list into a list."""

    if val is None:
        return [default]
    return val if isinstance(val, list) else [val]


def _fmt_bytes(num_bytes: int) -> str:
    """Format a byte count as a human-readable string (KB or MB)."""

    if num_bytes >= 1024 * 1024:
        return "{:.1f} MB".format(num_bytes / (1024 * 1024))
    elif num_bytes >= 1024:
        return "{:.1f} KB".format(num_bytes / 1024)
    else:
        return "{} B".format(num_bytes)


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
        context.update(_soundscapes_context())
        context.update(_sounds_context())

        return render_template("setup.html", context)


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

        storage: PersistentDict = PersistentDict()
        system_settings: dict = storage.get("system_settings", {})

        if WIFIManager().is_connected:
            wifi_connected = "Yes"
            ip_address = WIFIManager().ip
        else:
            wifi_connected = "No"
            ip_address = "N/A"

        hostname: str = str(system_settings.get("hostname", "") or "lightmotron")
        wifi_ssid: str = str(system_settings.get("wifi", {}).get("ssid", ""))

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
        # Show configured audio modules from system settings and overlay
        # live health data when available.
        configured_audio_players: list = system_settings.get("audio_players", [])
        if not isinstance(configured_audio_players, list):
            configured_audio_players = []

        audio_health_list: list = []
        audio_health_summary: str = "No audio modules configured"
        raw_health: dict = {}
        try:
            from sounds import SoundManager

            sm = SoundManager()
            raw_health = sm.refresh_health() or {}
        except Exception:
            raw_health = {}

        for module_index, module_cfg in enumerate(configured_audio_players):
            info: dict = raw_health.get(module_index, {}) if isinstance(raw_health, dict) else {}
            uart_id = module_cfg.get("uart") if isinstance(module_cfg, dict) else None
            if uart_id is None:
                uart_id = info.get("uart")

            audio_health_list.append(
                {
                    "index": module_index,
                    "uart": uart_id,
                    "ok": bool(info.get("ok", False)),
                    "state": info.get("state"),
                }
            )

        if audio_health_list:
            if raw_health:
                healthy = sum(1 for it in audio_health_list if it.get("ok"))
                audio_health_summary = f"{healthy}/{len(audio_health_list)} responsive"
            else:
                audio_health_summary = f"Configured modules: {len(audio_health_list)} (health unavailable)"

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
                "reset_cause": reset_cause_name(),
                "wifi_connected": wifi_connected,
                "ip_address": ip_address,
                "wifi_ssid": wifi_ssid,
                "hostname": hostname,
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

            try:
                apply_result = updater.apply_updates(
                    _ota_last_check.get("branch", "main"),
                    _ota_last_check.get("updates", []),
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
                    _schedule_reboot()
                    message_parts.append("Rebooting now to apply changes.")

                return render_template(
                    "setup/updates.html",
                    _updates_context(
                        message=" ".join(message_parts),
                        check_result=check_result,
                        apply_result=apply_result,
                    ),
                )
            except Exception as error:
                return render_template(
                    "setup/updates.html",
                    _updates_context(
                        error="Update apply failed: {}".format(str(error)),
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
        submitted_ssid: str = fd.get("wifi_ssid", "").strip()
        submitted_password: str = fd.get("wifi_password", "").strip()
        # Preserve the existing SSID/password when a field is left blank -
        # otherwise saving unrelated settings (e.g. just the hostname) with
        # WiFi fields left empty would silently wipe stored credentials.
        existing_wifi: dict = PersistentDict().get("system_settings", {}).get("wifi", {})
        ssid: str = submitted_ssid if submitted_ssid else existing_wifi.get("ssid", "")
        password: str = submitted_password if submitted_password else existing_wifi.get("password", "")

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


class SystemSettingsIPAnnouncedView(View):
    """Mark the current IP address as announced in persistent storage."""

    def post(self) -> str:
        """Store the current IP so it won't be announced again.

        Returns:
            Empty response (HTMX swap='none')
        """

        try:
            from ip_announcement import set_ip_announced  # type: ignore
            from comms import WIFIManager  # type: ignore

            wifi_mgr: object = WIFIManager()
            current_ip: str = str(getattr(wifi_mgr, "ip", "")).strip()

            if current_ip and current_ip != "N/A":
                set_ip_announced(current_ip)
                print(f"SystemSettingsIPAnnounced: marked IP {current_ip} as announced")
                return ""
            else:
                print("SystemSettingsIPAnnounced: no valid IP address available")
                return ""

        except Exception as err:
            print(f"SystemSettingsIPAnnounced: error: {err}")
            import sys

            sys.print_exception(err)
            return ""


def _schedule_reboot() -> None:
    """Queue a delayed device reboot so the current HTTP response can be sent first."""

    import _thread
    from time import sleep

    def _delayed_reset() -> None:
        """Wait briefly, then reboot the MCU."""

        sleep(0.75)
        machine.reset()

    _thread.start_new_thread(_delayed_reset, ())


class SystemRebootView(View):
    """Trigger a delayed device reboot so the HTTP response can be sent first."""

    def post(self) -> str:
        """Queue a reboot and return a status message fragment."""

        _schedule_reboot()
        return (
            '<div class="alert alert-warning small py-2 mb-0">'
            "Rebooting now. Reconnect to the device in a few seconds."
            "</div>"
        )


class SystemRebootConfirmView(View):
    """Return a confirmation fragment for rebooting the system."""

    def post(self) -> str:
        """Return the reboot confirmation fragment."""

        return render_template("setup/system_reboot_confirm.html", {})
