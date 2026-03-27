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
        "/named_range/toggle": views.NamedRangeToggleView,
        "/named_range/clear": views.NamedRangeClearView,
    }
)

# Animation runs freely without webserver synchronization
