# mDNS (`<hostname>.local`) reliability — investigation notes

**Status as of 2026-07-31: ROOT CAUSE CONFIRMED, resolved on the client side.**
Installing Bonjour Print Services on the Windows test machine fixed `.local`
resolution. The device's mDNS responder was working correctly all along —
Windows simply doesn't speak real mDNS natively (see "Root cause" below).
This file is kept for reference and for the LLMNR follow-up decision below;
append to the timeline rather than rewriting it if this needs revisiting.

## Root cause (confirmed 2026-07-31)

Windows does not use mDNS as its primary local-hostname resolution
mechanism — it uses **LLMNR** (Link-Local Multicast Name Resolution, RFC
4795, Microsoft's own protocol) and NetBIOS-NS, and only falls back to
trying real mDNS if something (Bonjour, iTunes, a printer driver, etc.) has
installed a resolver for it. The ESP32 only ever answered genuine mDNS
queries — correctly, per spec — so a stock Windows machine asking via LLMNR
got no answer regardless of anything on the device side. Installing Bonjour
gave Windows an mDNS-capable resolver, and resolution started working
immediately. **Every device-side change made below (power-save, IPv6
disable) was real and reasonable general mDNS hygiene, but neither was
actually the cause of this specific symptom** — worth remembering if this
resurfaces on a different client that already has a working mDNS resolver
(phones, Macs, Linux/Avahi), since the failure mode there would be
something else entirely.

## Second finding (2026-07-31): Android has a related but different gap — LLMNR won't fix it

Same device (hostname `nautilus`, not a different unit — confirmed with
user), tested from an Android phone: also can't resolve `nautilus.local`.

Android exposes mDNS via the `NsdManager` API for apps to use for
*programmatic service discovery*, but — unlike iOS — this is **not** wired
into the general system resolver. Chrome's address bar and a plain `ping`
on stock Android generally cannot resolve arbitrary `.local` hostnames at
all, regardless of whether the device's mDNS responder is working
perfectly (which it is).

**Important consequence for the LLMNR decision below: LLMNR would not help
Android at all.** LLMNR is a Windows-specific protocol; Android doesn't
speak it. There is no protocol we could add to the firmware that fixes
general-purpose `.local` resolution on stock Android's browser — that gap
is in how Android's system resolver is designed (only NSD-API-calling apps
get mDNS resolution), not anything fixable on the wire. An LLMNR responder
would therefore only close the Windows-without-Bonjour gap, not the
Android gap.

## Decision needed: LLMNR responder, and/or a client-independent fallback?

Given LLMNR only covers one of the two client gaps actually observed, the
realistic options are:
1. **Document it**: Windows users install Bonjour Print Services; Android
   users are told `.local` may not work in-browser and to use the IP
   address or an mDNS-browser app instead. Zero firmware risk.
2. **Implement an LLMNR responder** anyway — genuinely fixes the
   Windows-without-Bonjour case with nothing to install, but does nothing
   for Android. New UDP listener task, no existing ESP-IDF library for it.
3. **Client-independent fallback, works regardless of resolver support**:
   reserve a static/DHCP-fixed IP for the device on the router (by MAC
   address), so the IP never changes and can just be bookmarked — sidesteps
   the whole `.local` problem rather than solving it. Complements (doesn't
   replace) the existing audio "IP address announcement" feature, which
   already exists for exactly this class of problem. Zero firmware change;
   purely a router-config + documentation step.

Not yet decided — option 3 is likely the best return on effort given it
fixes the problem for *every* client regardless of resolver support, not
just Windows.

## 2026-08-02 — Third finding: the router reflects DHCP hostnames into unicast DNS

User tested `http://nautilus/` (bare hostname, no suffix at all) from the
Android phone that couldn't resolve `.local` — **it worked**. This means
this specific router reflects DHCP client hostnames into its own ordinary
unicast DNS resolver, which every client understands natively regardless of
mDNS/LLMNR support (this is the free/universal option floated in the prior
session — confirmed to apply here). Not fixable/controllable from the
firmware — it's a property of this particular router — but since it's free
when available, worth surfacing to every user rather than assuming a
particular suffix works.

**Change made (2026-08-02, build-verified)**: both captive-portal "join
successful" pages now list all four forms — `.local`, `.lan`, `.home`, and
the bare hostname — instead of just `.local`, plus a one-line note that
Android usually needs the plain hostname or `.lan`/`.home` instead of
`.local`. Since which (if any) of `.lan`/`.home`/bare actually resolve
depends entirely on the specific router, listing all of them costs nothing
and means whichever one this user's router happens to support just works
without them needing to know the mechanism.

- Python: `lib/captive_portal.py`'s `_SAVED_HTML` template (the post-save
  confirmation page only — Python's pre-save setup form has no equivalent
  note to update).
