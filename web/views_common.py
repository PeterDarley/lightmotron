"""Shared state and helpers used by two or more web view feature modules."""

from storage import PersistentDict
from lighting import Lighting
import settings


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


try:
    from network import WLAN, STA_IF
except Exception:
    pass

lights = Lighting()


def _iter_scene_entries():
    """Yield every entry dict across every scene (the shared shape behind several ``_rename_*_refs`` helpers)."""

    for scene in lights.settings.get("scenes", {}).values():
        for entry in scene.values():
            yield entry


def _replace_in_effect_lists(list_key: str, old_value: str, new_value: str) -> None:
    """Replace ``old_value`` with ``new_value`` (exact match) in every effect's ``list_key`` list."""

    for effect in lights.settings.get("effects", {}).values():
        items: list = effect.get(list_key)
        if isinstance(items, list):
            for i, item in enumerate(items):
                if item == old_value:
                    items[i] = new_value


def _rename_named_range_refs(old_name: str, new_name: str) -> None:
    """Update all target fields that reference a named range by its old name."""

    old_ref: str = "named:" + old_name
    new_ref: str = "named:" + new_name
    for entry in _iter_scene_entries():
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

    for entry in _iter_scene_entries():
        if entry.get("effect") == old_name:
            entry["effect"] = new_name


def _rename_filter_refs(old_name: str, new_name: str) -> None:
    """Update all effects whose filter lists reference a filter by its old name."""

    _replace_in_effect_lists("filters", old_name, new_name)


def _rename_scene_refs(old_name: str, new_name: str) -> None:
    """Update scene_settings key and kills and trigger_scenes_on_completion lists that reference a scene by its old name."""

    scene_settings: dict = lights.settings.get("scene_settings", {})
    if old_name in scene_settings:
        scene_settings[new_name] = scene_settings.pop(old_name)

    for meta in scene_settings.values():
        kills: list = meta.get("kills")
        if isinstance(kills, list):
            for i, item in enumerate(kills):
                if item == old_name:
                    kills[i] = new_name

        trigger_scenes: list = meta.get("trigger_scenes_on_completion")
        if isinstance(trigger_scenes, list):
            for i, item in enumerate(trigger_scenes):
                if item == old_name:
                    trigger_scenes[i] = new_name


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

    _replace_in_effect_lists("colors", "custom:" + old_name, "custom:" + new_name)


def _is_enabled_setting(value: object) -> bool:
    """Return True when a setting value represents an enabled/checked state."""

    if isinstance(value, bool):
        return value

    if isinstance(value, int):
        return value != 0

    if isinstance(value, str):
        normalized: str = value.strip().lower()
        return normalized in ("1", "true", "yes", "on")

    return bool(value)


def _parse_non_negative_int(value: object) -> int:
    """Parse value as a non-negative integer, tolerating numeric strings/floats."""

    try:
        if isinstance(value, bool):
            return int(value)

        if isinstance(value, int):
            return max(0, value)

        if isinstance(value, float):
            return max(0, int(value))

        if isinstance(value, str):
            normalized: str = value.strip()
            if not normalized:
                return 0

            if "." in normalized:
                return max(0, int(float(normalized)))

            return max(0, int(normalized))

        return max(0, int(value))
    except (TypeError, ValueError):
        return 0


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


def _sounds_list(sounds_dict: dict, playing_by_title: dict = None) -> list:
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


def _get_model_scoped_dict(storage: PersistentDict, key: str) -> dict | None:
    """Get the current model's ``key`` sub-dict (e.g. "sounds", "soundscapes"), creating it if absent.

    Returns None if there is no current-model context (legacy storage format), so
    callers can decide their own fallback behavior.
    """

    lighting_root = storage.get("lighting_settings", {})
    if isinstance(lighting_root, dict) and "models" in lighting_root:
        current = lighting_root.get("current_model")
        if current and current in lighting_root.get("models", {}):
            if key not in lighting_root["models"][current]:
                lighting_root["models"][current][key] = {}
            return lighting_root["models"][current][key]

    return None


