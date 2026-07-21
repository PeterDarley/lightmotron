"""Views for the home page, scene activation, and animation control."""

from webserver import View, render_template
from web.views_common import lights, _soundscapes_context


def _animation_context() -> dict:
    """Return a context dict with the current animation running state."""

    animation_is_running = bool(lights.animation.running) and not bool(lights.animation.stopped)
    return {"animation_running": animation_is_running, "animation_stopped": not animation_is_running}


def _scenes_context() -> dict:
    """Return a context dict with the scenes list and active scenes."""

    scene_names = sorted(lights.settings["scenes"].keys())
    ongoing_scenes = [name for name in scene_names if lights.is_scene_ongoing(name)]
    immediate_scenes = [name for name in scene_names if not lights.is_scene_ongoing(name)]
    # Show all currently active scenes (ongoing and immediate) in the Home
    # running/active list so one-shot immediate scenes are visible while active.
    active_scenes = [name for name in lights._active_scenes if name in scene_names]

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


class HomeView(View):
    """Display the home page with scene, animation, and soundscape controls."""

    def get(self) -> str:
        """Handle GET requests for the home route."""

        context = {"message": "Lighting", "page_title": "Home"}
        context.update(_scenes_context())
        context.update(_animation_context())
        context.update(_soundscapes_context(include_active=True))

        return render_template("home.html", context)


class SetSceneView(View):
    """Handle POST requests to set or modify the current lighting scene(s)."""

    def post(self) -> str:
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


class ScenePanelStatusView(View):
    """Handle GET requests to return updated scene panel status."""

    def get(self) -> str:
        """Return the scene panel HTML with current active scenes."""
        return render_template("scenes/scene_panel.html", _scenes_context())


class AnimationView(View):
    """Handle POST requests to start or stop the lighting animation."""

    def post(self) -> str:
        """Start or stop the animation based on the POST data."""
        action = self.request.form_data.get("action")
        if action == "start":
            lights.animation.start()
        elif action == "stop":
            lights.animation.stop()
        else:
            return "Invalid action", 400

        return render_template("animation/buttons.html", _animation_context())
