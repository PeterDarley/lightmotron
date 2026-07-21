"""Views for listing and editing effects (standalone pattern definitions)."""

from webserver import View, render_template
from web.views_common import lights, _color_name_to_hex, _filters_list, _rename_effect_refs


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


def _effects_list_response() -> str:
    """Render the effects list fragment (used after create/delete/rename actions)."""

    return render_template(
        "setup/effects.html",
        {
            "effects": _effects_list(lights.settings.get("effects", {})),
            "page_title": "Effects",
        },
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


class EffectsView(View):
    """List effects and create or delete effects."""

    def get(self) -> str:
        """Show effect list and create-effect form."""

        return _effects_list_response()

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

        return _effects_list_response()


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

            return _effects_list_response()

        return _effects_list_response()
