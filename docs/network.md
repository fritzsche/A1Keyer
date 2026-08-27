# Network Connectivity

A1Keyer can join a Wi-Fi network so the keyer becomes reachable over IP.
This is the foundation for two planned features — **remote keying** and a
**web interface** — but it is useful on its own: the device reports its
address on screen and serves the existing HTTP debug console.

Everything here is configured **on the device**, from the Cardputer
keyboard. No rebuild, no `secrets.h`, no phone app. That constraint comes
from how A1Keyer is actually distributed: users flash a prebuilt binary
with the web flasher and never touch a compiler.

---

## Contents

1. [Overview](#1-overview)
2. [Scope](#2-scope)
3. [User interface](#3-user-interface)
4. [Credential storage](#4-credential-storage)
5. [NVS schema](#5-nvs-schema)
6. [Connection state machine](#6-connection-state-machine)
7. [Error reporting](#7-error-reporting)
8. [Boot behaviour and auto-connect](#8-boot-behaviour-and-auto-connect)
9. [Real-time safety](#9-real-time-safety)
10. [Interaction with the web flasher](#10-interaction-with-the-web-flasher)
11. [Design decisions and rejected alternatives](#11-design-decisions-and-rejected-alternatives)
12. [Future work](#12-future-work)
13. [Access-point mode](#13-access-point-mode)

---

## 1. Overview

- **Function:** associate with a Wi-Fi access point as a station (STA),
  obtain an address by DHCP, and keep the link up across reboots and
  dropouts. **Or** turn the device itself into a Wi-Fi access point
  (AP), with a fixed SSID and a user-chosen passphrase. See
  [§13](#13-access-point-mode).
- **Configuration:** on-device. Press `C` to scan, pick an AP from the
  list, type the password on the built-in keyboard, connect.
  Press `A` to enter access-point mode (password entry is reused for
  the AP passphrase).
- **Persistence:** credentials are stored in NVS and re-applied on every
  boot. The device auto-connects in STA mode unattended; in AP mode the
  saved passphrase is reused so reboots do not require retyping.
- **Status:** press `N` for state, SSID, IP address, and (in AP mode)
  the count of connected stations. Disconnect from the same screen.
- **Default:** **Off**. A device with no stored credentials and no
  explicit AP-mode boot keeps the radio powered down and never
  associates.
- **Target:** M5Stack Cardputer ADV. The Tab5 (ESP32-P4) build compiles
  the UI out; the network layer itself is portable.

---

## 2. Scope

**In scope now**

- Station mode with DHCP.
- Scan, select, password entry, connect, disconnect, forget.
- Auto-connect at boot with exponential reconnect backoff.
- On-screen status and error reporting.
- Access-point mode (see [§13](#13-access-point-mode)).

**Deliberately out of scope for this iteration** — but the API, the NVS
schema and the state machine are all shaped to accept them without a
rewrite. See [§12](#12-future-work).

- Station mode with a static address.
- Remote keying and the web interface.

---

## 3. User interface

Three top-level keys, following the same convention as the existing
settings screens (`W` for WPM, `F` for frequency, and so on).

| Key | From | Action |
|---|---|---|
| `C` | Decoder | Open the network configuration flow (STA); starts a scan immediately. |
| `A` | Decoder | Open the access-point configuration flow. |
| `N` | Decoder | Open the network information screen. |

### 3.1 Scan list

The scan runs asynchronously. While it is in flight the screen shows
`Scanning networks…` with an animated ellipsis; the keyer remains fully
responsive throughout.

Results are presented as a scrolling list, four rows per page on the
240×135 display, each row showing the SSID (truncated to 11 characters),
a padlock for encrypted networks, and the signal strength.

| Key | Action |
|---|---|
| `;` / `.` | Move the selection up / down (the viewport follows). |
| `Enter` | Select. Open networks connect directly; encrypted networks go to password entry. |
| `Esc` | Cancel the scan and return to the decoder. |

### 3.2 Password entry

The first free-text input in A1Keyer. Characters are masked by default.

| Key | Action |
|---|---|
| printable | Append (clamped at 63 characters). |
| `Backspace` | Delete the last character. |
| `Shift`+`Space` | Toggle between masked and plaintext. |
| `Enter` | Save the credentials and start connecting. |
| `Esc` | Abandon and return to the scan list. |

### 3.3 Network information

Shows the current state, the SSID, the DHCP address, and — when the last
attempt failed — the reason ([§7](#7-error-reporting)).

| Key | Action |
|---|---|
| `X` | Disconnect and forget the stored credentials. |
| `R` | Retry the connection. |
| `Esc` / `Enter` | Return to the decoder. |

> **Note:** disconnect is bound to `X`, not `D`. `D` already toggles
> between Console and WinKey mode on the USB port.

---

## 4. Credential storage

Credentials live in **NVS**, via the Arduino `Preferences` API, in a
namespace named **`net`** — separate from the `morse` namespace that
holds WPM, sidetone frequency, volume, key type and radio keying.

### 4.1 Why NVS

NVS is the ESP-IDF-native key/value store: wear-levelled, transactional,
and already in use elsewhere in this firmware. The alternatives were
considered and rejected — a LittleFS JSON file adds a filesystem for no
benefit and is equally plaintext, and Arduino `EEPROM` is a legacy shim
over the same NVS partition.

### 4.2 Owning the credentials: `WIFI_STORAGE_RAM`

There is a trap here worth documenting, because the obvious fix is the
wrong one.

By default `esp_wifi` keeps **its own copy** of the SSID and passphrase
in the `nvs.net80211` namespace, because `esp_wifi_set_storage()`
defaults to `WIFI_STORAGE_FLASH`. That shadow copy is invisible to our
code. If the user asks the device to *forget* a network and we only clear
our own `net` namespace, the driver can still reassociate from its
private copy after a reboot — the credentials appear to be deleted but
the device rejoins anyway.

The widely-cited remedy, `WiFi.persistent(false)`, **does not solve
this** on Arduino-ESP32 3.x. In 3.x the flag defaults to *true* and only
suppresses persisting the Wi-Fi *mode*, not the credentials.

The correct call is:

```cpp
esp_wifi_set_storage(WIFI_STORAGE_RAM);
```

issued in `NetworkManager::begin()` before any `WiFi.begin()`. The driver
then holds credentials in RAM only, and the `net` namespace is the single
source of truth. `disconnectAndForget()` genuinely forgets.

This is directly testable: forget a network, power-cycle, and confirm the
device stays offline. A reassociation there means the shadow copy is
still live.

### 4.3 The passphrase is stored in plaintext

**Anyone with physical access to the device can recover the Wi-Fi
password**, by dumping flash over USB or JTAG. This is a deliberate,
documented trade-off.

Protecting it properly requires NVS encryption, which in turn requires
flash encryption and secure boot. That means burning eFuses — an
irreversible, per-device operation that would break the web-flasher
distribution model, complicate every future firmware update, and make
recovery from a bad flash impossible for a user. For a hobby keyer whose
threat model is "someone who is holding my keyer could also just read the
password off my router", the cost is not justified.

Mitigations that *are* in place: the credentials are in a namespace we
control and can erase on demand, the driver's shadow copy is disabled,
and the UI offers an explicit forget action.

---

## 5. NVS schema

Namespace **`net`**.

| Key | Type | Default | Purpose |
|---|---|---|---|
| `n` | `uint8` | `0` | Number of stored networks, 0–4. |
| `ssid0`…`ssid3` | `String` (≤32) | `""` | SSID per slot. |
| `pass0`…`pass3` | `String` (≤63) | `""` | Passphrase per slot, plaintext. |
| `ip_mode` | `uint8` | `0` | `0` = DHCP, `1` = static. *Reserved.* |
| `ip` | `uint32` | `0` | Static address. *Reserved.* |
| `gw` | `uint32` | `0` | Gateway. *Reserved.* |
| `mask` | `uint32` | `0` | Netmask. *Reserved.* |
| `dns` | `uint32` | `0` | DNS server. *Reserved.* |
| `mode` | `uint8` | `0` | `0` = STA, `1` = AP. |
| `ap_pass` | `String` (≤63) | `""` | AP passphrase. Empty = open AP. |

The SSID of the access point is the fixed string `A1Keyer` and is not
persisted. See [§13](#13-access-point-mode) for why the AP passphrase
lives in its own key separate from the STA passphrases.

Two notes on the shape of this table.

**The slot list exists from the first version**, even though the UI only
ever writes slot 0. Storage layout is the expensive thing to change once
devices are in the field; adding the remaining slots later would need a
migration path. Storing a list of known networks is standard practice —
`WiFiMulti` in the Arduino core does exactly this at runtime, though it
provides no persistence of its own, so the application has to re-register
the list from its own store on every boot.

**The reserved keys are read and written now** but not exposed in the UI.
They are the seam for static addressing; the AP passphrase was promoted
from "future" to "now" once AP mode shipped.

Access goes through free functions (`netConfigLoad`, `netConfigSave`,
`netConfigSaveAp`, `netConfigClearAp`, `netConfigClear`) rather than
inline `Preferences` calls, so the schema can be exercised host-side
against an in-memory mock.

### 5.1 Flash wear

Not a concern. NVS sectors are rated for tens of thousands of erase
cycles and are wear-levelled; a ~64-byte credential write per
provisioning event leaves decades of headroom. The one rule that follows
from this: **do not write on every disconnect** — only on an explicit
user save.

---

## 6. Connection state machine

Owned by `NetworkManager`. Advanced exclusively from `loop()`; never
blocks.

```
            'C' ──► startScan()
                        │
   IDLE ────────────► SCANNING ────► SCAN_DONE ────► CONNECTING ────► CONNECTED
    ▲  ▲                  │              │                │              │
    │  │                  ▼              │ Esc            ▼              │ link
    │  │             SCAN_FAILED         │           CONNECT_FAILED      │ drop
    │  │                  │              │                │              ▼
    │  └──────────────────┴──────────────┘                │        DISCONNECTED
    │                                                     │              │
    │                                              retry ─┘              │
    └──────────────── 'X' disconnect + forget ◄───────────────── backoff ┘
```

### 6.1 Events, not polling

State transitions are driven by Wi-Fi **events**, not by polling
`WiFi.status()`:

- `ARDUINO_EVENT_WIFI_SCAN_DONE`
- `ARDUINO_EVENT_WIFI_STA_GOT_IP`
- `ARDUINO_EVENT_WIFI_STA_DISCONNECTED`

The last one carries `info.wifi_sta_disconnected.reason`, and that field
is the only way to tell a wrong password from a missing network from a
faded signal. Polling `WiFi.status()` collapses all three into an
indistinguishable `WL_DISCONNECTED`, which would reduce every failure to
a useless "could not connect".

Event handlers run in the Wi-Fi task's context. They therefore do the
**minimum possible**: record the reason code, set an atomic flag, return.
All real work — including any call to `WiFi.begin()` — happens in
`poll()` on the loop core. Calling `WiFi.begin()` from inside a
disconnect handler is a recursion hazard and is never done.

Timeouts remain as a backstop in case an event never arrives: 10 s for a
scan, 15 s for an association.

### 6.2 Reconnect backoff

`WiFi.setAutoReconnect()` is left **off**. The built-in behaviour uses a
fixed short backoff, caps at 30 s, and retries forever — too aggressive
for a battery-powered device and impossible to surface in the UI.

Instead: exponential backoff of 1, 2, 4, 8 … seconds, capped at five
minutes, reset on `GOT_IP`. Repeated `NO_AP_FOUND` triggers a fresh scan
rather than another blind attempt, since the AP may have changed channel.

### 6.3 Guards

- `startScan()` is a no-op while a scan is already in flight, so
  repeatedly pressing `C` cannot stack scans.
- `cancel()` issues `WiFi.disconnect()`, so backing out of the flow never
  leaves an orphaned association attempt running.

---

## 7. Error reporting

Every failure produces a short message shown verbatim on the network
information screen, and every failure is recoverable from the UI — there
are no resets and no dead ends.

| Condition | Message |
|---|---|
| `AUTH_FAIL` (202), `4WAY_HANDSHAKE_TIMEOUT` (15) | `wrong password` |
| `NO_AP_FOUND` (201) | `network not found (5GHz-only?)` |
| `BEACON_TIMEOUT` (200), `ASSOC_LEAVE` (8) | `signal lost, reconnecting` |
| Scan produced no event within 10 s | `scan timeout` |
| No association within 15 s | `connect timeout` |
| Anything else | `connect failed (reason N)` |

The `NO_AP_FOUND` wording is deliberate. The ESP32-S3 is 2.4 GHz only,
and a user whose router presents a single 5 GHz-capable SSID will see the
network on a phone but not here; the hint saves a support round-trip.

---

## 8. Boot behaviour and auto-connect

Ordering in `setup()`, after the `morse` settings are loaded and before
the debug console is started:

1. `NetworkManager::begin()` — set STA mode, apply
   `esp_wifi_set_storage(WIFI_STORAGE_RAM)`, register the event handler,
   load the `net` namespace.
2. If credentials exist, `NetworkManager::connectWithSaved()` — starts an
   asynchronous association and returns immediately.

**Boot is not delayed.** Association completes in the background while
the keyer is already usable; the status line reflects the outcome when it
arrives. If no credentials are stored, the radio is not brought up at
all.

### 8.1 The HTTP debug console

A small built-in HTTP server (`GET /`, `GET /state`, `GET /log?n=N`) is
available for development and is **compiled out** of shipping builds.
The toggle is the marker file `src/wifi_debug.enable`:

- File present (e.g. `touch src/wifi_debug.enable`)
  → `scripts/wifi_debug_auto.py` defines `ENABLE_WIFI_DEBUG=1` and the
  HTTP console + Wi-Fi state logging are compiled in.
- File absent → `ENABLE_WIFI_DEBUG=0`, shipping default.

The marker is git-ignored, so toggling is purely local. This file used
to be `src/secrets.h`, which conflated the credentials source with the
debug-console flag. Credentials now live exclusively in NVS via the
on-device keyboard flow (see §3); the marker file is empty and only
expresses "I want the dev console in this build".

The console starts listening on port 80 lazily from `loop()`: routes are
registered at `setup()`, but `_server.begin()` only fires once
`WifiMgr::isConnected()` returns true. This avoids the race where
`WiFi.begin()` is non-blocking and the link is rarely up by the time
`setup()` exits.

#### TX buffer endpoints (v0.6.0+)

The web UI exposes a TX buffer card (type-and-send free-form CW text)
via five `/api/tx/*` endpoints — see [`docs/tx_buffer.md`](tx_buffer.md)
for the operator reference. These endpoints use the same shared
`WebServer` on port 80.

---

## 9. Real-time safety

The keyer's timing must not degrade because the device is on a network.
Three rules:

- **Nothing blocks.** The scan is asynchronous
  (`WiFi.scanNetworks(true)`) and association is event-driven. A
  synchronous scan blocks for one to three seconds, which would freeze
  the keyboard and display. The acceptance test is explicit: paddle
  keying stays responsive for the whole duration of a scan.
- **Wi-Fi stays on the loop core.** The Arduino-ESP32 Wi-Fi API is not
  thread-safe, so every call is made from `loop()`. The audio task
  remains pinned to core 1 at priority 22 and is untouched.
- **Event handlers do nothing.** See [§6.1](#61-events-not-polling).

Two hardware notes for later:

- Wi-Fi transmit peaks draw upwards of 500 mA on the ESP32-S3. If audio
  glitching appears when transmit coincides with the speaker amplifier,
  `WiFi.setTxPower(WIFI_POWER_15dBm)` is the first thing to try.
- The default `WIFI_PS_MIN_MODEM` power save adds up to one DTIM interval
  (100–300 ms) of receive latency. Irrelevant for a status page;
  **unacceptable for remote keying**, which will need `WIFI_PS_NONE`
  while a session is active.

---

## 10. Interaction with the web flasher

Now that configuration lives in NVS, the two modes offered by the web
flasher (`/js/A1Keyer-Flasher/manifest.json`) have a user-visible
difference for the first time:

| Mode | Image | Offset | Erase | Effect on settings |
|---|---|---|---|---|
| **Full flash** | `…-merged.bin` | `0x0` | yes | **Erases NVS.** Wi-Fi credentials *and* WPM/frequency/volume/key settings are lost. |
| **App update** | `….bin` | `0x10000` | no | Preserves NVS. Network and keyer settings survive. |

Use *app update* for routine firmware updates and *full flash* only for a
new or broken device. The flasher's mode labels and README state this
explicitly.

Recovering after a full flash means re-entering the Wi-Fi password on the
device — mildly annoying, and the reason the distinction is worth
spelling out in the UI rather than only here.

---

## 11. Design decisions and rejected alternatives

Provisioning was surveyed broadly before settling on the keyboard UI.
Recorded here so the question does not have to be reopened.

| Approach | Verdict |
|---|---|
| **Keyboard and screen UI** | **Chosen.** The Cardputer has a real keyboard and an LCD, and the operator is standing in front of it. Every other option below is a workaround for input hardware we already have. |
| WiFiManager captive portal | Rejected. Solves "the device has no input"; irrelevant here. Would be the right answer for a future headless variant. |
| ESP-IDF `wifi_provisioning` (SoftAP or BLE) with a phone app | Rejected. Introduces a phone-app dependency for a device that needs none. BLE on this board is separately troublesome — see [`ble_error.md`](ble_error.md). |
| ESP RainMaker | Rejected. Cloud service; far out of scope. |
| WPS push-button | **Unavailable.** Removed from Arduino-ESP32 3.x and ESP-IDF 5.1+. |
| SmartConfig / ESP-TOUCH | Rejected. Dated and requires a vendor app. |
| Wi-Fi Easy Connect (DPP) | Deferred. Attractive — scan a QR code, natively supported by phone operating systems — but as of 2025 there is no clean Arduino wrapper. Worth revisiting. |
| **Improv Wi-Fi Serial** | **Deferred.** The one genuinely strong alternative. See below. |

### 11.1 Improv Wi-Fi Serial

[Improv](https://www.improv-wifi.com/serial/) is the de-facto standard
for provisioning a device over USB from a browser, and it is the natural
companion to a web flasher — ESPHome, Tasmota and others use it. It is
deferred rather than rejected, for two concrete reasons.

**The flasher would need to grow support.** A1Keyer's flasher is a custom
application built directly on esptool-js, not ESP Web Tools, so Improv
support does not come for free; both ends would have to be written.

**It collides with WinKeyer.** The USB CDC port already carries the WK2
binary protocol, and operators leave the keyer connected to their logger
for the entire time they are on the air. The Improv wire format has no
framing, no escape byte and no inter-frame gap: any six-byte run matching
`IMPROV\x01` inside a WK2 stream would be misparsed as a provisioning
packet. Safe coexistence needs an explicit gate — for example holding a
key during the first two seconds of boot to open a time-limited Improv
window — or a second CDC interface dedicated to it.

Neither is hard, but neither is justified while the device in front of
the user has a keyboard. The case for Improv becomes strong the moment a
keyboard-less variant exists.

---

## 12. Future work

- **Static addressing in station mode.** The NVS keys
  (`ip_mode`, `ip`, `gw`, `mask`, `dns`) are already reserved; the work
  is a UI for entering four dotted quads and a branch in the connect
  path.
- **Tight DHCP lease range in AP mode.** The current build accepts
  ESP-IDF's default DHCP lease range (`192.168.73.2..N`). Setting a
  custom range (e.g. `192.168.73.10..100`) requires lwIP private-API
  calls (`dhcps_set_option_info`) whose signatures have drifted across
  ESP-IDF versions; deferred to a follow-up that pins the range on a
  per-platform basis.
- **Custom DHCP lease time.** Same as above; ESP-IDF does not expose a
  stable public API.
- **Multiple known networks.** The four storage slots exist; what is
  missing is the UI for managing them and best-AP selection at connect
  time.
- **Improv Wi-Fi Serial** — see [§11.1](#111-improv-wi-fi-serial).
- **Remote keying.** Requires `WIFI_PS_NONE` while a session is active
  ([§9](#9-real-time-safety)) and a latency budget that has not yet been
  measured.
- **Web interface.** The existing `ConsoleServer` is a debug-only
  read-only endpoint gated behind `ENABLE_WIFI_DEBUG`; a real control
  interface is a separate piece of work.

---

## 13. Access-point mode

The Cardputer can also act as a Wi-Fi access point. The use case is
exactly what a single-station CW keyer needs: an operator stands in
front of the device with no other infrastructure, the device announces
itself, and the operator's phone or laptop joins it for direct control.
A captive portal is intentionally not part of this design — the operator
already has the keyboard on the device for configuration, and the HTTP
listener (`HttpServer`) lazy-starts on the AP interface exactly as it
does in STA mode.

### 13.1 Defaults

| Setting | Value | Source |
|---|---|---|
| SSID | `A1Keyer` (literal, not configurable) | `network_manager.h:kApSsid` |
| Device address | `192.168.73.1/24` | `network_manager.h:kApIpAddr / kApNetmask` |
| Gateway | `192.168.73.1` | `kApGwAddr` |
| Max clients | `4` | `kApMaxStations` — Arduino default |
| Channel | `1` | hard-coded |
| DHCP lease range | ESP-IDF default (`.2..N` of `/24`) | no override |
| Hidden | no | `ssid_hidden=0` |

The DHCP lease range is the documented limitation called out in
[§12](#12-future-work). In practice the user assigns a phone and maybe a
laptop, both of which accept the first address the server hands them;
the .2..N range is more than enough.

### 13.2 User interface

| Key | From | Action |
|---|---|---|
| `A` | Decoder | Open AP passphrase entry. ENTER starts the AP. ESC returns. |
| `A` (when AP already up) | Decoder | Skip the password step and jump straight to the info screen. |
| `N` | Decoder | Open the network information screen. Mode-aware. |

The AP passphrase entry reuses the STA password screen layout — same
masking, same `FN show` toggle, same `FN , left  FN / right` cursor
navigation, same `ENTER ok  ESC bk` hint — so the editor does not have
to be re-learned. The title is "AP pw" instead of "WiFi pw" and the
SSID line is locked to "A1Keyer (AP)".

#### N-screen (mode-aware)

**STA:** unchanged. See [§3.3](#33-network-information).

**AP:**

```
AP     up
SSID:  A1Keyer
clients: N/4
hint:  X stop AP   ENT back
```

The retry-countdown row is omitted in AP mode (no automatic retry). The
SSID is always "A1Keyer" — it is not a stored credential. `X` calls
`WifiMgr::disconnectCurrent()` (see below); the AP password is not
erased.

### 13.3 Mode persistence

The radio's role lives in the existing NVS `mode` key (`0`=STA,
`1`=AP). Boot behaviour:

- `mode == STA`: existing auto-connect from saved STA credentials
  ([§8](#8-boot-behaviour-and-auto-connect)).
- `mode == AP`: nothing automatic. The user must press `A` to bring
  the AP up, or `C` to switch back to STA mode (the switch calls
  `disconnectCurrent()` first to free the AP interface). The previous
  AP passphrase is re-applied automatically, so a reboot does not
  require retyping it.

Pressing `A` saves `mode=AP` and the typed passphrase before the AP
is actually brought up — the same "persist on commit" discipline used
in STA mode ([§6](#6-connection-state-machine)).

### 13.4 The "no-retyping" guarantee

AP and STA credentials are stored in **separate** NVS keys:

- STA: `ssid0` / `pass0` … `ssid3` / `pass3`
- AP: `ap_pass`

Toggling modes never touches the other set. Concretely:

| User action | NVS touched |
|---|---|
| `A` → set AP password → AP comes up | `ap_pass`, `mode=AP` |
| Later `C` → stop AP, scan + connect STA | (nothing — STA creds still there) |
| Press `A` again | (nothing — AP passphrase was kept) |
| `N`-screen `X` in AP mode | (nothing — AP passphrase was kept) |
| `N`-screen `X` in STA mode | confirms with Y/N, then erases `ssid0` / `pass0` only |

The Y/N confirmation on STA-mode "forget" is the safety net for the one
destructive gesture in the flow. AP mode's `X` does not need a confirm
because it does not erase anything.

### 13.5 `disconnectCurrent()` vs `disconnectAndForget()`

Two distinct APIs, both single-purpose:

- `WifiMgr::disconnectCurrent()` — drops whatever link is up
  (STA or AP) without touching NVS. Use this when the user just wants
  the radio off (e.g. switching modes).
- `WifiMgr::disconnectAndForget()` — STA-only legacy. Drops the link
  AND erases the saved STA credentials. In AP mode it falls back to
  `disconnectCurrent()`-equivalent behaviour: the AP password and
  mode flag are intentionally preserved so the user can re-enter AP
  mode later without retyping.

### 13.6 Why no custom DHCP lease range

ESP-IDF v5.5 / Arduino-ESP32 3.x do not expose a public API for
constraining the DHCP lease pool. Setting a custom range requires
calling lwIP's `dhcps_set_option_info()` / `dhcps_set_pool()` under
`priv_include/`, with signatures that have drifted across
`pioarduino` releases. The cost of locking ourselves to one signature
was not justified for a hobby keyer. The .2..N default is documented on
the N-screen as `clients: N/4`.
