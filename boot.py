# boot.py -- run on boot-up

import machine  # type: ignore
import micropython  # type: ignore
import sys  # type: ignore

import settings

# set up the exception buffer so we can see what happens if we crash
micropython.alloc_emergency_exception_buf(100)

# Ensure shared modules in /lib are resolved before stale root-level files.
if "/lib" in sys.path:
    try:
        sys.path.remove("/lib")
    except Exception:
        pass
sys.path.insert(0, "/lib")

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

# Initialise audio player — runs health check and logs module status at boot
try:
    import audio

    audio_module_path = getattr(audio, "__file__", "<unknown>")
    print("boot: audio module:", audio_module_path)

    audio_player_class = getattr(audio, "AudioPlayer", None)
    if audio_player_class is None:
        try:
            exported_names = [name for name in dir(audio) if not str(name).startswith("__")]
            print("boot: audio exports:", exported_names)
        except Exception:
            print("boot: audio exports: <unavailable>")

        print("boot: audio module missing AudioPlayer export; retrying clean import")
        try:
            if "audio" in sys.modules:
                del sys.modules["audio"]
            import gc  # type: ignore

            gc.collect()
            import audio as audio_retry

            retry_player_class = getattr(audio_retry, "AudioPlayer", None)
            if retry_player_class is None:
                print("boot: audio retry still missing AudioPlayer export")
            else:
                retry_player_class()
        except Exception as retry_error:
            print("boot: audio retry failed:", retry_error)
    else:
        audio_player_class()
except Exception as audio_err:
    print("boot: audio init failed:", audio_err)

# Start the I2C
# I2CManager()

print("Boot complete.\n")
