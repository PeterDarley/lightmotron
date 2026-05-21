from webserver import View, render_template, Response
import json
import os

_lights = None
_rename_named_range_refs_func = None
_summarize_led_list_func = None

# Server-side selection state for the LED naming tool
_selected_leds = []
_editing_range_name = None


def initialize_named_range_views(lights_instance, rename_named_range_refs_func, summarize_led_list_func) -> None:
    """Inject shared dependencies used by named range views."""

    global _lights, _rename_named_range_refs_func, _summarize_led_list_func
    _lights = lights_instance
    _rename_named_range_refs_func = rename_named_range_refs_func
    _summarize_led_list_func = summarize_led_list_func


def _parse_selected_tokens(selected_leds_str: str) -> list:
    """Parse selected_leds string into a list of ints and named refs."""

    if not selected_leds_str:
        return []

    tokens = []
    for token in (x.strip() for x in selected_leds_str.split(",")):
        if not token:
            continue
        if token.startswith("named:"):
            name = token[6:]
            if name:
                tokens.append("named:" + name)
        else:
            try:
                tokens.append(int(token))
            except ValueError:
                pass

    return tokens


def _named_ranges_has_cycle(named_ranges: dict) -> bool:
    """Detect cycles in the named_ranges graph."""

    adj = {name: [] for name in named_ranges.keys()}
    for name, members in named_ranges.items():
        if isinstance(members, list):
            for item in members:
                if isinstance(item, str) and item.startswith("named:"):
                    adj.setdefault(name, []).append(item[6:])

    visited = set()
    recstack = set()

    def _dfs(node: str) -> bool:
        if node in recstack:
            return True
        if node in visited:
            return False

        visited.add(node)
        recstack.add(node)
        for child in adj.get(node, []):
            if _dfs(child):
                return True

        recstack.remove(node)
        return False

    for node in adj.keys():
        if _dfs(node):
            return True

    return False


def summarize_named_range_members(members: list) -> str:
    """Summarize named-range members as subranges plus direct LEDs.

    Subrange members are shown by name. Only direct integer LED members are
    summarized numerically, so LEDs that come from included subranges are not
    expanded in this display.
    """

    if not isinstance(members, list):
        return "none"

    direct_leds = []
    subrange_names = []
    seen_subranges = set()

    for item in members:
        if isinstance(item, int):
            direct_leds.append(item)
        elif isinstance(item, str) and item.startswith("named:"):
            subrange_name = item[6:]
            if subrange_name and subrange_name not in seen_subranges:
                seen_subranges.add(subrange_name)
                subrange_names.append(subrange_name)

    summary_parts = []
    if subrange_names:
        summary_parts.append("ranges: {}".format(", ".join(sorted(subrange_names, key=lambda name: name.lower()))))
    if direct_leds:
        summary_parts.append("leds: {}".format(_summarize_led_list_func(direct_leds)))

    if not summary_parts:
        return "none"

    return "; ".join(summary_parts)


def _build_named_range_entries(sort_by: str) -> list:
    """Build named range entries with summaries, sorted by name or LED index."""

    named_ranges_dict = _lights.settings.get("named_ranges", {})
    range_entries = []
    for name, members in named_ranges_dict.items():
        range_resolved = _lights.get_targets(members)
        min_led = min(range_resolved) if range_resolved else 99999
        range_entries.append(
            {
                "name": name,
                "summary": _summarize_led_list_func(range_resolved),
                "min_led": min_led,
            }
        )

    if sort_by == "leds":
        range_entries.sort(key=lambda range_entry: range_entry["min_led"])
    else:
        range_entries.sort(key=lambda range_entry: range_entry["name"].lower())

    return range_entries


def get_named_range_summary_entries(sort_by: str = "name") -> list:
    """Return named range summary entries without internal sort metadata."""

    range_entries = _build_named_range_entries(sort_by)
    for range_entry in range_entries:
        if "min_led" in range_entry:
            del range_entry["min_led"]

    return range_entries


def _named_range_context(sort_by: str = "name") -> dict:
    """Build template context for the LED picker from current server-side selection state."""

    direct_selected_leds = []
    seen_direct_leds = set()
    for item in _selected_leds:
        if isinstance(item, int) and item not in seen_direct_leds:
            seen_direct_leds.add(item)
            direct_selected_leds.append(item)

    resolved = []
    for item in _selected_leds:
        try:
            targets = _lights.get_targets(item)
        except Exception:
            targets = []
        for target in targets:
            if target not in resolved:
                resolved.append(target)

    selected_set = set(direct_selected_leds)
    led_list = [
        {
            "index": led_index,
            "css_class": "btn-warning" if led_index in selected_set else "btn-outline-secondary",
        }
        for led_index in range(_lights.leds.count)
    ]

    named_range_names = _build_named_range_entries(sort_by)

    if sort_by not in ("name", "leds"):
        sort_by = "name"

    selected_subranges = []
    seen_subranges = set()
    for item in _selected_leds:
        if isinstance(item, str) and item.startswith("named:"):
            subrange_name = item[6:]
            if subrange_name and subrange_name not in seen_subranges:
                seen_subranges.add(subrange_name)
                selected_subranges.append(subrange_name)

    try:
        selected_tokens_str = ",".join([str(value) for value in _selected_leds])
    except Exception:
        selected_tokens_str = ""

    return {
        "led_list": led_list,
        "named_range_names": named_range_names,
        "range_name": _editing_range_name,
        "selected_tokens": list(_selected_leds),
        "selected_tokens_str": selected_tokens_str,
        "selected_subranges": selected_subranges,
        "led_summary": _summarize_led_list_func(resolved),
        "sort_by": sort_by,
        "led_picker_script_version": _get_led_picker_script_version(),
    }


