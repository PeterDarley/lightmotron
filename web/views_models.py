"""Views for managing lighting models (create, rename, delete, activate)."""

from webserver import View, render_template, Response
from web.views_common import lights


def _models_context() -> dict:
    """Return a context dict describing available models and the current model."""

    return {"models": lights.get_model_names(), "current_model": lights.current_model_name}


class ModelsSummaryView(View):
    """Return a small summary snippet for the Models card in Setup."""

    def get(self) -> str:
        ctx = _models_context()
        return render_template(
            "setup/models_summary.html", {"current_model": ctx["current_model"], "model_count": len(ctx["models"])}
        )


class ModelsView(View):
    """Full modal view for managing models."""

    def get(self) -> str:
        ctx = _models_context()
        return render_template(
            "setup/models.html",
            {"models": ctx["models"], "current_model": ctx["current_model"], "page_title": "Models"},
        )

    def post(self) -> str:
        action = self.request.form_data.get("action", "set").strip()
        name = self.request.form_data.get("name", "").strip()
        new_name = self.request.form_data.get("new_name", "").strip()

        if action == "set" and name:
            try:
                lights.set_current_model(name)
                # Instruct HTMX to reload the setup page so summaries update
                return Response(status=200, reason="OK", body="", headers={"HX-Redirect": "/setup"})
            except Exception as e:
                return str(e), 400
        elif action == "create" and name:
            try:
                lights.create_model(name)
            except Exception as e:
                return str(e), 400
        elif action == "delete" and name:
            try:
                lights.delete_model(name)
            except Exception as e:
                return str(e), 400
        elif action == "rename" and name and new_name:
            try:
                lights.rename_model(name, new_name)
            except Exception as e:
                return str(e), 400

        # Return updated modal
        return self.get()


class ModelsSetView(View):
    """Set the active model from the setup page model selector."""

    def post(self) -> str:
        name = self.request.form_data.get("model", "").strip()
        if name:
            try:
                lights.set_current_model(name)
            except Exception as e:
                return str(e), 400

        # Ask the client to reload the setup page so the UI reflects the new active model.
        return Response(status=200, reason="OK", body="", headers={"HX-Redirect": "/setup"})


class ModelsWrapView(View):
    """Wrap legacy top-level lighting_settings into a model called 'Model'."""

    def post(self) -> str:
        try:
            lights.wrap_current_settings_into_model("Model")
            return self.get()
        except Exception as e:
            return str(e), 400
