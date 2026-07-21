"""Views for managing sounds: list/edit sounds, playback control, status, and master volume."""

from webserver import View, render_template
from storage import PersistentDict

from web.views_common import (
    _sounds_context,
    _audio_debug_enabled,
    _get_model_scoped_dict,
    _save_model_scoped_dict,
)


def _get_sounds_dict(storage: PersistentDict) -> dict:
    """Get the sounds dictionary from storage."""

    scoped = _get_model_scoped_dict(storage, "sounds")
    if scoped is not None:
        return scoped

    if "sounds" not in storage:
        storage["sounds"] = {}
    return storage["sounds"]


def _save_sounds_dict(storage: PersistentDict, sounds: dict) -> None:
    """Save the sounds dictionary to storage."""

    if _save_model_scoped_dict(storage, "sounds", sounds):
        return

    storage["sounds"] = sounds
    storage.store()


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


class SoundsSummaryView(View):
    """Return a summary snippet of sounds for the setup card."""

    def get(self) -> str:
        """Return sounds summary HTML fragment."""

        return render_template("setup/sounds_summary.html", _sounds_context())


class SoundsView(View):
    """List sounds and create or delete sounds."""

    def get(self) -> str:
        """Show sound list and create-sound form."""

        context: dict = _sounds_context()
        context["page_title"] = "Sounds"
        return render_template("setup/sounds.html", context)

    def post(self) -> str:
        """Create or delete a sound."""

        action: str = self.request.form_data.get("action", "").strip()
        sound_title: str = self.request.form_data.get("sound_title", "").strip()

        storage: PersistentDict = PersistentDict()
        sounds: dict = _get_sounds_dict(storage)

        if action == "create_sound" and sound_title:
            if sound_title not in sounds:
                sounds[sound_title] = {
                    "file": 1,
                    "high_quality": False,
                    "show_on_home": True,
                    "loop_count": 0,
                    "chain_next": None,
                }
                _save_sounds_dict(storage, sounds)

        elif action == "delete_sound" and sound_title and sound_title in sounds:
            del sounds[sound_title]
            _save_sounds_dict(storage, sounds)

        context: dict = _sounds_context()
        context["page_title"] = "Sounds"
        return render_template("setup/sounds.html", context)


class SoundEditView(View):
    """Edit a specific sound's properties."""

    def get(self) -> str:
        """Show edit form for a specific sound."""

        sound_title: str = self.request.query_params.get("sound", "").strip()

        storage: PersistentDict = PersistentDict()
        sounds: dict = _get_sounds_dict(storage)

        if not sound_title or sound_title not in sounds:
            return '<p class="text-danger small">Sound not found.</p>'

        sound: dict = sounds[sound_title]
        context: dict = {
            "sound_title": sound_title,
            "sound_file": sound.get("file", 1),
            "sound_high_quality": bool(sound.get("high_quality", False)),
            "sound_show_on_home": bool(sound.get("show_on_home", True)),
            "sound_loop_count": sound.get("loop_count", 0),
            "sound_chain_next": sound.get("chain_next"),
            "available_sounds": sorted(sounds.keys()),
        }
        return render_template("setup/sound_edit.html", context)

    def post(self) -> str:
        """Update a sound's properties."""

        action: str = self.request.form_data.get("action", "").strip()
        old_sound_title: str = self.request.form_data.get("old_sound_title", "").strip()
        sound_title: str = self.request.form_data.get("sound_title", "").strip()

        if action == "update_sound" and old_sound_title and sound_title:
            storage: PersistentDict = PersistentDict()
            sounds: dict = _get_sounds_dict(storage)

            if old_sound_title in sounds:
                # Get the current sound data
                sound_data: dict = sounds[old_sound_title]

                # Update with new form values
                try:
                    sound_data["file"] = int(self.request.form_data.get("sound_file", 1))
                except (ValueError, TypeError):
                    sound_data["file"] = 1

                sound_data["high_quality"] = self.request.form_data.get("sound_high_quality", "") == "1"
                sound_data["show_on_home"] = self.request.form_data.get("sound_show_on_home", "") == "1"

                try:
                    sound_data["loop_count"] = int(self.request.form_data.get("sound_loop_count", 0))
                except (ValueError, TypeError):
                    sound_data["loop_count"] = 0

                chain_next: str = self.request.form_data.get("sound_chain_next", "").strip()
                sound_data["chain_next"] = chain_next if chain_next else None

                # If title changed, delete old entry and create new one
                if old_sound_title != sound_title:
                    if sound_title not in sounds:
                        del sounds[old_sound_title]
                        sounds[sound_title] = sound_data
                    else:
                        # New title already exists, just update the old sound
                        sounds[old_sound_title] = sound_data
                else:
                    sounds[sound_title] = sound_data

                _save_sounds_dict(storage, sounds)

        context: dict = _sounds_context()
        context["page_title"] = "Sounds"
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
            sound_info = None
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
            from sounds import SoundManager

            manager: SoundManager = SoundManager()
            module_idx = None
            try:
                parsed_module_idx: int = int(module_idx_str)
                module_idx = parsed_module_idx
            except (ValueError, TypeError):
                module_idx = None

            stop_ok: bool = manager.stop_sound(title, module_idx)
            if not stop_ok:
                print(f"StopSoundView: stop command did not confirm stopped for title='{title}' module={module_idx}")
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
