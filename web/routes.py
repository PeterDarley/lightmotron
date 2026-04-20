from webserver import WebServer, set_context_processor
import web.views as views
import web.context_processors as context_processors

# Wire the global context processor so every render_template call automatically
# receives theme and other shared context values.
set_context_processor(context_processors.get_context)

# Register views with the web server
web_server = WebServer()

web_server.add_routes(
    {
        "/": views.HomeView,
        "/set_scene": views.SetSceneView,
        "/animation": views.AnimationView,
        "/storage": views.StorageView,
        "/setup": views.SetupView,
        "/status": views.StatusView,
        "/named_range": views.NamedRangeView,
        "/named_range/set": views.NamedRangeSetView,
        "/named_range/summary": views.NamedRangeSummaryView,
        "/custom_colors": views.CustomColorsView,
        "/custom_colors/summary": views.CustomColorsSummaryView,
        "/scenes": views.ScenesView,
        "/scenes/edit": views.SceneEditView,
        "/scenes/color_select": views.ColorSelectView,
        "/scenes/summary": views.ScenesSummaryView,
        "/effects": views.EffectsView,
        "/effects/edit": views.EffectEditView,
        "/effects/color_select": views.ColorSelectView,
        "/effects/summary": views.EffectsSummaryView,
        "/filters": views.FiltersView,
        "/filters/edit": views.FilterEditView,
        "/filters/summary": views.FiltersSummaryView,
        "/backup": views.BackupView,
        "/restore": views.RestoreView,
        "/theme": views.ThemeView,
        "/theme/delete": views.ThemeDeleteView,
    }
)


# Animation runs freely without webserver synchronization
