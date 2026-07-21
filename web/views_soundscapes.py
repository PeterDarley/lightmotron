"""Views for managing and playing soundscapes."""

from webserver import View, render_template
from storage import PersistentDict
from web.views_common import (
    _soundscapes_context,
    _sounds_context,
    _get_model_scoped_dict,
    _save_model_scoped_dict,
    _is_enabled_setting,
    _parse_non_negative_int,
)


class SoundscapesSummaryView(View):
    """Return a summary snippet of soundscapes for the setup card."""

    def get(self) -> str:
        """Return soundscapes summary HTML fragment."""

        return render_template("setup/soundscapes_summary.html", _soundscapes_context())


class SoundscapesView(View):
    """List soundscapes and create or delete soundscapes."""

    def get(self) -> str:
        """Show soundscape list and create-soundscape form."""

        context: dict = _soundscapes_context()
        context["page_title"] = "Soundscapes"
        context.update(_sounds_context())
        return render_template("setup/soundscapes.html", context)

    def post(self) -> str:
        """Create or delete a soundscape."""

        action: str = self.request.form_data.get("action", "").strip()
        soundscape_name: str = self.request.form_data.get("soundscape_name", "").strip()

        storage: PersistentDict = PersistentDict()
        soundscapes: dict = self._get_soundscapes_dict(storage)

        if action == "create" and soundscape_name:
            if soundscape_name not in soundscapes:
                soundscapes[soundscape_name] = {}
                self._save_soundscapes_dict(storage, soundscapes)

        elif action == "delete" and soundscape_name and soundscape_name in soundscapes:
            del soundscapes[soundscape_name]
            self._save_soundscapes_dict(storage, soundscapes)

        # Return updated manager fragment
        context: dict = _soundscapes_context()
        return render_template("setup/soundscapes.html", context)

    def _get_soundscapes_dict(self, storage: PersistentDict) -> dict:
        """Get the soundscapes dictionary from storage."""

        return _get_model_scoped_dict(storage, "soundscapes") or {}

    def _save_soundscapes_dict(self, storage: PersistentDict, soundscapes: dict) -> None:
        """Save the soundscapes dictionary to storage."""

        _save_model_scoped_dict(storage, "soundscapes", soundscapes)


class PlaySoundscapeView(View):
    """Handle requests to play a soundscape."""

    def post(self) -> str:
        """Start a soundscape and return updated soundscape button area."""

        soundscape_name: str = self.request.form_data.get("soundscape", "").strip()

        try:
            from sounds import SoundManager

            manager: SoundManager = SoundManager()
            if not manager.play_soundscape(soundscape_name):
                print(f"PlaySoundscapeView: failed to play soundscape '{soundscape_name}'")
        except Exception as err:
            print(f"PlaySoundscapeView: error playing soundscape: {err}")

        return render_template("soundscapes/buttons.html", _soundscapes_context(include_active=True))


class StopSoundscapeView(View):
    """Handle requests to stop the currently playing soundscape."""

    def post(self) -> str:
        """Stop current soundscape and return updated soundscape button area."""

        try:
            from sounds import SoundManager

            manager: SoundManager = SoundManager()
            manager.stop_all()
        except Exception as err:
            print(f"StopSoundscapeView: error stopping soundscape: {err}")

        return render_template("soundscapes/buttons.html", _soundscapes_context(include_active=True))


class SoundscapesStatusView(View):
    """Return the home soundscapes controls fragment with current playback state."""

    def get(self) -> str:
        """Render soundscape buttons using current active state."""

        return render_template("soundscapes/buttons.html", _soundscapes_context(include_active=True))