- C: `components/network/captive_portal.c` — both `build_saved_page()`
  (post-save confirmation) and `build_portal_html()`'s `.note` paragraph
  (pre-save setup form, which has a similar note that Python's doesn't).
  `build_saved_page()`'s buffer had to grow from 1024 to 1536 bytes
  (`-Werror=format-truncation` caught it at compile time) since escaping
  the hostname up to 128 chars, repeated 4x, plus the added copy no longer
  safely fit.

**LLMNR decision**: still not built. Given the router-reflection fix
already resolves the one case actually seen (Android + this network) with
zero firmware complexity, there's currently no concrete case left that only
an LLMNR responder would fix — revisit if a Windows-without-Bonjour-and-no-
router-reflection case actually comes up.

## Applies to: the C build only

The Python/MicroPython build (`boot.py`'s `network.hostname()`) does **not**
run a real mDNS responder at all — MicroPython's `network` module only sets
the DHCP hostname option; whether `.local` resolves depends entirely on
whether the router happens to reflect DHCP hostnames into its own DNS (some
do, most consumer routers don't). If you're ever testing the Python build
specifically, "mostly doesn't work" there isn't fixable from firmware at
all without adding a real mDNS library. Everything below is about
`c_project`, which uses ESP-IDF's real `mdns` component
(`components/network/mdns_setup.c`) — assuming that's what's being tested,
since that's been the focus of every recent session.

## My assessment: likely a mix, but probably weighted toward the client/network side

I can't observe your network or your Windows machine's resolver behavior
directly, so this is inference from the mechanism, not a diagnosis — the
diagnostic steps below are how to actually pin it down.

- **Most likely single factor: Windows' built-in mDNS support is genuinely
  unreliable** for general-purpose hostname resolution (browser address
  bar, `ping`) unless Apple's Bonjour service is installed (bundled with
  iTunes, or available as a standalone "Bonjour Print Services" installer).
  Windows added *some* native multicast-DNS support around Windows 10, but
  it's inconsistent outside specific contexts (e.g. printer/device
  discovery), and plenty of people never get plain `http://name.local/` to
  resolve in a browser without Bonjour. This would explain "mostly not
  working" well: occasional success (cache, retry timing) with a mostly-broken
  general case. **This is the first thing to rule out** — see diagnostic #1.
- **Plausible device-side contributor, just addressed**: dual-stack
  flakiness. See "2026-07-31" entry below.
- **Possible network/router contributor**: some routers/APs (especially
  mesh systems, ISP-provided combo units, or anything with "AP isolation" /
  weak IGMP snooping) don't forward multicast reliably between wireless
  clients, or split 2.4GHz/5GHz radios into different broadcast domains.
  Can't be fixed from the device; see diagnostic #3 for how to tell if this
  is happening.
- **Already ruled out / fixed**: ESP32 WiFi modem-sleep power-save
  (STA radio only waking on DTIM intervals, missing multicast queries
  arriving in between) — disabled in both builds. This was a real,
  well-understood ESP32 mDNS failure mode, but clearly wasn't the whole
  story since the problem persists.

## Diagnostics to actually localize the problem (do these before trying more device-side changes)

1. **Bonjour test (rules client vs. device in/out in one shot)**: install
   [Bonjour Print Services](https://support.apple.com/kb/DL999) (a small,
   standalone installer — you don't need iTunes) on the Windows machine,
   reboot, and retry `http://lightmotron.local/` (or your configured
   hostname). If it suddenly works reliably, the device was fine all
   along and this was a Windows resolver limitation — no more firmware
   work needed, just document "install Bonjour" as the setup instruction.
2. **Cross-device test**: try resolving the same hostname from a phone
   (iOS has real mDNS built in; most Android does too) on the same WiFi.
   If it works reliably from a phone but not from the Windows PC, that
   points at the Windows client specifically, not the device or network.
3. **Command-line resolution, bypassing the browser**: from Windows,
   `ping lightmotron.local` and separately `Resolve-DnsName lightmotron.local`
   in PowerShell. If neither resolves but the phone test in #2 succeeds,
   that's further confirmation it's Windows-side, not the network's
   multicast handling.
4. **Serial log correlation**: next time it fails, immediately check the
   device's serial log for what it was doing at that moment (was it mid
   template-render, had it just reconnected WiFi, etc.) in case there's a
   device-side timing window we haven't identified yet.

## Timeline

### 2026-07-2x — WiFi STA power-save disabled (confirmed necessary, not sufficient)

`esp_wifi_set_ps(WIFI_PS_NONE)` added right after `esp_wifi_start()` in
`components/network/wifi_manager.c` (and the equivalent `config(pm=...)`
call added to `lib/comms.py` for the Python build). Rationale: default
modem-sleep only wakes the radio on DTIM beacon intervals, so unsolicited
multicast mDNS queries arriving between wake windows get missed. This is a
real, standard ESP32 mDNS failure mode and was worth fixing regardless, but
user confirmed on 2026-07-31 that `.local` is "still mostly not working" —
so it was not the (or not the only) cause here.

