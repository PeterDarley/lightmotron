"""Views for listing scenes and editing scene entries, settings, and trigger scenes."""

from webserver import View, render_template
import json

from web.views_common import (
    lights,
    _scenes_list,
    _scene_name_id,
    _sounds_context,
    _rename_scene_refs,
    _rename_scene_entry_after_refs,
    _clear_scene_entry_after_refs,
)


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

    # Scene-level settings (kills list, trigger scenes on completion).
    scene_meta: dict = lights.settings.get("scene_settings", {}).get(scene_name, {})
    kills: list = scene_meta.get("kills", [])
    all_scene_names: list = sorted(lights.settings.get("scenes", {}).keys())
    killable_scenes: list = [n for n in all_scene_names if n != scene_name]
    trigger_scenes_on_completion: list = scene_meta.get("trigger_scenes_on_completion", [])
    triggerable_scenes: list = [n for n in all_scene_names if n != scene_name]
    stop_sounds_on_start: list = scene_meta.get("stop_sounds_on_start", [])
    stop_sounds_on_end: list = scene_meta.get("stop_sounds_on_end", [])

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
        "trigger_scenes_on_completion": trigger_scenes_on_completion,
        "triggerable_scenes": triggerable_scenes,
        "scene_sound": scene_meta.get("sound", ""),
        "scene_stop_sounds_on_start": stop_sounds_on_start,
        "scene_stop_sounds_on_start_csv": ",".join(stop_sounds_on_start),
        "scene_stop_sounds_on_end": stop_sounds_on_end,
        "scene_stop_sounds_on_end_csv": ",".join(stop_sounds_on_end),
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