def _get_led_picker_script_version() -> str:
    """Return a stable cache-busting version for the LED picker script.

    Uses file mtime so the URL query string changes only when the script file
    actually changes.
    """

    candidate_paths = (
        "/www/scripts/led_picker.js",
        "www/scripts/led_picker.js",
    )

    for candidate_path in candidate_paths:
        try:
            stat = os.stat(candidate_path)
            if len(stat) > 8:
                return str(stat[8])
            return str(stat[6])
        except OSError:
            pass

    return "0"


class NamedRangeSummaryView(View):
    """Return a summary snippet of named ranges for the setup card."""

    def get(self) -> str:
        """Return named ranges summary HTML fragment."""

        ranges_info = get_named_range_summary_entries("name")

        return render_template(
            "setup/named_ranges_summary.html",
            {"named_ranges": ranges_info},
        )


class NamedRangeView(View):
    """Manage LED naming - create new or edit existing ranges."""

    def get(self) -> str:
        """Show LED picker for creating or editing a range."""

        global _selected_leds, _editing_range_name
        range_name = self.request.query_params.get("name")
        sort_by = self.request.query_params.get("sort", "name")
        if sort_by not in ("name", "leds"):
            sort_by = "name"

        if range_name and range_name in _lights.settings.get("named_ranges", {}):
            _editing_range_name = range_name
            _selected_leds = list(_lights.settings["named_ranges"][range_name])
        else:
            _selected_leds = []
            _editing_range_name = None

        if _lights.animation.running:
            _lights.animation.pause()

        _lights.leds.clear()
        if _selected_leds:
            resolved = []
            for item in _selected_leds:
                try:
                    targets = _lights.get_targets(item)
                except Exception:
                    targets = []
                for target in targets:
                    if target not in resolved:
                        resolved.append(target)
            _lights.leds.identify(resolved)
        _lights.leds.show()

        context = _named_range_context(sort_by)
        context["page_title"] = "Named Ranges"
        return render_template("setup/led_picker.html", context)

    def post(self) -> str:
        """Save or delete a named range."""

        global _selected_leds, _editing_range_name
        action = self.request.form_data.get("action", "save").strip()
        old_name = self.request.form_data.get("old_name", "").strip()
        range_name = self.request.form_data.get("range_name", "").strip()
        selected_leds_str = self.request.form_data.get("selected_leds", "").strip()

        selected_tokens = _parse_selected_tokens(selected_leds_str)

        if action == "delete" and old_name and old_name in _lights.settings.get("named_ranges", {}):
            del _lights.settings["named_ranges"][old_name]
        elif range_name:
            candidate = dict(_lights.settings.get("named_ranges", {}))
            candidate[range_name] = selected_tokens
            if _named_ranges_has_cycle(candidate):
                return Response(status=400, reason="Bad Request", body="Circular named-range reference detected")

            _lights.settings["named_ranges"][range_name] = selected_tokens
            if old_name != range_name and old_name in _lights.settings.get("named_ranges", {}):
                del _lights.settings["named_ranges"][old_name]
                _rename_named_range_refs_func(old_name, range_name)

        _lights.settings_object.store()

        _selected_leds = []
        _editing_range_name = None
        _lights.animation.resume()
        _lights.leds.clear()
        _lights.leds.show()

        context = _named_range_context()
        context["page_title"] = "Named Ranges"
        return render_template("setup/led_picker.html", context)


class NamedRangeSetView(View):
    """Set the current LED selection without returning HTML."""

    def post(self) -> None:
        """Update selection, light resolved LEDs, and return debug payload for the editor UI."""

        global _selected_leds
        selected_leds_str = self.request.form_data.get("selected_leds", "").strip()
        tokens = _parse_selected_tokens(selected_leds_str)
        _selected_leds = tokens

        direct_selected_leds = []
        seen_direct_leds = set()
        for item in tokens:
            if isinstance(item, int) and item not in seen_direct_leds:
                seen_direct_leds.add(item)
                direct_selected_leds.append(item)

        resolved = []
        for item in tokens:
            try:
                targets = _lights.get_targets(item)
            except Exception:
                targets = []
            for target in targets:
                if target not in resolved:
                    resolved.append(target)

        if resolved:
            _lights.leds.identify(resolved)
        else:
            _lights.leds.clear()
            _lights.leds.show()

        response_payload = {
            "selected_tokens": [str(item) for item in tokens],
            "direct_selected_leds": direct_selected_leds,
            "resolved_leds": resolved,
        }

        return Response(body=json.dumps(response_payload), content_type="application/json")


class NamedRangeRemoveSubrangeView(View):
    """Remove a named subrange token from the current editing selection."""

    def post(self) -> str:
        """Remove one named: token, update hardware identification, and re-render editor."""

        global _selected_leds

        token = self.request.query_params.get("token", "").strip()
        sort_by = self.request.query_params.get("sort", "name")
        if sort_by not in ("name", "leds"):
            sort_by = "name"

        if token and token.startswith("named:"):
            _selected_leds = [item for item in _selected_leds if not (isinstance(item, str) and item == token)]

        resolved = []
        for item in _selected_leds:
            try:
                targets = _lights.get_targets(item)
            except Exception:
                targets = []
            for target in targets:
                if target not in resolved:
                    resolved.append(target)

        if resolved:
            _lights.leds.identify(resolved)
        else:
            _lights.leds.clear()
            _lights.leds.show()

        context = _named_range_context(sort_by)
        context["page_title"] = "Named Ranges"
        return render_template("setup/led_picker.html", context)
