"""Views for listing and editing lighting filters."""

from webserver import View, render_template
from web.views_common import lights, _color_name_to_hex, _filters_list, _rename_filter_refs


def _filter_edit_context(filter_name: str = None) -> dict:
    """Build template context for the filter editor."""

    filters_dict: dict = lights.settings.get("filters", {})
    filter_order: list = lights.settings.get("filter_order", [])
    filter_metadata: dict = lights.get_filter_metadata()
    custom_colors: dict = lights.settings.get("custom_colors", {})
    context: dict = {
        "filters": _filters_list(filters_dict, filter_order),
        "filter_metadata": filter_metadata,
        "custom_colors": custom_colors,
        "page_title": "Filters",
    }

    if filter_name and filter_name in filters_dict:
        filter_def: dict = filters_dict[filter_name]
        filter_type: str = filter_def.get("filter", "")
        context["edit_filter_name"] = filter_name
        context["edit_filter_type"] = filter_type
        context["old_filter_name"] = filter_name

        if filter_type in ("sizzle", "scintillate"):
            variation_percent = None

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

        # Resolve the "color" param into dropdown/swatch context, mirroring
        # the pattern-effect color selector (see _pattern_params_context).
        if "color" in filter_def:
            existing_color = filter_def["color"]
            if isinstance(existing_color, (list, tuple)) and len(existing_color) == 3:
                context["filter_color_hex"] = "#{:02X}{:02X}{:02X}".format(
                    int(existing_color[0]), int(existing_color[1]), int(existing_color[2])
                )
                context["filter_color_selected"] = "__picker__"
                context["filter_color_display_name"] = ""
                context["filter_color_is_named"] = False
                context["filter_color_is_picker"] = True
            elif isinstance(existing_color, str) and existing_color.startswith("#"):
                context["filter_color_hex"] = existing_color
                context["filter_color_selected"] = "__picker__"
                context["filter_color_display_name"] = ""
                context["filter_color_is_named"] = False
                context["filter_color_is_picker"] = True
            else:
                existing_color = str(existing_color)
                if existing_color.startswith("custom:"):
                    raw_name: str = existing_color[7:]
                else:
                    raw_name = existing_color
                context["filter_color_selected"] = raw_name
                context["filter_color_display_name"] = raw_name
                context["filter_color_hex"] = _color_name_to_hex(raw_name, custom_colors)
                context["filter_color_is_named"] = True
                context["filter_color_is_picker"] = False
        else:
            context["filter_color_hex"] = "#FF0000"
            context["filter_color_selected"] = ""
            context["filter_color_display_name"] = ""
            context["filter_color_is_named"] = False
            context["filter_color_is_picker"] = False

    return context


def _filters_list_response() -> str:
    """Render the filters list fragment (used after create/delete actions)."""

    return render_template(
        "setup/filters.html",
        {
            "filters": _filters_list(lights.settings.get("filters", {})),
            "page_title": "Filters",
        },
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


class FiltersView(View):
    """List filters and create or delete filters."""

    def get(self) -> str:
        """Show filter list and create-filter form."""

        return _filters_list_response()

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

        return _filters_list_response()


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
                raw_value = self.request.form_data.get("filter_param_" + param_name, "")
                # The color select and its paired color-picker swatch share a
                # form field name, so submissions may arrive as a list; the
                # picker's value (submitted last) reflects the actual choice.
                param_value: str = (raw_value[-1] if raw_value else "").strip() if isinstance(raw_value, list) else raw_value.strip()
                if param_value:
                    if param_name == "color" and (
                        param_value.startswith("#")
                        and len(param_value) == 7
                        and all(c in "0123456789ABCDEFabcdef" for c in param_value[1:])
                    ):
                        hex_color = param_value.lstrip("#")
                        filter_def[param_name] = [
                            int(hex_color[0:2], 16),
                            int(hex_color[2:4], 16),
                            int(hex_color[4:6], 16),
                        ]
                    else:
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

            return _filters_list_response()

        return _filters_list_response()
