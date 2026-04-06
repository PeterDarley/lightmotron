import settings
from lighting import Lighting

lights = Lighting()
lights.add_colors({"test color": (123, 45, 67)})

lights.animation.start()

print("running")
