from webserver import View, render_template
from billboard import Billboard
from storage import PersistentDict
from lighting import Lighting
import json

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

        return render_template("setup.html", {})