class ScenesSummaryView(View):
    """Return a summary snippet of scenes for the setup card."""

    def get(self) -> str:
        """Return scenes summary HTML fragment."""

        return render_template(
            "setup/scenes_summary.html",
            {"scenes": _scenes_list(lights.settings.get("scenes", {}))},
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

                trigger_scenes_raw: str = self.request.form_data.get("trigger_scenes_on_completion", "").strip()
                trigger_scenes_list: list = [
                    s.strip()
                    for s in trigger_scenes_raw.split(",")
                    if s.strip() and s.strip() in lights.settings.get("scenes", {})
                ]

                scene_sound: str = self.request.form_data.get("scene_sound", "").strip()
                from sounds import SoundManager

                sound_titles: set = set(SoundManager().get_sounds().keys())

                stop_sounds_on_start_raw: str = self.request.form_data.get("stop_sounds_on_start", "").strip()
                stop_sounds_on_start_list: list = [
                    s.strip() for s in stop_sounds_on_start_raw.split(",") if s.strip() and s.strip() in sound_titles
                ]

                stop_sounds_on_end_raw: str = self.request.form_data.get("stop_sounds_on_end", "").strip()
                stop_sounds_on_end_list: list = [
                    s.strip() for s in stop_sounds_on_end_raw.split(",") if s.strip() and s.strip() in sound_titles
                ]

                if "scene_settings" not in lights.settings:
                    lights.settings["scene_settings"] = {}
                if scene_name not in lights.settings["scene_settings"]:
                    lights.settings["scene_settings"][scene_name] = {}

                if kills_list:
                    lights.settings["scene_settings"][scene_name]["kills"] = kills_list
                elif "kills" in lights.settings["scene_settings"].get(scene_name, {}):
                    del lights.settings["scene_settings"][scene_name]["kills"]

                if trigger_scenes_list:
                    lights.settings["scene_settings"][scene_name]["trigger_scenes_on_completion"] = trigger_scenes_list
                elif "trigger_scenes_on_completion" in lights.settings["scene_settings"].get(scene_name, {}):
                    del lights.settings["scene_settings"][scene_name]["trigger_scenes_on_completion"]

                if scene_sound:
                    lights.settings["scene_settings"][scene_name]["sound"] = scene_sound
                elif "sound" in lights.settings["scene_settings"].get(scene_name, {}):
                    del lights.settings["scene_settings"][scene_name]["sound"]

                if stop_sounds_on_start_list:
                    lights.settings["scene_settings"][scene_name]["stop_sounds_on_start"] = stop_sounds_on_start_list
                elif "stop_sounds_on_start" in lights.settings["scene_settings"].get(scene_name, {}):
                    del lights.settings["scene_settings"][scene_name]["stop_sounds_on_start"]

                if stop_sounds_on_end_list:
                    lights.settings["scene_settings"][scene_name]["stop_sounds_on_end"] = stop_sounds_on_end_list
                elif "stop_sounds_on_end" in lights.settings["scene_settings"].get(scene_name, {}):
                    del lights.settings["scene_settings"][scene_name]["stop_sounds_on_end"]

                lights.settings_object.store()

        return render_template("setup/scene_edit.html", _scene_edit_context(scene_name))


class SceneEditAddTriggerSceneView(View):
    """Handle HTMX request to add a trigger scene to the list."""

    def post(self) -> str:
        """Add a scene to the trigger_scenes_on_completion list and return updated control."""

        scene_name: str = self.request.form_data.get("scene_name", "").strip()
        trigger_scene_to_add: str = self.request.form_data.get("trigger_scene_to_add", "").strip()

        # Validate both scenes exist
        all_scenes = lights.settings.get("scenes", {})
        if not scene_name or scene_name not in all_scenes:
            return '<div class="alert alert-danger small">Scene not found.</div>'

        if not trigger_scene_to_add or trigger_scene_to_add not in all_scenes:
            return '<div class="alert alert-danger small">Invalid trigger scene.</div>'

        # Initialize scene_settings if needed
        if "scene_settings" not in lights.settings:
            lights.settings["scene_settings"] = {}
        if scene_name not in lights.settings["scene_settings"]:
            lights.settings["scene_settings"][scene_name] = {}

        # Get current trigger scenes list
        trigger_scenes: list = list(
            lights.settings["scene_settings"][scene_name].get("trigger_scenes_on_completion", [])
        )

        # Add scene if not already present
        if trigger_scene_to_add not in trigger_scenes:
            trigger_scenes.append(trigger_scene_to_add)
            lights.settings["scene_settings"][scene_name]["trigger_scenes_on_completion"] = trigger_scenes
            lights.settings_object.store()

        # Build the updated control fragment context
        context = {
            "scene_name": scene_name,
            "scene_name_id": _scene_name_id(scene_name),
            "trigger_scenes_on_completion": trigger_scenes,
            "triggerable_scenes": [n for n in sorted(all_scenes.keys()) if n != scene_name],
            "trigger_scene_to_add": "",  # Reset dropdown
        }

        return render_template("setup/scene_edit_trigger_control.html", context)


class SceneEditRemoveTriggerSceneView(View):
    """Handle HTMX request to remove a trigger scene from the list."""

    def post(self) -> str:
        """Remove a scene from the trigger_scenes_on_completion list and return updated control."""

        scene_name: str = self.request.form_data.get("scene_name", "").strip()
        trigger_scene_to_remove: str = self.request.form_data.get("trigger_scene_to_remove", "").strip()

        # Validate scene exists
        all_scenes = lights.settings.get("scenes", {})
        if not scene_name or scene_name not in all_scenes:
            return '<div class="alert alert-danger small">Scene not found.</div>'

        # Initialize scene_settings if needed
        if "scene_settings" not in lights.settings:
            lights.settings["scene_settings"] = {}
        if scene_name not in lights.settings["scene_settings"]:
            lights.settings["scene_settings"][scene_name] = {}

        # Get current trigger scenes list
        trigger_scenes: list = list(
            lights.settings["scene_settings"][scene_name].get("trigger_scenes_on_completion", [])
        )

        # Remove scene if present
        if trigger_scene_to_remove in trigger_scenes:
            trigger_scenes.remove(trigger_scene_to_remove)
            if trigger_scenes:
                lights.settings["scene_settings"][scene_name]["trigger_scenes_on_completion"] = trigger_scenes
            elif "trigger_scenes_on_completion" in lights.settings["scene_settings"][scene_name]:
                del lights.settings["scene_settings"][scene_name]["trigger_scenes_on_completion"]
            lights.settings_object.store()

        # Build the updated control fragment context
        context = {
            "scene_name": scene_name,
            "scene_name_id": _scene_name_id(scene_name),
            "trigger_scenes_on_completion": trigger_scenes,
            "triggerable_scenes": [n for n in sorted(all_scenes.keys()) if n != scene_name],
            "trigger_scene_to_add": "",  # Reset dropdown
        }

        return render_template("setup/scene_edit_trigger_control.html", context)
