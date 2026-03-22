from webserver import View, render_template
from billboard import Billboard
from storage import PersistentDict


billboard = Billboard.from_settings(debug=True)

class TestView(View):

    def get(self):
        """Handle GET requests for the /test route."""

        storage = PersistentDict('storage.json')

        if "test_count" in storage:
            storage['test_count'] += 1

        else:
            print("Initializing test_count")
            storage['test_count'] = 1

        storage.store()

        billboard.scroll_text("Cindy Rocks!", delay_ms=60)

        return render_template("template_test.html", {"message": f"test count: {storage['test_count']}"})
    
    def post(self):
        """Handle POST requests for the /test route."""
        
        message = "Post"

        if "text_input" in self.request.form_data:
            message = self.request.form_data["text_input"]


        billboard.scroll_text(message, delay_ms=60)