### 2026-07-31 — IPv6 (AAAA) mDNS records disabled — untested on hardware yet

**Change:** `components/network/mdns_setup.c`'s `mdns_setup_init()` now
calls `mdns_netif_action(sta_netif, MDNS_EVENT_DISABLE_IP6)` after the
existing setup, using `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")` to
get the STA netif handle.

**Why:** `CONFIG_LWIP_IPV6=y` is enabled in this project, and lwIP
auto-configures a link-local IPv6 address on any connected interface
regardless of whether the LAN has real IPv6 multicast routing end to end
(most home networks don't, or only partially do). Traced through
`managed_components/espressif__mdns/mdns_netif.c`: the mdns component's
built-in event hooks enable IPv6 mDNS (AAAA records) automatically the
first time `IP_EVENT_GOT_IP6` fires — which happens shortly after connect
purely from link-local auto-config, not from any real IPv6 usability check.
A client that gets both an A and an AAAA record back for the same query and
tries the (non-functional, in most home LANs) IPv6 path first can stall or
fail rather than falling back cleanly to the working IPv4 address — this
looks exactly like "the hostname doesn't resolve" from the user's side even
though the device answered correctly.

**Known limitation, not yet handled**: this disable is a one-time call at
`mdns_setup_init()` (boot time, once). The mdns component's own event hooks
can re-enable IPv6 mDNS if the device does a full WiFi reconnect later in
the session (a fresh `IP_EVENT_GOT_IP6` re-fires `post_enable_pcb(...,
MDNS_IP_PROTOCOL_V6)`), since nothing currently re-applies the disable after
that point. Given this is a lighting controller that typically connects
once and stays connected, this should cover the common case, but if the
device is observed reconnecting mid-session (check for
`wifi:<ba-add>`/reconnect log lines) and the problem specifically resumes
after such an event, this is the next thing to fix — hook
`wifi_manager.c`'s existing `IP_EVENT_STA_GOT_IP` handler
(`components/network/wifi_manager.c` ~line 93) to re-apply the disable,
guarding against calling it before `mdns_setup_init()` has run on the very
first connect (ordering: `wifi_manager_connect()` unblocks and returns
*before* `boot.c` calls `mdns_setup_init()`, so a naive unconditional call
in the WiFi event handler would run mdns_netif_action too early on first
boot).

**Not done, and deliberately not attempted yet**: fully switching away from
the mdns component's "predefined netif" convenience mode
(`mdns_init()`/auto-managed) to manual `mdns_register_netif()` +
`mdns_netif_action()` management, which would give full control over
IPv4/IPv6 enablement without the auto-re-enable behavior at all. This is a
bigger, less-tested change to the mdns integration; worth considering if
the reconnect gap above turns out to matter in practice, but not justified
without evidence it's needed.

**To test:** flash `./flash_c.ps1 COM6 -Full` (touches app code) and retry
`.local` access. Please run diagnostics #1-#3 above regardless of whether
this helps — they'll tell us how much of the remaining problem (if any) is
even fixable from the firmware side.

## Candidate next steps (not yet tried)

- **LLMNR responder (strong candidate, discussed 2026-07-31)**: Windows
  doesn't use mDNS as its primary single-label-hostname resolution
  mechanism — it uses **LLMNR** (RFC 4795, Microsoft's own protocol) and
  NetBIOS-NS, and only tries mDNS at all if Bonjour is installed. The
  device currently answers real mDNS queries only; it never answers an
  LLMNR query. If diagnostic #1 (installing Bonjour) fixes resolution, that
  *confirms* this is the actual mechanism at play — Windows was asking via
  LLMNR, getting no answer, and Bonjour is what taught it to also try mDNS.
  A minimal LLMNR responder (UDP 5355, multicast 224.0.0.252, single
  question/answer, no service records — much simpler than mDNS, no
  ESP-IDF library exists for it so it'd be a small custom UDP listener
  task) would let `.local`-style resolution work on stock Windows with
  nothing to install, which is a better long-term fix than telling every
  user to install Bonjour. Not implemented yet — worth doing once
  diagnostic #1 confirms the theory, or sooner if we'd rather not wait.
- Re-apply the IPv6 disable on reconnect (see limitation above), if
  reconnects turn out to be happening and mattering.
- Reduce mDNS TTL / increase re-announcement frequency, in case records are
  being cached-and-expired unfavorably by some resolvers — low confidence
  this matters, not investigated yet.
- If Bonjour install (diagnostic #1) confirms it's a Windows-resolver
  problem and not fixable from the device: consider whether the existing
  audio "IP address announcement" feature (announces the IP by voice when
  it changes) is sufficient as the practical workaround, or whether a
  visual IP display (e.g. on the MAX7219 billboard, if configured) would
  help more day-to-day than continuing to chase `.local` reliability.
