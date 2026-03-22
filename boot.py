# boot.py -- run on boot-up

import machine  # type: ignore
import micropython  # type: ignore

import settings

# set up the exception buffer so we can see what happens if we crash
micropython.alloc_emergency_exception_buf(100)

from comms import WIFIManager, I2CManager  # type: ignore
from webserver import WebServer  # type: ignore

try:
    import esp

    esp.osdebug(None)
except Exception:
    pass

print("\nBooting...")

# Set the CPU frequency
if hasattr(settings, "BOARD") and "CPU_Frequency" in settings.BOARD:
    machine.freq(settings.BOARD["CPU_Frequency"])


# Start the WIFI and begin the web server after the connection is established
# WIFIManager(callback=web_server.start_in_thread)
WIFIManager(block=True)

# Create the web server
web_server = WebServer()
web_server.start_in_thread()

# Import views to register routes
# try:
#     import views
#     print("views imported")
# except Exception as e:
#     print('boot: failed to import views:', e)

import web.routes

# Start the I2C
# I2CManager()

print("Boot complete.\n")
