from webserver import WebServer
import web.views as views

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
    }
)


# Animation runs freely without webserver synchronization
