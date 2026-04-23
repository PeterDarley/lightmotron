# boot.py -- run on boot-up

import machine  # type: ignore
import micropython  # type: ignore

import settings

# set up the exception buffer so we can see what happens if we crash
micropython.alloc_emergency_exception_buf(100)

from comms import WIFIManager, I2CManager  # type: ignore
from webserver import WebServer  # type: ignore
import network  # type: ignore

try:
    import esp

    esp.osdebug(None)
except Exception:
    pass

print("\nBooting...")

from storage import PersistentDict  # type: ignore

# Seed all default settings into persistent storage for any missing keys.
# After this call every system_settings key exists in storage.json.
settings.seed_defaults()

# Set mDNS hostname so the device is reachable at <hostname>.local
_stored_hostname = PersistentDict().get("system_settings", {}).get("hostname", "")
network.hostname(_stored_hostname if _stored_hostname else "lightmotron")

# Determine stored WiFi credentials
_wifi_ssid = PersistentDict().get("system_settings", {}).get("wifi", {}).get("ssid", "")
_wifi_password = PersistentDict().get("system_settings", {}).get("wifi", {}).get("password", "")

if not _wifi_ssid or not _wifi_password:
    # No credentials configured — start captive portal to collect them
    print("No WiFi credentials configured, starting captive portal...")
    from captive_portal import CaptivePortal  # type: ignore

    CaptivePortal().start()  # blocks and resets device when done

# Attempt to connect to WiFi
WIFIManager(block=True, timeout=20)

if not WIFIManager().is_connected:
    # Connection failed — start captive portal so user can correct credentials
    print("WiFi connection failed, starting captive portal...")
    from captive_portal import CaptivePortal  # type: ignore

    CaptivePortal().start()  # blocks and resets device when done

_active_hostname = network.hostname()
_active_ip = WIFIManager().ip
print("Hostname:", _active_hostname)
print("Home URL (mDNS): http://" + _active_hostname + ".local/")
print("Home URL (IP):   http://" + _active_ip + "/")

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
