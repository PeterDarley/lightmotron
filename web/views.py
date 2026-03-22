from webserver import View, render_template
from billboard import Billboard
from storage import PersistentDict
from lighting import Lighting

billboard = Billboard.from_settings(debug=True)

storage = PersistentDict()
lights = Lighting()


class HomeView(View):

    def get(self):
        """Handle GET requests for the /test route."""

        return render_template("home.html", {"message": lights.seting})
