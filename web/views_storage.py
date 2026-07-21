"""Views for viewing, backing up, and restoring persistent storage."""

from webserver import View, render_template, Response
from storage import PersistentDict
import io
import json
import sys

from web.views_common import lights


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

    def get(self):
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
