"""Global template context processors for the web layer.

Usage
-----
Import this module and call ``register(func)`` to add a callback::

    from web.context_processors import register

    def my_processor() -> dict:
        return {"current_user": "admin"}

    register(my_processor)

Each registered callable is called with no arguments on every
``render_template`` invocation.  Their return values are merged in
registration order, then the caller-supplied context is overlaid on top
(so view-level values always win).

The built-in theme processor is registered automatically when this module
is imported.  It adds ``theme_css`` (e.g. ``"themes/dark_red.css"`` or
``""`` if no theme is set) to every template context.
"""

from storage import PersistentDict

_callbacks: list = []


def register(func) -> None:
    """Register a context-processor callback.

    The callback must accept no arguments and return a dict.  It is called
    once per ``render_template`` invocation and its keys are merged into the
    template context.
    """

    _callbacks.append(func)


def get_context() -> dict:
    """Call all registered processors and merge their results.

    Returns a single dict suitable for passing to ``render_template``.
    """

    merged: dict = {}
    for callback in _callbacks:
        try:
            result = callback()
            if isinstance(result, dict):
                merged.update(result)
        except Exception:
            pass

    return merged


# ---------------------------------------------------------------------------
# Built-in: theme processor
# ---------------------------------------------------------------------------


def _theme_processor() -> dict:
    """Return the active theme CSS path for injection into every page.

    Reads ``ui_settings.theme`` from persistent storage.  Returns an empty
    string when no theme is configured, so templates can use
    ``{% if theme_css %}`` safely.
    """

    storage = PersistentDict()
    theme_filename: str = storage.get("ui_settings", {}).get("theme", "")
    return {"theme_css": "themes/" + theme_filename if theme_filename else ""}


register(_theme_processor)
