# A1Keyer

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Changelog](https://img.shields.io/badge/changelog-0.1.0-blue.svg)](CHANGELOG.md)
[![Web Flasher](https://img.shields.io/badge/Web_Flasher-flash%20in%20browser-orange)](https://fritzsche.github.io/A1Keyer-Flasher/)

Low-latency Morse code trainer firmware for the **M5Stack Cardputer ADV**.
Plug in a paddle or a straight key, send CW, hear it on the speaker and see it
decoded as text on the device screen — sub-3 ms audio latency end-to-end.
**Current release: v0.1.0.**

---

## What is A1Keyer?

A1Keyer turns an M5Stack Cardputer ADV into a self-contained Morse code
trainer. You connect a key, you send CW, and the device:

- **Plays** your keying through the on-board speaker (or, on a straight key,
  decodes your on/off keying into the same audio path).
- **Decodes** your keying back to text on the 240×135 ST7789V2 display, with
  the last ~200 characters visible at any time.
- **Plays text** — type a string and A1Keyer renders it as CW through the
  same audio path, so you can practise copying by ear.
- **Persists your settings** (WPM, tone, volume, keyer type) to flash so the
  device is ready the moment you power it on.

A Tab5 (ESP32-P4) build exists in the source tree but is **not part of this
release** — see the [CHANGELOG](CHANGELOG.md) for the rationale.

---

## Version

- **[v0.1.0](https://github.com/fritzsche/A1Keyer/releases/tag/v0.1.0)** —
  *2026-06-14* — Initial public release. Cardputer ADV (ESP32-S3) target,
  full iambic B + straight-key + decoder + text-encoder feature set, web
  flasher, and CTest unit-test suite.

---

## Hardware

| Device | MCU | Audio | Display | Input | Env |
|---|---|---|---|---|---|
| **M5Stack Cardputer ADV** | ESP32-S3 @ 240 MHz | NS4168 I2S amp (mono) | 1.14" 240×135 ST7789V2 | 56-key matrix + Button A + GPIO paddle (Grove) | `esp32s3_cardputer` |

The Cardputer target shares all platform-independent code (`morse_encoder`,
`key_envelop`, `morse_generator`, `iambic_keyer`, `straight_keyer`,
`morse_decoder`, `morse_constants`, `MorseTable`, `Log`).

---

## Features

- **Iambic B keyer** — dual-paddle squeeze keying with proper SWAP /
  AUTOREPEAT / END-OF-CHAR logic and 7-dit word-space detection.
- **Straight key** — single-contact keying with bounce-guard verification
  and adaptive DIT/DAH classification based on a rolling median.
- **CW decoder** — your keying is rendered as text on the screen, with
  per-character colour-coding for what you keyed vs. what was played back.
- **Text playback** — `playText("…")` renders typed text as CW through the
  same audio path, with playback characters in a distinct colour.
- **Click-free audio** — Blackman-Harris envelopes eliminate the keying
  transients you hear on cheaper keyers.
- **Adjustable settings** — WPM, tone frequency, volume, and keyer type
  (Paddle / Straight) are settable from the keyboard and **persist across
  reboots** (NVS flash).
- **Screen-saver** — display dims after 5 minutes of inactivity; any key
  press or paddle touch wakes it and resets the timer.

---

## Try it in 30 seconds

The fastest path to a working device is the **web flasher** — no toolchain
install required.

1. Plug your **M5Stack Cardputer ADV** into your computer with a USB-C cable.
2. Open **[fritzsche.github.io/A1Keyer-Flasher](https://fritzsche.github.io/A1Keyer-Flasher/)**
   in a Chromium-based browser (Chrome / Edge / Brave).
3. Click **Flash**, select the correct serial port, wait ~30 seconds.
4. Unplug, press the power button, and the device boots into A1Keyer.

The flasher downloads the latest pre-built `a1keyer-cardputer-vX.Y.Z-merged.bin`
from the [GitHub releases page](https://github.com/fritzsche/A1Keyer/releases).

### Building from source (PlatformIO)

If you'd rather build the firmware yourself — for development, customisation,
or to target a different environment — see [Building and flashing](#building-and-flashing)
below.

---

## Connecting a key

A1Keyer accepts either a dual-lever **iambic paddle** or a single-contact
**straight key** (handkey). Wire one — not both at once.

Both key types use the **Grove Port.A** connector on the back of the Cardputer
ADV. The standard 4-pin Grove pinout is reused as GPIO:

| Grove pin | Wire colour | Mapped to | Used for |
|---|---|---|---|
| 1 | White | GPIO1 | DIT (dot) contact / straight-key contact |
| 2 | Yellow | GPIO2 | DAH (dash) contact — paddle only |
| 3 | Black | GND | Common / shaft ground |
| 4 | Red | (5 V) | **Not used** |

Both GPIO inputs have **internal pull-ups enabled** in firmware, so you do
**not** need external resistors — wiring is "contact closes to GND" (active
LOW).

### Morse paddle (iambic)

Wire each lever of your paddle between a signal pin and GND:

- **DIT lever** (the lever you press for a dot) → Grove pin 1 (white) and
  pin 3 (black, GND).
- **DAH lever** (the lever you press for a dash) → Grove pin 2 (yellow)
  and pin 3 (black, GND).

A standard 3-wire Grove-to-bare-ends cable works directly. For a 1/4" or
3.5 mm paddle jack, use a passive breakout — no resistor or capacitor
needed.

### Straight key / handkey

A straight key has only one contact, so only **one signal pin and GND** are
used:

- **Key contact** → Grove pin 1 (white).
- **Shaft / common** → Grove pin 3 (black, GND).

The DAH pin (yellow, GPIO2) is **not used** in straight-key mode.

> **A1Keyer does not auto-detect which key is connected.** After flashing,
> open the mode settings (press **M**), then **;** for Paddle or **.** for
> Straight, then **Enter** (or Button A) to save. See [Using A1Keyer](#using-a1keyer)
> below.

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
| **M** | Open **keyer Mode** settings screen (Paddle / Straight) |
| `;` | Increment value (WPM +1, freq +10 Hz, volume +10) — or select **Paddle** in Mode |
| `.` | Decrement value — or select **Straight** in Mode |
| **Enter** | Save current setting to flash (NVS) and close the screen |
| **Button A** | Same as Enter — dismiss any settings screen |
| **P** | Play **"Hello Morse!"** through the speaker (text-encoder demo) |
| Any key | Wake the screen-saver; resets the 5-minute inactivity timer |

WPM and tone frequency are **mutable while audio is running** — you can
change them mid-sentence and the change is heard immediately. Volume and
keyer mode require an **Enter** (or Button A) to take effect.

> **Tip:** if your straight key is producing gibberish, you probably have
> the device in Paddle mode. Press **M**, then **.** (period) for Straight,
> then **Enter**.

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
inside `src/` is guarded with `#ifdef BOARD_CARDPUTER`.

The build also produces a **`firmware-merged.bin`** (via
`scripts/merge_bin.py`) suitable for the web flasher — that file is what
gets attached to GitHub releases as `a1keyer-cardputer-vX.Y.Z-merged.bin`.

---

## Documentation

| Document | What's in it |
|---|---|
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