def _save_model_scoped_dict(storage: PersistentDict, key: str, value: dict) -> bool:
    """Save ``value`` as the current model's ``key`` sub-dict. Returns False if there's no current-model context."""

    lighting_root = storage.get("lighting_settings", {})
    if isinstance(lighting_root, dict) and "models" in lighting_root:
        current = lighting_root.get("current_model")
        if current and current in lighting_root.get("models", {}):
            lighting_root["models"][current][key] = value
            storage["lighting_settings"] = lighting_root
            storage.store()
            return True

    return False


def _soundscapes_context(include_active: bool = False) -> dict:
    """Build template context from soundscapes in persistent storage."""

    from sounds import SoundManager

    manager: SoundManager = SoundManager()
    soundscapes: dict = manager.get_soundscapes()

    storage: PersistentDict = PersistentDict()
    system_settings: dict = storage.get("system_settings", {})
    try:
        current_volume: int = int(system_settings.get("master_volume", 20))
    except (TypeError, ValueError):
        current_volume = 20
    current_volume = max(0, min(30, current_volume))

    soundscapes_list: list = sorted(soundscapes.keys())
    active_soundscape = manager.get_active_soundscape() if include_active else None
    soundscape_rows: list = []
    for soundscape_name in soundscapes_list:
        raw_entries = soundscapes.get(soundscape_name, {})
        if not isinstance(raw_entries, dict):
            raw_entries = {}

        entry_names: list = [
            entry_name for entry_name, entry_data in raw_entries.items() if isinstance(entry_data, dict)
        ]
        entry_count: int = len(entry_names)

        infinite_repeat_count: int = 0
        finite_repeat_count: int = 0
        distinct_sounds: dict = {}
        for entry_name in entry_names:
            entry_data = raw_entries.get(entry_name, {})
            sound_title = str(entry_data.get("sound", "")).strip()
            if sound_title:
                distinct_sounds[sound_title] = True

            repeat_raw = entry_data.get("repeat", 0)
            repeat_count: int = _parse_non_negative_int(repeat_raw)
            if "repeat_enabled" in entry_data:
                repeat_enabled: bool = _is_enabled_setting(entry_data.get("repeat_enabled", False))
            else:
                repeat_enabled = repeat_count > 0

            if repeat_enabled:
                if repeat_count == 0:
                    infinite_repeat_count += 1
                else:
                    finite_repeat_count += 1

        sound_names: list = sorted(distinct_sounds.keys())
        preview_names: list = sound_names[:2]
        extra_sound_count: int = max(0, len(sound_names) - len(preview_names))
        preview_suffix: str = f" +{extra_sound_count}" if extra_sound_count > 0 else ""
        sounds_preview: str = ", ".join(preview_names) + preview_suffix if preview_names else "-"

        soundscape_rows.append(
            {
                "name": soundscape_name,
                "entry_count": entry_count,
                "infinite_repeat_count": infinite_repeat_count,
                "finite_repeat_count": finite_repeat_count,
                "sounds_preview": sounds_preview,
            }
        )

    soundscape_items: list = []
    for soundscape_name in soundscapes_list:
        soundscape_items.append(
            {
                "name": soundscape_name,
                "id": _scene_name_id(soundscape_name),
            }
        )

    context: dict = {
        "soundscapes": soundscapes_list,
        "soundscape_rows": soundscape_rows,
        "soundscape_count": len(soundscapes_list),
        "soundscape_plural_suffix": "" if len(soundscapes_list) == 1 else "s",
        "soundscape_items": soundscape_items,
        "active_soundscape": active_soundscape,
        "current_volume": current_volume,
    }

    return context


def _sounds_context(include_playing: bool = False, home_only: bool = False) -> dict:
    """Build template context from sounds in persistent storage."""

    from sounds import SoundManager

    manager: SoundManager = SoundManager()
    sounds: dict = manager.get_sounds()

    storage: PersistentDict = PersistentDict()
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
