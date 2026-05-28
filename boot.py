# boot.py -- run on boot-up

import machine  # type: ignore
import micropython  # type: ignore
import sys

import settings

# set up the exception buffer so we can see what happens if we crash
micropython.alloc_emergency_exception_buf(100)

from comms import WIFIManager  # type: ignore
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

_storage = PersistentDict()
_system_settings = _storage.get("system_settings", {})

# Set mDNS hostname so the device is reachable at <hostname>.local
_stored_hostname = _system_settings.get("hostname", "")
network.hostname(_stored_hostname if _stored_hostname else "lightmotron")

# Determine stored WiFi credentials
_wifi_ssid = _system_settings.get("wifi", {}).get("ssid", "")
_wifi_password = _system_settings.get("wifi", {}).get("password", "")

if not _wifi_ssid or not _wifi_password:
    # No credentials configured — start captive portal to collect them
    print("No WiFi credentials configured, starting captive portal...")
    from captive_portal import CaptivePortal  # type: ignore

    CaptivePortal(
        ap_ssid="lightmotron-setup",
        ap_password="",
        portal_title="Lightmotron Setup",
        portal_heading="Lightmotron",
        default_hostname="lightmotron",
    ).start()  # blocks and resets device when done

# Attempt to connect to WiFi
WIFIManager(block=True, timeout=20)

if not WIFIManager().is_connected:
    # Connection failed — start captive portal so user can correct credentials
    print("WiFi connection failed, starting captive portal...")
    from captive_portal import CaptivePortal  # type: ignore

    CaptivePortal(
        ap_ssid="lightmotron-setup",
        ap_password="",
        portal_title="Lightmotron Setup",
        portal_heading="Lightmotron",
        default_hostname="lightmotron",
    ).start()  # blocks and resets device when done

_active_hostname = network.hostname()
_active_ip = WIFIManager().ip
print("Hostname:", _active_hostname)
print("Home URL (mDNS): http://" + _active_hostname + ".local/")
print("Home URL (IP):   http://" + _active_ip + "/")

# Register routes before starting the server so '/' resolves to HomeView
# immediately and does not transiently fall back to static www/index.html.
import web.routes

# Create/start the web server after routes are in place.
web_server = WebServer()
web_server.start_in_thread()

# Initialise audio player — runs health check and logs module status at boot
try:
    from audio import AudioPlayer

    AudioPlayer()
    AudioPlayer().start_continuous_polling()
except Exception as audio_err:
    print("boot: audio init failed:", audio_err)
    sys.print_exception(audio_err)
    raise

print("Boot complete.\n")
