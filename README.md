# A1Keyer

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Changelog](https://img.shields.io/badge/changelog-0.4.0-blue.svg)](CHANGELOG.md)
[![Web Flasher](https://img.shields.io/badge/Web_Flasher-flash%20in%20browser-orange)](https://fritzsche.github.io/A1Keyer-Flasher/)
[![Version](https://img.shields.io/badge/version-0.4.0-green.svg)](#version)

Low-latency Morse code trainer and **computer-controlled CW keyer** for the
**M5Stack Cardputer ADV**. Plug in a paddle or a straight key, send CW, hear
it on the speaker, see it decoded as text on the device screen — sub-3 ms
audio latency end-to-end. Key your transceiver through an isolated
optocoupler, drive CW from your logger via **WinKey (WK2) emulation** over
USB, store ten CW memories, and configure everything either from the
built-in keyboard or from the **web interface** over Wi-Fi.
**Current release: v0.4.0.**

---

## What is A1Keyer?

A1Keyer turns an M5Stack Cardputer ADV into a self-contained CW station
tool. You connect a key, you send CW, and the device:

- **Plays** your keying through the on-board speaker (or, on a straight
  key, decodes your on/off keying into the same audio path).
- **Decodes** your keying back to text on the 240×135 ST7789V2 display,
  with the last ~200 characters visible at any time.
- **Keys your radio** — drives a real transceiver's CW/KEY input through a
  PC817 optocoupler on the Cardputer's EXT extension header (galvanically
  isolated, off by default).
- **Speaks WinKey** — emulates a K1EL WinKeyer 2 over the USB serial port,
  so contest loggers and logging programs (RUMlogNG, N1MM Logger+, fldigi,
  WriteLog, …) can send CW through A1Keyer with no extra software.
- **Plays text** — type a string or recall one of ten stored **memory
  slots** (`M0`–`M9`) and A1Keyer renders it as CW through sidetone, the
  WinKey bridge, and the radio output.
- **Serves a web interface** — join it to your WLAN and adjust WPM, tone,
  volume, polarity, keyer type, radio keying, and all ten memories from
  any phone or laptop browser.
- **Persists your settings** (WPM, tone, volume, keyer type, paddle
  polarity, radio keying, memories, Wi-Fi credentials) to flash so the
  device is ready the moment you power it on.

A Tab5 (ESP32-P4) build exists in the source tree but is **not part of
this release** — see the [CHANGELOG](CHANGELOG.md) for the rationale.

---

## Flash it in your browser

No toolchain install required — the **web flasher** programs the device
over USB directly from a Chromium-based browser (Chrome / Edge / Brave):

> ### 🔗 Web Flasher: [https://fritzsche.github.io/A1Keyer-Flasher/](https://fritzsche.github.io/A1Keyer-Flasher/)

1. Plug your **M5Stack Cardputer ADV** into your computer with a USB-C
   cable.
2. Open the [web flasher](https://fritzsche.github.io/A1Keyer-Flasher/) in
   Chrome / Edge / Brave.
3. Click **Flash**, select the correct serial port, wait ~30 seconds.
4. Unplug, press the power button, and the device boots into A1Keyer.

The flasher downloads the latest pre-built
`a1keyer-cardputer-vX.Y.Z-merged.bin` from the
[GitHub releases page](https://github.com/fritzsche/A1Keyer/releases).

> **Two flash modes:** *full flash* erases settings (including Wi-Fi
> credentials); *app update* preserves them. Use *app update* for routine
> upgrades — see [`docs/network.md § 10`](docs/network.md#10-interaction-with-the-web-flasher).

### Building from source (PlatformIO)

If you'd rather build the firmware yourself — for development,
customisation, or to target a different environment — see
[Building and flashing](#building-and-flashing) below.

---

## Version

| Version | Date | Highlights |
|---|---|---|
| **[v0.4.0](https://github.com/fritzsche/A1Keyer/releases)** | *current* | **WinKey WK2 emulation** over USB (logger-driven CW, RUMlogNG/N1MM/fldigi/WriteLog), **CW memory keyer** (10 slots, keyboard + web editing/playback), **on-device web interface** (settings + live decode view over Wi-Fi), **paddle polarity setting**, robust Wi-Fi provisioning. |
| **[v0.3.0](https://github.com/fritzsche/A1Keyer/releases/tag/v0.3.0)** | *2026-08-13* | Bugfix release: Wi-Fi credentials persist across failed connects and reboots. |
| **[v0.2.0](https://github.com/fritzsche/A1Keyer/releases/tag/v0.2.0)** | *2026-08-13* | On-device Wi-Fi configuration (scan / password / forget), passphrase masking, HTTP state console. |
| **[v0.1.0](https://github.com/fritzsche/A1Keyer/releases/tag/v0.1.0)** | *2026-06-14* | Initial public release: iambic B + straight key + decoder + text encoder, radio keying output, web flasher. |

See the [CHANGELOG](CHANGELOG.md) for the full details of every release.

---

## Hardware

| Device | MCU | Audio | Display | Input | Radio keying | Env |
|---|---|---|---|---|---|---|
| **M5Stack Cardputer ADV** | ESP32-S3 @ 240 MHz | NS4168 I2S amp (mono) | 1.14" 240×135 ST7789V2 | 56-key matrix + Button A + GPIO paddle (Grove) | GPIO4 → optocoupler (EXT header) | `esp32s3_cardputer` |

The Cardputer target shares all platform-independent code (`morse_encoder`,
`key_envelop`, `morse_generator`, `iambic_keyer`, `straight_keyer`,
`morse_decoder`, `winkey_bridge`, `key_event_bus`, `memory_store`,
`morse_constants`, `MorseTable`, `Log`).

---

## Features

- **Iambic B keyer** — dual-paddle squeeze keying with proper SWAP /
  AUTOREPEAT / END-OF-CHAR logic and 7-dit word-space detection.
- **Straight key** — single-contact keying with bounce-guard verification
  and adaptive DIT/DAH classification based on a rolling median.
- **Paddle polarity** — Normal / Reversed swap of the paddle levers in
  software (`S` key), no rewiring needed. Iambic keyer only.
- **CW decoder** — your keying is rendered as text on the screen, with
  per-character colour-coding for what you keyed vs. what was played back.
- **Text playback** — type text or play one of ten stored **memory slots**
  (`0`–`9`) as CW through sidetone, the WinKey bridge, and the radio
  output, with playback characters in a distinct colour.
- **WinKey (WK2) emulation** — the USB-C serial port speaks the K1EL
  WinKeyer 2 protocol, so any logger with WinKeyer support keys CW through
  A1Keyer. Speed/sidetone commands, status bytes, echo, backspace/clear,
  tune, and WPM sync back to the host are implemented; host keying cannot
  override a locally disabled radio output.
- **Radio keying output** — drive a real transceiver's CW input through a
  PC817 optocoupler wired to the Cardputer's **EXT extension header**.
  Paddle, straight key, memories, WinKey host text, and the keyboard `K`
  key all mirror the sidetone; default Off. See
  [Keying your radio](#keying-your-radio-optocoupler-wiring) and
  [`docs/keyer.md`](docs/keyer.md).
- **Wi-Fi + web interface** — scan and join a WLAN from the device
  keyboard, or press **A** to turn the device itself into a Wi-Fi
  access point (`A1Keyer`, `192.168.73.1`). Either way, control the
  keyer from a browser: live decode view, settings, and memory
  editing/playback. See
  [Wi-Fi and the web interface](#wi-fi-and-the-web-interface).
- **Click-free audio** — Blackman-Harris envelopes eliminate the keying
  transients you hear on cheaper keyers.
- **Persistent settings** — WPM, tone frequency, volume, keyer type,
  paddle polarity, radio keying, memories, and Wi-Fi credentials survive
  reboots (NVS flash).
- **Screen-saver** — display dims after 5 minutes of inactivity; any key
  press or paddle touch wakes it and resets the timer.

---

## Connecting a key

A1Keyer accepts either a dual-lever **iambic paddle** or a single-contact
**straight key** (handkey). Wire one — not both at once.

Both key types use the **Grove Port.A** connector on the back of the
Cardputer ADV. The standard 4-pin Grove pinout is reused as GPIO:

| Grove pin | Wire colour | Mapped to | Used for |
|---|---|---|---|
| 1 | White | GPIO1 | DIT (dot) contact / straight-key contact |
| 2 | Yellow | GPIO2 | DAH (dash) contact — paddle only |
| 3 | Black | GND | Common / shaft ground |
| 4 | Red | (5 V) | **Not used** |

Both GPIO inputs have **internal pull-ups enabled** in firmware, so you do
**not** need external resistors — wiring is "contact closes to GND"
(active LOW).

### Morse paddle (iambic)

Wire each lever of your paddle between a signal pin and GND:

- **DIT lever** (the lever you press for a dot) → Grove pin 1 (white) and
  pin 3 (black, GND).
- **DAH lever** (the lever you press for a dash) → Grove pin 2 (yellow)
  and pin 3 (black, GND).

A standard 3-wire Grove-to-bare-ends cable works directly. For a 1/4" or
3.5 mm paddle jack, use a passive breakout — no resistor or capacitor
needed. If your levers feel swapped, don't rewire: press **S** and set
polarity to **Reversed**.

### Straight key / handkey

A straight key has only one contact, so only **one signal pin and GND**
are used:

- **Key contact** → Grove pin 1 (white).
- **Shaft / common** → Grove pin 3 (black, GND).

The DAH pin (yellow, GPIO2) is **not used** in straight-key mode.

> **A1Keyer does not auto-detect which key is connected.** After
> flashing, set the keyer type once to match your hardware — open the
> web interface (*Settings → Keyer type → Iambic paddle / Straight key*).
> In v0.4.0 there is no keyboard shortcut for paddle-vs-straight; the
> web interface is the only place to switch it. See
> [Wi-Fi and the web interface](#wi-fi-and-the-web-interface) below.

---

## Keying your radio (optocoupler wiring)

A1Keyer can key a real transceiver's CW input through a galvanically
isolated **PC817 optocoupler**. The signal leaves the Cardputer on its
**EXT 2.54-14P extension header** (not the Grove connector) — GPIO4 sits
right next to a GND pin there, so only two wires are needed.

> ⚠️ **You are keying a transmitter.** Verify your module and radio before
> going on-air. Full disclaimer, module modifications, and safety notes:
> [`docs/keyer.md`](docs/keyer.md). Verified on Icom IC-705 and Yaesu
> FTX-1 with the Hailege 2-channel PC817 module.

### Wiring overview

```
 Cardputer ADV                 PC817 module                 Transceiver
 (EXT 2.54-14P header)    (galvanically isolated sides)
┌─────────────────────┐        ┌────────────────┐          ┌──────────────┐
│                     │        │  INPUT side    │          │              │
│  G4  (GPIO4) ───────┼───────►│ +   (LED anode)│          │              │
│                     │        │                │ C ───────┼─► KEY        │
│  GND ───────────────┼───────►│ −   (LED cath.)│          │              │
│                     │        │ ═══ isolated ═══ E ───────┼─► KEY GND    │
└─────────────────────┘        │  OUTPUT side   │          └──────────────┘
                               └────────────────┘

   firmware: GPIO4 HIGH = keyed     no shared ground between Cardputer and radio
```

| Side | From | To |
|---|---|---|
| Cardputer → module | EXT header **G4** (GPIO4), white wire | PC817 **input +** (LED anode side, after the on-module resistor) |
| Cardputer → module | EXT header **GND**, black wire | PC817 **input −** (LED cathode) |
| Module → radio | Collector (**C**) | Radio **KEY** input |
| Module → radio | Emitter (**E**) | Radio **KEY GND** |

- Idle = GPIO4 LOW = radio not keyed. Keyed = GPIO4 HIGH for exactly as
  long as each dit/dah sounds (the output mirrors the sidetone envelope as
  a straight-key contact).
- Most transceivers provide their own pull-up on KEY — no extra parts on
  the radio side.
- **Cheap PC817 modules usually need two small modifications** to key
  reliably from 3.3 V: solder a ~220 Ω resistor in parallel with the
  on-module input resistor (~11 mA LED current instead of ~2 mA), and
  short the series indicator LED on the input side. Exact procedure:
  [`docs/keyer.md § 3.3`](docs/keyer.md#33-module-modifications-hailege-2-channel-variant).

### Enabling keying

The feature ships **Off**. Press **K** on the decoder screen, `;` for
**On**, **Enter** to save. While On, holding `K` keys the radio like a
straight key — handy for a `VVV TEST TEST` tune-up. A WinKey host can key
the radio too, but never when you've left keying Off locally.

---

## WinKey (WK2) support

A1Keyer emulates a **K1EL WinKeyer 2** over its USB-C serial port. Any
logging program with WinKeyer support — **RUMlogNG, N1MM Logger+, fldigi,
WriteLog**, hamlib-based tools, … — sends CW through the keyer exactly as
it would to a real WinKeyer unit: speed and sidetone commands, streamed
text with character echo, backspace/clear, tune/key-immediate, status
bytes, and WPM sync in both directions. Protocol details and the wire-level
reference live in [`docs/winkey.md`](docs/winkey.md).

### Setting up WinKey

The ESP32-S3 exposes a **single** USB serial port that time-shares between
a debug console and the WinKey protocol; the **D key** toggles between
them. To hook up your logger:

1. Flash the device and connect it to your computer over USB-C.
2. On the Cardputer, press **D** — the status line shows **`WK`**. The
   port now speaks the WK2 binary protocol; debug output is buffered
   internally so it cannot corrupt the stream. *(The device boots in
   WinKey mode by default, so an already-configured logger reconnects
   after every power cycle without touching the device.)*
3. In your logger's WinKeyer/CW settings, select the same serial port and
   connect:
   - **macOS:** `/dev/cu.usbmodem…` — e.g. RUMlogNG: Preferences →
     CW/WinKeyer → pick the device. It performs the host-open handshake;
     A1Keyer answers with the WK2 revision byte.
   - **Windows:** `COMx` (*USB Serial Device*) — N1MM+: Config →
     Ports → CW/Other → WinKey. fldigi: Configure → Rig Control →
     Winkeyer.
   - **Linux:** `/dev/ttyACM0`.
4. Set your desired WPM in the logger (or on the device — local WPM
   changes are pushed back to the host) and send CW from the logger.
5. Press **D** again to return to **Console** mode (for
   `pio device monitor`). Buffered debug output is replayed on return.

> Only one program may own the port: close the logger's serial connection
> before opening a serial monitor. Full flow including troubleshooting:
> [`docs/connecting.md`](docs/connecting.md) and
> [`docs/winkey.md`](docs/winkey.md).

---

## Wi-Fi and the web interface

A1Keyer joins your WLAN as a normal station and serves a dark,
mobile-responsive **web interface** on port 80 — no app, no cloud.
It can also act as its own access point when no WLAN is available.

### Connect the device to your WLAN

Everything happens on the device keyboard (no rebuild, no config files):

| Step | Key | Action |
|---|---|---|
| 1 | **C** | Start an asynchronous network scan (list appears; keyer stays responsive) |
| 2 | `;` / `.` | Move the selection through the SSID list (padlock = encrypted, signal strength shown) |
| 3 | **Enter** | Select. Open networks connect directly; encrypted ones ask for the password |
| 4 | printable keys / `Backspace` | Type the passphrase (masked as `*`; `Shift`+`Space` reveals) |
| 5 | **Enter** | Save credentials to flash and connect |
| 6 | **N** | Show network status: state, SSID, and the DHCP **IP address** |

Credentials persist in NVS and auto-connect at every boot (association
runs in the background — boot is never delayed). Wrong password, missing
network, and signal loss each get a plain-language message on the `N`
screen. `X` opens a **Y/N** confirmation and, on **Y**, forgets the
stored network; `R` retries. Details: [`docs/network.md`](docs/network.md).

### Or run the device as a Wi-Fi access point

When there's no WLAN handy — at a picnic table, in the car, at the
field-day site — press **A** instead of `C`. The device announces the
fixed SSID `A1Keyer` on `192.168.73.1/24` (DHCP-leases `.2..N`,
defaulting to `4` clients). Type a passphrase on the same editor as the
STA password screen; the password persists, so rebooting does not
require retyping it. Press **N** to see the connected-station count.

Mode is persisted: a reboot comes back in the mode the user last
chose. The AP password and the STA password live in separate NVS keys,
so toggling modes never destroys either. `X` on the N-screen drops the
AP without erasing the passphrase; the equivalent in STA mode still
requires a Y/N confirmation because it does erase. See
[`docs/network.md § 13`](docs/network.md#13-access-point-mode).

### Use the web interface

With the device on your network (either as STA or AP), open
**`http://<device-ip>/`** in any browser:

- **Live decode** — a scrolling view of the decoded CW text, refreshed
  twice a second.
- **Settings** — WPM, sidetone frequency, volume, paddle polarity, keyer
  type (paddle/straight — **the only place to switch it; the old `M`
  keyboard shortcut is now the memory picker**), **radio keying on/off**
  (with confirmation), and a Console↔WinKey mode switch. Every change
  applies immediately and is persisted to flash, mirroring the keyboard
  UI. (WinKey mode itself is session-only, matching the `D` key.)
- **Memories M0–M9** — edit any of the ten CW memory slots and save to
  flash, or hit ▶ to play a slot through sidetone, WinKey, and radio
  keying.

Under the hood it's three JSON endpoints (`POST /api/settings`,
`POST /api/memory`, `POST /api/play`) plus `GET /state` — scriptable from
curl if you want. The web server is compiled into the standard Cardputer
build; see `src/web_ui.cpp` and [`docs/network.md`](docs/network.md)
for what's next (static addressing, custom DHCP range).

---

## Memory keyer

Ten CW message slots (`M0`–`M9`) live in flash. From the decoder screen:

- **`0`–`9`** — play slot N immediately (through sidetone, WinKey, and —
  if enabled — the radio).
- **`M`** — open the memory picker; `0`–`9` switches slots, `Enter` opens
  the editor pre-filled with that slot, `X` clears it.
- In the editor: type text, `Fn`+`,` / `Fn`+`/` move the cursor, `Enter`
  commits, `Esc` discards.

Any keypress or paddle touch during playback aborts it cleanly. Full
reference: [`docs/memory.md`](docs/memory.md).

---

## Using A1Keyer

All on-device interaction is through the Cardputer's 56-key keyboard (plus
Button A on the front of the device). The default screen shows the **CW
decoder** with the last ~200 characters of decoded text.

### Keyboard shortcuts

| Key | Action |
|---|---|
| **W** | Open **WPM** settings screen |
| **F** | Open **tone Frequency** settings screen |
| **V** | Open **Volume** settings screen |
| **M** | Open the **memory picker** (then `0`–`9` select, `Enter` edits, `X` clears) |
| **K** | Open **Radio Keying** settings screen (On / Off). When On, holding `K` on the decoder screen keys the radio as a straight key. See [`docs/keyer.md`](docs/keyer.md). |
| **D** | Toggle the USB serial port between **Console** and **WinKey** mode (see [WinKey support](#winkey-wk2-support)) |
| **C** | Open **Wi-Fi configuration**: scan, pick an SSID, enter the password |
| **A** | Open **access-point configuration**: bring up `A1Keyer` as a Wi-Fi AP (press `N` for the connected-station count) |
| **N** | Open the **network info** screen: state, SSID, IP address; `X` forgets (STA) or drops the AP, `R` retries |
| **S** | Open **paddle polarity** settings screen (Normal / Reversed). Reversed swaps dit/dah on the paddle levers — iambic keyer only. |
| `;` | Increment value (WPM +1, freq +10 Hz, volume +10) — or select **On** in Radio Keying, **Normal** in Polarity |
| `.` | Decrement value — or select **Off** in Radio Keying, **Reversed** in Polarity |
| **Enter** | Save current setting to flash (NVS) and close the screen |
| **Esc** | Cancel the current screen / flow without saving |
| **Button A** | Same as Enter — dismiss any settings screen |
| **0**–**9** | Play memory slot N (decoder screen; empty slots are silent no-ops) |
| **P** | Play **"Hello Morse!"** through the speaker (text-encoder demo) |
| Any key | Wake the screen-saver; resets the 5-minute inactivity timer |

WPM and tone frequency are **mutable while audio is running** — you can
change them mid-sentence and the change is heard immediately. Paddle
polarity also applies immediately (on the next element). Volume and
radio keying require an **Enter** (or Button A) to take effect and be
saved.

While a memory or demo playback is running, **any key press or paddle
touch stops playback** and is consumed — release and press again for the
next action ([`docs/memory.md § 2.6`](docs/memory.md#26-cancel-on-any-keypress)).

> **Tip:** if your straight key is producing gibberish, you probably have
> the device in Paddle mode. Switch **Keyer type** to *Straight key* in
> the web interface (`http://<device-ip>/` → Settings), or from a paddle
> whose levers feel swapped, set polarity with **S**.

---

## Building and flashing

If you'd rather build the firmware yourself, or you're hacking on the code:

### Prerequisites

- [PlatformIO](https://platformio.org/) CLI or the PlatformIO IDE extension
  for VS Code
- USB-C cable

### Cardputer ADV (ESP32-S3)

```bash
pio run -e esp32s3_cardputer                # build
pio run -e esp32s3_cardputer -t upload      # flash at 1.5 Mbit/s
pio monitor -e esp32s3_cardputer            # serial monitor @ 115200 baud
```

`BOARD_CARDPUTER` is added to the build flags; hardware-specific code
inside `src/` is guarded with `#ifdef BOARD_CARDPUTER`. Note that pressing
**D** puts the port into WinKey mode — switch back to Console mode (or
close your logger) before opening a serial monitor.

The build also produces a **`firmware-merged.bin`** (via
`scripts/merge_bin.py`) suitable for the web flasher — that file is what
gets attached to GitHub releases as `a1keyer-cardputer-vX.Y.Z-merged.bin`.

---

## Documentation

| Document | What's in it |
|---|---|
| [`docs/winkey.md`](docs/winkey.md) | WinKey WK2 protocol reference and the A1Keyer emulator architecture: command map, status bytes, host-open handshake, RUMlogNG traces, threading model, tests. |
| [`docs/connecting.md`](docs/connecting.md) | End-user guide to the single USB serial port: Console vs WinKey mode, finding the port, connecting RUMlogNG / N1MM / fldigi. |
| [`docs/network.md`](docs/network.md) | Wi-Fi connectivity: on-device provisioning UI, credential storage (NVS schema), connection state machine, error reporting, interaction with the web flasher. |
| [`docs/memory.md`](docs/memory.md) | CW memory keyer: keystroke reference, storage, playback paths, editing flow. |
| [`docs/keyer.md`](docs/keyer.md) | Radio keying output: PC817 optocoupler wiring on the EXT header, module modifications, settings UI, central key-event bus, safety. |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Firmware internals: audio path, Blackman-Harris envelopes, Iambic B / straight-key FSMs, decoder protocol, ES8388 codec init, display subsystem, cross-core synchronisation, multi-device porting. |
| [`docs/TESTING.md`](docs/TESTING.md) | Unit-test framework: CMake / CTest wiring, `run_tests.sh`, `test_framework.h` macros, adding a new test suite. |

---

## Running the tests

```bash
./run_tests.sh
```

The unit tests compile the platform-independent library sources against a
minimal Arduino stub — no ESP32 toolchain required. Full documentation at
[`docs/TESTING.md`](docs/TESTING.md).

---

## Credits

A1Keyer's Iambic B keyer state machine and decoder ring buffer are a
port of **[cmorse](https://github.com/fritzsche/cmorse)** — a portable
Morse code trainer that runs on Windows, Linux, and macOS. cmorse is the
upstream project; A1Keyer adapts the same core logic to the M5Stack
Cardputer / Tab5 audio path. See
[`docs/ARCHITECTURE.md` § 16. cmorse reference](docs/ARCHITECTURE.md#16-cmorse-reference-extern)
for the port lineage and what was changed.

---

## License

MIT — see [`LICENSE`](LICENSE).