class SoundscapeEditView(View):
    """Handle editing a soundscape's entries."""

    def get(self) -> str:
        """Show soundscape entry editor."""

        soundscape_name: str = self.request.query_params.get("soundscape", "").strip()
        edit_entry_name: str = self.request.query_params.get("edit_entry", "").strip()

        from sounds import SoundManager

        manager: SoundManager = SoundManager()
        soundscape: dict = manager.get_soundscape(soundscape_name)
        sounds: dict = manager.get_sounds()

        context: dict = self._build_soundscape_edit_context(soundscape_name, soundscape, sounds, edit_entry_name)
        return render_template("setup/soundscape_edit.html", context)

    def post(self) -> str:
        """Create or update soundscape entries."""

        soundscape_name: str = self.request.form_data.get("soundscape_name", "").strip()
        action: str = self.request.form_data.get("action", "").strip()

        from sounds import SoundManager

        manager: SoundManager = SoundManager()
        storage: PersistentDict = PersistentDict()

        if action == "add_entry":
            entry_sound: str = self.request.form_data.get("entry_sound", "").strip()
            entry_repeat: str = self.request.form_data.get("entry_repeat", "0").strip()
            entry_repeat_enabled: bool = _is_enabled_setting(self.request.form_data.get("entry_repeat_enabled", ""))

            repeat_count: int = _parse_non_negative_int(entry_repeat)

            if entry_sound and soundscape_name:
                soundscape: dict = manager.get_soundscape(soundscape_name)
                # Generate a unique entry name
                entry_num = len(soundscape) + 1
                entry_name = f"entry{entry_num}"
                soundscape[entry_name] = {
                    "sound": entry_sound,
                    "repeat_enabled": entry_repeat_enabled,
                    "repeat": repeat_count,
                }
                self._save_soundscape(storage, soundscape_name, soundscape)

        elif action == "delete_entry":
            entry_name: str = self.request.form_data.get("entry_name", "").strip()
            if entry_name and soundscape_name:
                soundscape: dict = manager.get_soundscape(soundscape_name)
                if entry_name in soundscape:
                    del soundscape[entry_name]
                    self._save_soundscape(storage, soundscape_name, soundscape)

        elif action == "update_entry":
            entry_name: str = self.request.form_data.get("entry_name", "").strip()
            entry_sound: str = self.request.form_data.get("entry_sound", "").strip()
            entry_repeat: str = self.request.form_data.get("entry_repeat", "0").strip()
            entry_repeat_enabled: bool = _is_enabled_setting(self.request.form_data.get("entry_repeat_enabled", ""))

            repeat_count: int = _parse_non_negative_int(entry_repeat)

            if entry_name and entry_sound and soundscape_name:
                soundscape: dict = manager.get_soundscape(soundscape_name)
                if entry_name in soundscape and isinstance(soundscape.get(entry_name), dict):
                    soundscape[entry_name]["sound"] = entry_sound
                    soundscape[entry_name]["repeat_enabled"] = entry_repeat_enabled
                    soundscape[entry_name]["repeat"] = repeat_count
                    self._save_soundscape(storage, soundscape_name, soundscape)

            # Saving an entry returns to the manager list, effectively closing
            # the per-soundscape editor and refreshing the soundscapes table.
            return render_template("setup/soundscapes.html", _soundscapes_context())

        # Return updated edit view
        soundscape: dict = manager.get_soundscape(soundscape_name)
        sounds: dict = manager.get_sounds()
        context: dict = self._build_soundscape_edit_context(soundscape_name, soundscape, sounds)
        return render_template("setup/soundscape_edit.html", context)

    def _build_soundscape_edit_context(
        self,
        soundscape_name: str,
        soundscape: dict,
        sounds: dict,
        edit_entry_name: str = "",
    ) -> dict:
        """Build context for soundscape edit modal, optionally selecting one entry for edit mode."""

        context: dict = {
            "soundscape_name": soundscape_name,
            "soundscape": soundscape,
            "sounds": sorted(sounds.keys()),
            "entries": [],
            "edit_entry_name": "",
            "edit_entry": {},
        }

        for entry_name, entry_data in soundscape.items():
            if not isinstance(entry_data, dict):
                continue

            repeat_raw = entry_data.get("repeat", 0)
            repeat_count: int = _parse_non_negative_int(repeat_raw)
            if "repeat_enabled" in entry_data:
                repeat_enabled: bool = _is_enabled_setting(entry_data.get("repeat_enabled", False))
            else:
                # Backward compatibility for entries created before repeat_enabled existed.
                repeat_enabled = repeat_count > 0

            if not repeat_enabled:
                repeat_mode_label: str = "off"
            elif repeat_count == 0:
                repeat_mode_label = "infinite"
            else:
                repeat_mode_label = str(repeat_count)

            entry_context: dict = {
                "name": entry_name,
                "sound": entry_data.get("sound", ""),
                "repeat": repeat_count,
                "repeat_enabled": repeat_enabled,
                "repeat_mode_label": repeat_mode_label,
                "after": entry_data.get("after", ""),
            }
            context["entries"].append(entry_context)

            if edit_entry_name and entry_name == edit_entry_name:
                context["edit_entry_name"] = edit_entry_name
                context["edit_entry"] = entry_context

        return context

    def _save_soundscape(self, storage: PersistentDict, soundscape_name: str, soundscape: dict) -> None:
        """Save a soundscape to storage."""

        lighting_root = storage.get("lighting_settings", {})
        if isinstance(lighting_root, dict) and "models" in lighting_root:
            current = lighting_root.get("current_model")
            if current and current in lighting_root.get("models", {}):
                soundscapes = lighting_root["models"][current].get("soundscapes", {})
                soundscapes[soundscape_name] = soundscape
                lighting_root["models"][current]["soundscapes"] = soundscapes
                storage["lighting_settings"] = lighting_root
                storage.store()
