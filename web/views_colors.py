"""Views for managing custom colors and the shared color-select fragment."""

from webserver import View, render_template
from web.views_common import lights, _color_name_to_hex, _rename_color_refs


def _custom_colors_response(
    edit_name: str = "", edit_hex: str = "", copy_name: str = "", copy_hex: str = "#FF0000"
) -> str:
    """Build and render the custom colors template with current settings.

    Pass edit_name and edit_hex to pre-fill the form for editing an existing
    color. Pass copy_name and copy_hex to pre-fill the "add" form for
    duplicating an existing color under a new name.
    """

    custom_colors_list = sorted(
        [
            (name, tuple(value) if isinstance(value, (list, tuple)) else value)
            for name, value in lights.settings.get("custom_colors", {}).items()
        ],
        key=lambda item: item[0],
    )
    return render_template(
        "setup/custom_colors.html",
        {
            "custom_colors": custom_colors_list,
            "page_title": "Custom Colors",
            "edit_name": edit_name,
            "edit_hex": edit_hex,
            "copy_name": copy_name,
            "copy_hex": copy_hex,
        },
    )


class CustomColorsSummaryView(View):
    """Return a summary snippet of custom colors for the setup card."""

    def get(self) -> str:
        """Return custom colors summary HTML fragment."""

        custom_colors_list = sorted(
            [
                (name, tuple(value) if isinstance(value, (list, tuple)) else value)
                for name, value in lights.settings.get("custom_colors", {}).items()
            ],
            key=lambda item: item[0],
        )
        return render_template(
            "setup/custom_colors_summary.html",
            {"custom_colors": custom_colors_list},
        )


class CustomColorsView(View):
    """Manage custom colors - create new or delete existing colors."""

    def get(self) -> str:
        """Show custom colors form and list of current colors."""

        return _custom_colors_response()

    def post(self) -> str:
        """Add or delete a custom color."""

        action = self.request.form_data.get("action", "add").strip()
        color_name = self.request.form_data.get("color_name", "").strip()
        color_value = self.request.form_data.get("color_value", "").strip()

        if "custom_colors" not in lights.settings:
            lights.settings["custom_colors"] = {}

        if action == "delete" and color_name and color_name in lights.settings["custom_colors"]:
            del lights.settings["custom_colors"][color_name]
        elif action in ("add", "update") and color_name and color_value:
            try:
                hex_color = color_value.lstrip("#")
                r = int(hex_color[0:2], 16)
                g = int(hex_color[2:4], 16)
                b = int(hex_color[4:6], 16)
                old_color_name: str = self.request.form_data.get("old_color_name", "").strip()
                if action == "update" and old_color_name and old_color_name != color_name:
                    if old_color_name in lights.settings["custom_colors"]:
                        del lights.settings["custom_colors"][old_color_name]
                    _rename_color_refs(old_color_name, color_name)
                lights.settings["custom_colors"][color_name] = (r, g, b)
            except (ValueError, IndexError):
                pass
        elif action == "edit_form" and color_name and color_name in lights.settings["custom_colors"]:
            rgb = lights.settings["custom_colors"][color_name]
            edit_hex = "#{:02X}{:02X}{:02X}".format(int(rgb[0]), int(rgb[1]), int(rgb[2]))
            return _custom_colors_response(edit_name=color_name, edit_hex=edit_hex)
        elif action == "copy_form" and color_name and color_name in lights.settings["custom_colors"]:
            rgb = lights.settings["custom_colors"][color_name]
            copy_hex = "#{:02X}{:02X}{:02X}".format(int(rgb[0]), int(rgb[1]), int(rgb[2]))
            return _custom_colors_response(copy_name="{}_copy".format(color_name), copy_hex=copy_hex)
        elif action == "cancel":
            return _custom_colors_response()

        lights.settings_object.store()
        return _custom_colors_response()


class ColorSelectView(View):
    """Return an HTML fragment for the color display area when the color dropdown changes."""

    def post(self) -> str:
        """Return a pill, color picker, or empty based on the selected color."""

        field_name: str = self.request.form_data.get("field_name", "").strip()
        color_index: str = self.request.form_data.get("color_index", "0").strip()
        if not field_name:
            field_name = f"param_color_{color_index}"
        color_value: str = self.request.form_data.get(field_name, "").strip()
        custom_colors: dict = lights.settings.get("custom_colors", {})

        is_named: bool = bool(color_value and color_value != "__picker__" and not color_value.startswith("#"))
        is_picker: bool = color_value == "__picker__" or color_value.startswith("#")

        # Determine stored color name and hex for display
        if is_named:
            is_custom: bool = color_value.startswith("custom:")
            if is_custom:
                raw_name: str = color_value[7:]
                stored_name: str = color_value
                display_name: str = raw_name
            else:
                raw_name = color_value
                stored_name = color_value
                display_name = color_value
            hex_val: str = _color_name_to_hex(raw_name, custom_colors)
        else:
            stored_name = ""
            display_name = ""
            hex_val = color_value if color_value.startswith("#") else "#FF0000"

        context: dict = {
            "color_index": color_index,
            "field_name": field_name,
            "color_name": stored_name,
            "color_display_name": display_name,
            "color_hex_val": hex_val,
            "is_named": is_named,
            "is_picker": is_picker,
        }

        return render_template("setup/color_select.html", context)
