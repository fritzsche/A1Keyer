# Winkey 2.x Protocol & Emulator Architecture

The K1EL WK2 serial protocol as implemented by the A1Keyer `WinkeyBridge`.
Canonical reference for the host side, the firmware side, and the test
harness.

> **Status.** The command byte codes and the command-vs-text model in
> this document were CORRECTED on 2026-08-03 against the K3NG reference
> implementation (`k3ng_cw_keyer/k3ng_keyer/k3ng_keyer.ino`,
> `service_winkey()`), read directly from source. An earlier revision of
> this file used **fabricated ASCII-letter command codes** (`'S'`=speed,
> `'A'`=sidetone, `'L'`/`'P'` load/play) attributed to a non-existent
> "K3NG convention" — those were wrong and are replaced below.
>
> **The real protocol:** command bytes are **0x00–0x1F**; bytes **≥ 0x20
> are text sent as CW immediately** (§ 4.3, § 7). The version byte on
> host-open is a revision number (**0x17** = WK2 rev 2.3); hosts accept
> ≥ 0x10 as "a WinKeyer". Some parameter encodings (sidetone presets,
> mode/pinconfig bit-fields, load-defaults) still warrant a pass against
> the K1EL WK2 datasheet v23 and are tagged `[verify]`.

---

## Contents

1. [Overview](#1-overview)
2. [Scope of this document](#2-scope-of-this-document)
3. [History of the K1EL Winkeyer family](#3-history-of-the-k1el-winkeyer-family)
4. [Physical and transport layer](#4-physical-and-transport-layer)
5. [Host open sequence](#5-host-open-sequence)
6. [Admin mode (register-file read/write)](#6-admin-mode-register-file-readwrite)
7. [Operating commands — the real byte map](#7-operating-commands--the-real-byte-map)
8. *(merged into § 7)*
9. *(merged into § 7)*
10. *(merged into § 7)*
11. *(merged into § 7)*
12. [Status byte format](#12-status-byte-format)
13. [Send buffer (text, backspace, clear)](#13-memory-buffer-load--unload--play--pause--resume)
14. [Pin and event reporting](#14-pin-and-event-reporting)
15. [Differences between WK1, WK2, and WK3](#15-differences-between-wk1-wk2-and-wk3)
16. [Winkey emulator architecture in A1Keyer](#16-winkey-emulator-architecture-in-a1keyer)
17. [Test strategy and the host-side test utility](#17-test-strategy-and-the-host-side-test-utility)
18. [Open questions / future work](#18-open-questions--future-work)
19. [References](#19-references)

---

## 1. Overview

The K1EL **Winkeyer** (often written "WinKey" or "WK") is a serial-controlled
Morse keyer chip family designed by Steven Dimse, K1EL, and used in amateur
radio for computer-controlled CW (continuous-wave Morse) keying. The
original chip — **WK1** — appeared in the early 2000s as a small PIC-based
keyer that exposed a paddle, an audio sidetone, and a key-down output. The
**WK2** (2004) added a memory buffer for canned messages, Farnsworth
spacing, an admin-mode register file, and an extended status byte.
**WK3** (2018) added USB and a handful of new commands. Today, virtually
every contest logger (N1MM Logger+, Win-Test, WriteLog), every CW skimmer
client, and most general-purpose Morse programs (fldigi, CW Skimmer Server)
speak at least the WK2 dialect of the protocol.

The protocol itself is intentionally tiny: a **single UART** carries
**one-byte ASCII-letter commands** from the host to the chip and
**unsolicited status bytes** from the chip back. There are no packet
delimiters, no length prefixes, no CRCs. Host and chip agree on a small
state machine (operating mode vs. admin mode) and exchange byte sequences
that are documented exhaustively in §§ 5-14 below.

A1Keyer's reason for caring: the same byte stream that drives a real
Winkeyer chip can drive **an emulator inside the firmware**. Once a
`WinkeyBridge` class parses the protocol and translates it into calls on
the existing `KeyEventBus`, every N1MM-class host that already supports
WinKeyer "just works" with A1Keyer — without any A1Keyer-specific host
software. This is the integration seam this document exists to specify.
The architecture is detailed in § 16.

---

## 2. Scope of this document

**In scope.**

- The WK2 baseline byte-level protocol: commands, status byte, register
  file, host-open sequence, memory buffer, pin events.
- The A1Keyer WinkeyBridge architecture: class layout, threading model,
  integration with `KeyEventBus`, `MorseGenerator`, `RadioKeyer`,
  `MorseModel`, the command dispatch table, and the state-propagation
  rules that govern how a host-issued byte becomes a sound, a bus event,
  or a GPIO edge.
- The test strategy: native C++ unit tests, the host-side Python test
  utility, and a manual validation flow.

**Out of scope.**

- USB-CDC / USB-HID **class-level** descriptors (the bridge treats USB
  as a stream-of-bytes pipe; the OS-side CDC or HID class stack is the
  host's concern).
- The K1EL firmware bootloader.
- RF-chain details, contest scoring, log file formats.

**Baseline.** WK2 unless explicitly noted. Where WK1 or WK3 differ, that
is called out in § 15. Byte-level defaults and parameter ranges are taken
from the K1EL WK2 datasheet; the K3NG `k3ng_winkey.cpp` implementation
serves as a sanity check; `hamlib/rig/winkey.c` is the third independent
reference. Where the three sources disagree, the conservative WK2 value
wins and a footnote marks the conflict.

**Status of implementation.** A1Keyer's `WinkeyBridge` is **not yet
implemented** at the time of writing. This document is the *spec* the
implementation will follow. The integration seams it will plug into are:

- `src/key_event_bus.{h,cpp}` — the central reference-counted dispatcher
  (`src/key_event_bus.h:27-69`).
- `src/morse_generator.cpp:10-14` — the explicit TODO marking where
  buffer-playback should call `KeyEventBus::keyDown()` / `keyUp()` at
  every dit/dah boundary so the PC817 radio line follows stored text.
- `docs/keyer.md § 6` — the existing producer/consumer ASCII diagram
  has a `(future)` placeholder for `WinkeyBridge`; § 16 of this document
  replaces that placeholder with the real architecture.

---

## 3. History of the K1EL Winkeyer family

### 3.1 WK1 — original PIC16F84 chip (~2001)

The first Winkeyer was a small 8-pin PIC with paddle input, sidetone,
and a key-down output. The protocol was deliberately minimal: a few
single-letter commands for speed and sidetone, no memory buffer, no
Farnsworth, no admin mode. The version byte returned on host-open is
`0x05`. WK1 is largely obsolete today; few modern hosts even check for
it, but the byte is still in the spec because older firmware may
encounter it.

### 3.2 WK2 — extended keyer (~2004)

WK2 is the chip (and the protocol dialect) that the entire amateur-radio
ecosystem settled on. It adds:

- **Memory buffer** for canned messages (about 110 characters; see
  § 13).
- **Farnsworth spacing** (§ 8.2).
- **Hang time** to avoid repeated PTT toggling on brief gaps (§ 9.3).
- **Admin mode** with a 16-register file for advanced scripting (§ 6).
- **Sidetone frequency** command (the WK1 sidetone was a fixed audio
  frequency; WK2 makes it programmable, § 7.3).
- **Paddle echo** so the host can mirror what the operator is doing
  (§ 10.3).
- **Extended status byte** that includes the busy/overflow bit, the
  speed-pot-changed bit, and the pushbutton bit (§ 12).

The version byte is `0x06`. **This is the baseline A1Keyer emulates.**

### 3.3 WK3 — USB variant (~2018)

WK3 is a WK2 + USB-HID-or-USB-CDC interface. Byte-level commands are a
superset of WK2; a WK3 chip dropped into a WK2 host works. Additions:

- **USB interface** (HID or CDC), so the chip appears as a USB device
  without an external USB-to-serial bridge.
- **Configurable echo** (WK2 always echoes; WK3 can be told to echo or
  not).
- **Higher max speed** (up to 100 WPM; WK2 is 50).
- A few small commands: serial-number query, auto-send mode.

The version byte is `0x07`.

### 3.4 Variants and clones

| Variant | Source | Notes |
|---|---|---|
| WKUSB | K1EL | WK2 + FTDI USB bridge in the same module |
| K3NG `k3ng_winkey.cpp` | github.com/k3ng/k3ng_arduino_keyer | Open-source Arduino emulation. **The reference implementation A1Keyer's bridge is modelled on.** |
| PicoKeyer | ham radio hobbyists | Teensy-based, WK2-compatible |
| OpenInterface | N0XAS et al. | Adds a few extension bytes for non-standard keys |
| WK-mini / WK-nano | K1EL | Smaller-footprint WK2 modules |

| Feature | WK1 | WK2 | WK3 |
|---|---|---|---|
| Memory buffer | no | ≥ 110 chars | 160 chars |
| Admin mode | no | yes (21-entry set, 0x00-0x14) | yes |
| Sidetone freq command | no | yes | yes |
| Farnsworth | no | yes | yes |
| Paddle echo | no | yes | yes |
| Hang time | no | yes | yes |
| USB | no | via WKUSB | yes |
| Max WPM | 40 | 50 | 100 |
| Version byte reply | `0x05` | `0x06` | `0x07` |
| Status byte richness | partial | full (3-MSB=110) | full (3-MSB=110) |

---

## 4. Physical and transport layer

### 4.1 UART parameters

| Parameter | Value | Notes |
|---|---|---|
| Baud (default) | `[unverified]` | Default baud rate after power-up is not documented in the WK2 datasheet v23 § 4. Admin commands 17/18 (0x11/0x12) are `Set High Baud` and `Set Low Baud` respectively (see § 6.1); the byte values they accept are datasheet-specified. Hosts should consult the WK2 v23 datasheet before assuming a baud. |
| Data bits | 8 | |
| Parity | none | |
| Stop bits | 1 | |
| Flow control | none | "XOFF" in the protocol is a **software** flow-control byte (bit 0 of the status byte, set when the input buffer is > 2/3 full), not RTS/CTS. |

### 4.2 Echo behavior

The chip echoes each **sent text character** back to the host — but
crucially it echoes it **after** the character has been keyed as CW, not
when the byte is received, and it does **not** echo command or parameter
bytes. Hosts use this echo to track send progress.

| Chip | Character echo |
|---|---|
| WK1 | off |
| WK2 | on (text chars, after keying) |
| WK3 | on (configurable) |

A1Keyer's bridge emulates WK2 echo: the device glue echoes each sent
character (the earlier "echoes every byte including commands" model was
wrong — commands/params are silent, only text is echoed).

### 4.3 Wire-level framing

There is no packet framing — but there IS a byte-value split that
determines how each received byte is interpreted:

- **0x00 – 0x1F → command bytes.** `0x00` is the admin prefix (a second
  byte selects the admin sub-command). Every other value in this range
  is an operating command; each consumes a fixed number of parameter
  bytes that follow it (see § 7).
- **0x20 – 0x7F → text.** Printable ASCII is appended to the send buffer
  and keyed as CW **immediately**. Lowercase is upper-cased; `|` (0x7C)
  is a half word-space. There is no "load then play" step for normal
  sending — the host just streams the text.

Chip → host: the version byte (on host-open), status bytes (3-MSB tag
`110`, § 12), and a character echo (§ 4.2) are sent unsolicited, one
byte at a time, no delimiters.

This range split is the whole disambiguation mechanism: a message like
`CQ TEST` streams as raw ASCII (all bytes ≥ 0x41) and can never collide
with the command space (all ≤ 0x1F).

### 4.4 Transport in A1Keyer

The WinkeyBridge is **transport-independent**: it sees a stream of bytes
and writes a stream of bytes, and the transport underneath (USB-CDC,
BLE-NUS) is a separate concern. A1Keyer's two targets:

- **Cardputer ADV (ESP32-S3).** Primary target. USB-CDC for development
  (`ARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini`); BLE-NUS for
  field use (ESP32-S3 has no Bluetooth Classic, only BLE; see
  `docs/ARCHITECTURE.md § 2`).
- **Tab5 (ESP32-P4).** Compiles-only target. Same transports.

```
┌──────────┐    ┌──────────────────┐    ┌──────────────────┐
│ Host     │    │  Winkey bytes    │    │  A1Keyer         │
│ logger   │◄──►│  (transparent)   │◄──►│  WinkeyBridge    │
│ (N1MM)   │    │                  │    │                  │
└──────────┘    └──────────────────┘    └──────────────────┘
       ▲                ▲                       ▲
       │                │                       │
   USB-CDC          BLE-NUS                KeyEventBus
   /dev/tty.USB0    GATT char              (ref-counted)
   COM5             notify/write           radio + sidetone
```

The byte-level Winkey protocol is **unchanged** regardless of transport;
BLE/USB wrappers are just stream-of-bytes pipes.

---

## 5. Host open sequence

Every host must perform an opening handshake before sending operating-
mode commands. The bridge rejects operating commands (`'S'`, `'V'`, …)
until it has been opened.

### 5.1 The handshake — three forms

The host-open mechanism is built on top of the **admin command** space
(§ 6). Every admin command begins with the prefix byte `0x00`. Host Open
is admin command `0x02`. So the byte sequence is:

| Form | Bytes (host → bridge) | Expected reply | Meaning |
|---|---|---|---|
| **Host Open** | `0x00 0x02` | one byte: version (`0x05`/`0x06`/`0x07`) | Open the host interface; enter operating mode. After open, WK1 mode is set by default. |
| **Enter WK2 mode** | (admin `0x0B`) | one byte | Switch to WK2-mode status bytes (§ 12.2) and other WK2-only behaviour. Issued **after** host-open. |
| **Soft reset** | `0x00 0x01` | `0x00` | Admin command 1 — Reset. Returns to defaults. Host must re-open. |
| **Host Close** | `0x00 0x03` | one byte | Admin command 3 — Close the host interface. |

> **Verified against [WK2 datasheet v23 § 4](https://hamcrafters2.com/files/WK2_Datasheet_v23.pdf), page 5.**
> Quote: *"Upon power-up, Winkeyer2 initializes with the host mode turned
> off. To enable host mode, the PC host must issue the admin:open
> command. Upon open, Winkeyer2 will respond by sending the revision
> code back to the host. The host must wait for this return code before
> any other commands or data can be sent to Winkeyer2. Upon open, WK1
> mode is set."*
> Admin command `0x02` = Host Open. The same byte sequence is encoded
> in `hamlib/rig/winkey.c` and `K3NG/k3ng_winkey.cpp`.

```
// Host Open (operating mode, WK1 status bytes)
host  →  0x00 0x02
bridge ←  0x06          // WK2 revision code

// Switch to WK2 status byte format
host  →  0x00 0x0B
bridge ←  0x06          // admin command echo

// Soft reset
host  →  0x00 0x01
bridge ←  0x00
```

### 5.2 A1Keyer-specific

- The bridge answers `0x06` (WK2) regardless of A1Keyer firmware version.
- A WK3-aware host (WriteLog with WK3 support, hamlib's winkey driver)
  that sends the version query and receives `0x06` will fall back to
  WK2 behaviour — which is what A1Keyer actually implements. This is
  the same compromise K3NG's bridge makes (see § 16.6 and the
  `OPTION_WINKEY_*` flags in `k3ng_cw_keyer/k3ng_keyer/keyer_features_and_options.h`).
- The bridge echoes the open bytes first, then sends the version reply
  (matches K3NG behaviour; some hosts rely on this order).

---

## 6. Admin mode (command set, not a register file)

Admin mode is an alternative to operating mode: instead of single-letter
commands, the host issues numbered admin commands, each prefixed by
`0x00`. **Important correction:** the WK2 datasheet does not describe
a "16-register file" in the host-facing protocol — it describes a
**21-entry admin command set** numbered `0x00` through `0x14` (decimal
0-20). The earlier draft's "16-register file" terminology conflated the
admin command set with the Load Defaults parameter list. The 21
admin commands are:

### 6.1 Admin command enumeration

| Cmd | Name | Notes |
|---|---|---|
| 0 | Calibrate | Calibration routine |
| 1 | Reset | Soft reset; restores defaults |
| 2 | Host Open | Required handshake to begin host mode (§ 5.1) |
| 3 | Host Close | End host mode |
| 4 | Echo Test | Loopback test |
| 5 | Paddle A2D | Read paddle A/D |
| 6 | Speed A2D | Read speed-pot A/D |
| 7 | Get Values | Read all current settings |
| 8 | Reserved | |
| 9 | Get Cal | Read calibration values |
| 10 | Set WK1 Mode | Switch to WK1-mode status bytes (§ 12.2) |
| 11 | Set WK2 Mode | Switch to WK2-mode status bytes (§ 12.2) |
| 12 | Dump EEPROM | Dump EEPROM contents |
| 13 | Load EEPROM | Restore EEPROM contents |
| 14 | Send Standalone Message | Transmit a stored standalone message |
| 15 | Load XMODE | Load extended-mode parameters |
| 16 | Reserved | |
| 17 | Set High Baud | Set the higher of two user-selectable baud rates |
| 18 | Set Low Baud | Set the lower of two user-selectable baud rates |
| 19 | Reserved | |
| 20 | Reserved | |

> **Verified against [WK2 datasheet v23](https://hamcrafters2.com/files/WK2_Datasheet_v23.pdf), § 4, page 5:**
> *"Admin Command `<00><nn>` nn is a value from 0 to 20."*
> Full enumeration from datasheet pages 5-9 and Table 14 (page 18).

### 6.2 Get Values (admin 7) payload format

The `Get Values` reply is the closest thing to a register dump in the
host-facing protocol: it returns the current state of every settable
parameter (speed, sidetone, weight, Farnsworth, PTT lead/tail, hang
time, output enable, paddle swap, etc.). The exact byte count and
encoding are detailed in WK2 datasheet v23 Table 14 (page 18) and are
the canonical reference for any host that wants to mirror chip state.

### 6.3 Defaults at power-up

Defaults are documented in WK2 datasheet v23 Table 3 (page 7). The most
relevant subset:

- Speed = 20 WPM
- Sidetone = 600 Hz
- Weight = 50 (= 100% of nominal)
- Farnsworth WPM = 0 (off)
- PTT lead = 50 ms (admin 5 units)
- PTT tail = 50 ms
- Hang time = 0 (off)
- Keyer mode = iambic B
- Output enable = 1
- Status byte mode = WK1 (until admin 11 is issued after host-open)

A soft reset (§ 5.1, `0x00 0x01`) restores every value to those defaults.

---

## 7. Operating commands — the real byte map

Command bytes are **0x00–0x1F**. Values below are from the K3NG
`service_winkey()` switch (verified by reading the source). Text
(≥ 0x20) is not a command — see § 4.3.

| Byte | Command | Params | A1Keyer handling |
|---|---|---|---|
| `0x00` | Admin prefix | +1 (sub-cmd) | See § 6 (host-open/close/reset/WK1-WK2 mode). |
| `0x01` | Sidetone control | 1 | Low nibble 1–10 selects a preset frequency; bit 7 = paddle-only. Mapped to Hz and sent to `AudioEngine::setToneFrequency()` (A1Keyer clamps to [300,900]). `[verify]` exact preset table. |
| `0x02` | Set speed (WPM) | 1 | `0` = use pot (ignored — no pot). Else clamp 5–99 → `MorseModel::setWPM()` (clamps [5,50]). |
| `0x03` | Weighting | 1 | Stored for status readback (no audio effect yet — § 16.6). |
| `0x04` | PTT lead/tail | 2 | Stored (lead, tail). |
| `0x05` | Set speed pot | 3 | Consumed; no physical pot. |
| `0x06` | Pause | 1 | Accepted; PTT/hang timing deferred (§ 16.6). |
| `0x07` | Get speed pot | 0 | Replies one byte (top bit set). |
| `0x08` | Backspace | 0 | Removes the last un-sent char from the send buffer. |
| `0x09` | Pin config | 1 | Stored. |
| `0x0A` | Clear buffer | 0 | Clears the send buffer + stops sending. |
| `0x0B` | Key immediate (tune) | 1 | 1 = key down, 0 = up → `RadioKeyer` (gated by operator KEYING, § 16.4 r3). |
| `0x0C` | HSCW | 1 | Consumed; not implemented. |
| `0x0D` | Farnsworth | 1 | Stored (§ 16.6). |
| `0x0E` | Set keyer mode | 1 | Stored (iambic A/B, ultimatic, bug, paddle-swap bits). `[verify]` bit layout. |
| `0x0F` | Load defaults | 15 | All 15 bytes consumed; not applied. |
| `0x10` | First extension | 1 | Consumed. |
| `0x11` | Key compensation | 1 | Consumed. |
| `0x12` | (reserved) | 1 | Consumed. |
| `0x13` | Null | 0 | No-op. |
| `0x14` | Software paddle | 1 | Consumed. |
| `0x15` | Request status | 0 | Replies the status byte (§ 12). |
| `0x16` | Pointer op | 1 | Consumed. |
| `0x17` | Dit/dah ratio | 1 | Stored (§ 16.6). |
| `0x18`–`0x1F` | Buffered commands | varies | Buffered PTT (0x18), key (0x19), wait (0x1A), merge (0x1B), buffered speed (0x1C), buffered HSCW (0x1D), cancel-speed (0x1E, 0 params), NOP (0x1F, 0 params). Accepted; buffered timing deferred. |

Example — set 25 WPM, sidetone preset 5, then send "CQ TEST":
```
host  →  0x02 0x19          // speed = 25 WPM
host  →  0x01 0x05          // sidetone preset 5
host  →  'C' 'Q' ' ' 'T' 'E' 'S' 'T'   // text (0x20+) → keyed as CW
```

Note how the letters `S` (0x53) and `T` (0x54) in "TEST" are **text**,
not the (nonexistent) speed/PTT commands — this is the byte-range split
of § 4.3. A1Keyer's default sidetone is 600 Hz (`src/audio_engine.cpp`).

### 7.1 Commands A1Keyer acts on vs. stores

- **Acts on:** `0x01` sidetone, `0x02` speed, `0x08` backspace,
  `0x0A` clear, `0x0B` key-immediate, `0x15` status, plus all text
  (≥ 0x20) → CW.
- **Stores for status readback only** (no audio effect yet, § 16.6):
  weighting, PTT times, Farnsworth, keyer mode, dit/dah ratio.
- **Consumes and ignores:** pot, pin-config, HSCW, extensions, key-comp,
  load-defaults, software paddle, pointer, buffered commands.

### 7.2 Output enable and the operator's KEYING setting

There is no standalone "output enable" byte in the real protocol (the
old doc's `E`=0x45 was fabricated). Host keying still cannot override the
operator: any host key-down (`0x0B`) routes through
`RadioKeyer::setEnabled()` (`src/radio_keyer.cpp`), which is AND-ed with
the operator's KEYING toggle — GPIO stays LOW until the operator also
enables keying locally. (See § 16.4 rule 3.)

---

## 12. Status byte format

The bridge sends **unsolicited** status bytes to the host whenever its
internal state changes. This is how the host knows the buffer is busy,
break-in is active, the speed pot moved, etc.

### 12.1 When status bytes are sent

- After a host command that changes state.
- When the bridge itself transitions (e.g. buffer > 2/3 full, buffer
  drained).
- On edge events (paddle press if paddle-echo is on, speed-pot change,
  pushbutton event).

### 12.2 Bit definitions

The **three MSBs of the status byte are always `110`** (bits 7-5),
yielding a byte in the range `0xC0`-`0xDF`. **Bit 3** is repurposed
depending on whether WK1 or WK2 mode is active:

**WK2 mode** (active after the host issues admin `0x0B` post-open, § 5.1):

| Bit | Mask | Name | Meaning |
|---|---|---|---|
| 7-5 | `0xE0` | (tag) | Always `110` (`0xC0`); identifies byte as a WK status byte |
| 4 | `0x10` | WAIT | Buffer > 2/3 full — host should pause sending |
| 3 | `0x08` | PUSHBUTTON | When set, this byte is a **pushbutton status byte** (see § 14.2), not a regular status byte |
| 2 | `0x04` | BUSY | Buffer busy / XOFF (input buffer full) |
| 1 | `0x02` | BREAK-IN | Paddle is being squeezed (iambic B opposite memory set) |
| 0 | `0x01` | XOFF | Input buffer > 2/3 full — host should pause sending |

**WK1 mode** (default after host-open, until admin `0x0B` is issued):
bit 3 means KEYDOWN instead of being a pushbutton discriminator.
The other bits carry the same meanings.

```
       ┌─────────────────────────────────────────┐
       │        Status byte (8 bits)             │
       │   7   6   5   4   3   2   1   0        │
       │  1   1   0  WAIT  *  BUSY BKIN XOFF    │
       └─────────────────────────────────────────┘
                          ▲     ▲    ▲    ▲
                          │     │    │    └─ XOFF (buffer > 2/3 full)
                          │     │    └─ BREAK-IN
                          │     └─ BUSY
                          └─ bit 3 = PUSHBUTTON discriminator in WK2,
                             KEYDOWN in WK1
```

> **Verified against [WK2 datasheet v23](https://hamcrafters2.com/files/WK2_Datasheet_v23.pdf), page 13, Tables 10/11/12:**
> *"The three MSBs of the status byte are always 110. Note that in WK2
> mode bit 3 identifies the status byte as a pushbutton status byte.
> WK2 mode is set by the ADMIN command 11 before WK is opened."*

### 12.3 Implementation in A1Keyer

- **BREAK-IN** ← derived from iambic squeeze detection in `IambicKeyer`
  (`src/iambic_keyer.cpp:240-301`). Surfaces to the host only when WK2
  status mode is active.
- **BUSY / WAIT / XOFF** ← derived from `WinkeyBridge::_buffer`
  occupancy and `KeyEventBus::demand()` (`src/key_event_bus.h:65`,
  `src/key_event_bus.cpp:131-133`).
- **PUSHBUTTON** ← synthesised from `RadioKeyer::setEnabled()`
  transitions (§ 14.2).
- **KEYDOWN (WK1 mode)** ← `KeyEventBus::demand() > 0`.

A1Keyer's bridge will operate in **WK2 mode** by default (issue admin
`0x0B` after host-open), since the PUSHBUTTON discriminator and the
explicit BREAK-IN bit are useful to hosts (N1MM in particular).

### 12.4 Status-byte coalescing

Multiple events in the same tick produce **only one** status byte per
host poll cycle. The bridge maintains a `_pendingStatus` atomic byte
and sends it at most once per `loop()` iteration. This prevents a busy
paddle from saturating the serial port with status bytes.

---

## 13. Memory buffer (load / unload / play / pause / resume)

The WK memory buffer is an ASCII text-storage FIFO that the host loads
text into, then commands playback. This is the primary contest-messaging
mechanism.

### 13.1 Buffer capacity `[unverified]`

The exact capacity of the WK2 input buffer is not pinned down in the
datasheet v23. Cross-references suggest:

- **WK2**: ≥ 110 characters (the most-cited value across implementations).
- **WK3**: 160 characters (one K3NG implementation note).
- **XOFF** is asserted when the input buffer passes 2/3 full
  (regardless of absolute size).

The host should treat the buffer as opaque and rely on the XOFF status
bit (§ 12.2 bit 0) to throttle sending.

### 13.2 Commands

**Correction:** the earlier `L`/`U`/`P` (0x4C/0x55/0x50) "load buffer /
play slot" table was fabricated — those are printable ASCII letters and
would be *sent as CW*, not interpreted as commands. In the real
protocol there is no host-visible load/play framing for the normal send
path: the host simply streams text (bytes ≥ 0x20) and the chip keys it
immediately (§ 4.3, § 7). The relevant real commands that touch the send
buffer are:

| Byte | Command | Notes |
|---|---|---|
| (≥ 0x20) | Send text | Appended to the send buffer, keyed as CW at once. |
| `0x08` | Backspace | Remove the last un-sent character. |
| `0x0A` | Clear buffer | Discard everything not yet sent; stop. |
| `0x06` | Pause | `[verify]` pause/resume of the send buffer. |

A1Keyer's `WinkeyBuffer` (`src/winkey_buffer.h`) is a FIFO of pending
characters; `WinkeyBridge::poll()` drains it to `MorseGenerator`.

### 13.3 Prosigns `[verify]`

Prosigns (`<SK>`, `<BK>`, `<KN>`, `<AR>`, `<AS>`, `<VE>`, `<HH>`,
`<INT>`, `<RR>`) are sent as ASCII text; K1EL uses the `merge` command
(0x1B) or a `\` between two letters to bond them into one symbol:

```
host  →  'C' 'Q' ' '                  // text, keyed immediately
host  →  0x1B 'A' 'R'                 // merge: <AR> as one symbol
```

A1Keyer's `MorseEncoder` (`src/morse_encoder.cpp`) does **not** currently
expand prosigns. The bridge converts `<SK>`, `<AR>`, `<KN>`, `<BK>` to
their equivalent element sequences at playback time (see § 18.4).
K3NG exposes this behaviour behind the `OPTION_WINKEY_PROSIGN_COMPATIBILITY`
build flag in `keyer_features_and_options.h`.

### 13.4 Pause / resume semantics

Pause (`B 0`) preserves the current buffer position. A subsequent
`B 1` resumes from that position. A subsequent `P 0` restarts from
position 0.

### 13.5 How A1Keyer plays buffer contents

The bridge hands the buffer to `MorseGenerator::playText()`
(`src/morse_generator.cpp:67-85`). The generator emits audio as before.

**Future TODO at `src/morse_generator.cpp:10-14`:** the generator must
additionally call `KeyEventBus::keyDown()` / `keyUp()` at each dit/dah
boundary so the PC817 radio line follows playback. Until that hook is
added, the bridge's buffer-playback path is **sidetone-only**: audio
plays through the NS4168 amp but GPIO4 stays LOW.

A critical distinction between the bridge and the existing
`MorseGenerator`: the existing player (used by the `P` key for the
"Hello Morse!" demo) emits audio but does NOT touch `KeyEventBus`, so
the PC817 line stays LOW during demo playback. The bridge's buffer-play
path must add `KeyEventBus::keyDown()` / `keyUp()` at each element
boundary. The TODO at `morse_generator.cpp:10-14` is exactly this hook
point — adding it there makes BOTH the demo keying AND the bridge's
buffer keying work at once.

---

## 14. Pin and event reporting

Beyond the 8-bit status byte (§ 12), the bridge can send **pin-event
bytes** for things like speed-pot value changes, pushbutton transitions,
and paddle edges. Hosts (especially CW skimmers) use these.

> **Caveat:** the specific prefix bytes (`0xF0`, `0xF1`, `0xF2`) used
> below are a *common convention* but are not verified against the
> WK2 datasheet v23 — that document treats pushbutton and speed-pot
> events via the status byte mechanism (§ 12), not via prefixed bytes.
> A1Keyer's bridge uses these prefixes for **K3NG compatibility**
> (`OPTION_WINKEY_SEND_BREAKIN_STATUS_BYTE` and friends in
> `keyer_features_and_options.h`); hosts should treat any prefixed
> byte as a K3NG-specific extension.

### 14.1 Speed-pot pin event

When the host has enabled `J 1` (§ 10.3) and the bridge detects a
speed-pot change, the K3NG convention emits a 2-byte sequence:
prefix `0xF0` + value 0-255.

```
bridge ←  0xF0 0xB4        // prefix 0xF0, value 180
```

A1Keyer has **no physical speed pot** (the Cardputer ADV has no
potentiometer). The bridge accepts the `V` command and converts linearly
to WPM between min and max WPM set via the `S` command or admin
parameters.

### 14.2 Pushbutton pin event

In WK2 status-byte mode (§ 12.2), pushbutton events are reported by
setting **bit 3** of the status byte. A1Keyer has **no physical
pushbutton** on the Cardputer that maps to this. The bridge synthesises
pushbutton events — for example, when the operator toggles KEYING via
`;` in the KEYING settings overlay (`docs/keyer.md § 5`). Implementation:
the bridge intercepts `RadioKeyer::setEnabled()` calls
(`src/radio_keyer.cpp:85-99`) and emits a status byte with bit 3 set on
each transition.

For K3NG compatibility, the bridge may also emit `0xF1 <0|1>` on a
synthetic event:

```
bridge ←  0xF1 0x01        // prefix 0xF1, pressed (K3NG convention)
```

### 14.3 Paddle-echo pin event

When `J 1`, the bridge sends a paddle-event byte on every paddle edge.
The K3NG convention is `0xF2` + a 4-bit nibble encoding dit-state /
dah-state / squeeze:

```
bridge ←  0xF2 0x01        // prefix 0xF2, dit set (K3NG convention)
```

Implementation: a small subscriber to `KeyEventBus` for the bridge,
tapping the existing `IambicKeyer::startElement()` callsite
(`src/iambic_keyer.cpp:118`).

### 14.4 Parser note

Pin-event bytes are emitted **in addition to** the status byte (§ 12).
WK2 hosts parse the status byte (which has the 3-MSB tag `110`); K3NG-
aware hosts may also parse on the high-bit prefix `0xF0..0xFF`.

---

## 15. Differences between WK1, WK2, and WK3

| Feature | WK1 | WK2 | WK3 |
|---|---|---|---|
| Memory buffer | no | ≥ 110 chars | 160 chars |
| Admin mode | no | yes (21-entry set, 0x00-0x14) | yes |
| Sidetone freq command | no | yes (`A`) | yes |
| Farnsworth | no | yes (`N`) | yes |
| Paddle echo | no | yes (`J`) | yes |
| Hang time | no | yes (`H`) | yes |
| USB | no | via WKUSB | yes |
| Max WPM | 40 | 50 | 100 |
| Version byte reply | `0x05` | `0x06` | `0x07` |
| Status byte (8 bits) | partial | full | full |
| Configurable echo | n/a | n/a | yes (admin reg 1) |

### 15.1 A1Keyer's policy

- **Baseline:** WK2. The bridge answers `0x06` on host-open.
- **WK3-aware hosts:** they may downgrade to WK2 behaviour upon seeing
  `0x06`. That is fine — the bridge implements the WK2 subset.
- **WK1 hosts:** they expect `0x05`. If a host *requires* the WK1
  version byte (which few do today), the bridge can be reconfigured
  to answer `0x05`; this is a build-time option, not runtime.

---

## 16. Winkey emulator architecture in A1Keyer

This section is the implementation-narrative core of the document. It
describes how a future `WinkeyBridge` producer plugs into A1Keyer's
existing `KeyEventBus` architecture, and what files / lines / rules are
involved.

### 16.1 Class layout

A new module `src/winkey_bridge.{h,cpp}` and a few supporting classes:

```
src/winkey_bridge.h           — public API: begin(), feed(byte), poll(), statusByte()
src/winkey_bridge.cpp         — command dispatch table, state machine
src/winkey_buffer.h           — memory buffer (load/play/pause/resume)
src/winkey_register_file.h    — admin-mode 16-register file
src/winkey_serial.h           — abstract transport interface (USB-CDC, BLE-NUS)
src/winkey_serial_usb.cpp     — USB-CDC transport (Cardputer dev)
src/winkey_serial_ble.cpp     — BLE-NUS transport (Cardputer field)
scripts/winkey_host.py        — host-side test utility (Python; § 17)
```

A1Keyer's codebase follows the **one-class-per-file** rule (see
`docs/ARCHITECTURE.md § 1` lines 53-76 — the repository layout table).
The transport abstraction lets the same protocol logic run on USB during
development and BLE in the field.

### 16.2 Threading model

The bridge runs on **Core 0** in `loop()` (alongside `handleKeyboard()`
and the decoder drain at `src/main.cpp:367-405`). Bytes arrive via the
transport's ISR → ring buffer → `feed()` is called in `loop()`. The
bridge mutates `MorseModel` and the audio chain; those have their own
atomic contracts.

```
┌──────────────────────────────────────────────────────────────────┐
│                       Host (contest logger)                       │
└────────────────────────────────┬─────────────────────────────────┘
                                 │ USB-CDC or BLE-NUS
                                 │ (raw Winkey bytes)
                                 ▼
┌──────────────────────────────────────────────────────────────────┐
│ src/winkey_serial.{h,cpp}    ISR → ring buffer → feed()          │
│ src/winkey_bridge.cpp        command dispatch                     │
│       │                                                           │
│       ├──► MorseModel::setWPM / setFrequency / setVolume          │
│       │         │                                                  │
│       │         └──► AudioEngine::keyer()->setWPM()               │
│       │              AudioEngine::setToneFrequency()              │
│       │              AudioEngine::morseGen()->setWPM()            │
│       │                                                           │
│       ├──► MorseGenerator::playText()   (for buffer playback)    │
│       │         │                                                  │
│       │         └──► [TODO] KeyEventBus::keyDown / keyUp          │
│       │              at each dit/dah boundary                    │
│       │              (src/morse_generator.cpp:10-14)             │
│       │                                                           │
│       └──► WinkeyBridge state (registers, buffer, current cmd)   │
└──────────────────────────────────────────────────────────────────┘
                                 │ (status bytes, pin events)
                                 ▼
┌──────────────────────────────────────────────────────────────────┐
│ src/winkey_serial.{h,cpp}    transport → host                     │
└──────────────────────────────────────────────────────────────────┘

(Central bus on the firmware side:)

         ┌────────────────────┐
         │   IambicKeyer      │
         │   StraightKeyer    │  producers (call keyDown / keyUp)
         │   MorseGenerator   │
         │   WinkeyBridge     │  ← NEW (§ 16)
         └─────────┬──────────┘
                   │
                   ▼
         ┌────────────────────┐
         │   KeyEventBus      │  ref-counted 0→1 / 1→0 edges
         │   src/key_event_bus│
         └─────────┬──────────┘
                   │
                   ▼
         ┌────────────────────┐
         │   RadioKeyer       │  sinks (subscribe once)
         │   src/radio_keyer  │
         │   ↳ GPIO4 (PC817)  │
         └────────────────────┘
```

This diagram is an *extended version* of the one in `docs/keyer.md § 6`
lines 282-301. The existing diagram already has `WinkeyBridge (future)`
as a placeholder; this doc replaces that placeholder with the real
architecture.

### 16.3 Command dispatch

The bridge dispatches on the byte-value split (§ 4.3): text (≥ 0x20) is
accumulated in the send buffer; command bytes (0x00–0x1F) switch on the
real WK codes (§ 7). The implementation (`src/winkey_bridge.cpp`) uses a
`switch` with a `paramCountFor()` helper that returns how many parameter
bytes each command consumes, so the parser knows when a command is
complete:

```cpp
// feed(byte):
//   0x00        -> admin (next byte = sub-command)
//   0x01..0x1F  -> command; collect paramCountFor(cmd) param bytes
//   >= 0x20     -> text: buffer.push(toupper(byte))  (send as CW)
//
// Real command codes (see § 7): 0x01 sidetone, 0x02 speed, 0x03 weight,
// 0x04 PTT times, 0x08 backspace, 0x0A clear, 0x0B key, 0x0D Farnsworth,
// 0x0E mode, 0x15 status, 0x17 ratio, 0x18-0x1F buffered.
```

The earlier version of this section listed ASCII-letter codes
(`'S'`, `'A'`, `'L'`, …) — those were the fabricated encoding and do not
exist in the real protocol.

### 16.4 State propagation rules — the critical rules

These six rules govern how bridge state maps to A1Keyer state. They are
the things that will trip up an implementer who hasn't read this
section.

1. **Speed (`S`, `V`) goes through `MorseModel::setWPM()`**
   (`src/display_model.cpp:118-128`), not directly to `AudioEngine`. The
   operator's persisted WPM (saved via `Preferences` at
   `src/main.cpp:351`) is **overridden by the host's value**, and the
   host's value persists across reboots only if the host sends it again.
   Documented as intentional — the host is the source of truth on the
   air.

2. **Sidetone frequency (`A`) goes through `AudioEngine::setToneFrequency()`**
   (`src/audio_engine.h:185-191`). The audio task reads `s_toneFrequency`
   atomically at every `fillBuffer()` call (`src/audio_engine.cpp:283-312`).

3. **Output enable (`E`) is gated by the operator's KEYING setting.**
   If the operator has turned keying off via the keyboard
   (`docs/keyer.md § 5`), the bridge's `E 1` only re-enables the
   bridge's *intent* — GPIO4 stays LOW until the operator also presses
   `;`. Rationale: don't let a contest logger remotely re-enable RF
   output that the operator explicitly disabled. Implementation:
   `RadioKeyer::setEnabled()` carries two flags (operator + bridge)
   that are AND-ed internally; see `src/radio_keyer.cpp:85-99`.

4. **Buffer playback keys the radio only if the operator's KEYING is on
   AND the TODO at `src/morse_generator.cpp:10-14` has been wired.**
   Until that TODO is done, buffer-playback is sidetone-only. (See
   § 13.4.)

5. **Paddle swap (`R`) and keyer mode (`K`) write to a new
   `WinkeyBridge::_mode` register**, not directly to `MorseKey`. The
   audio task reads this register every element boundary and reinterprets
   `s_keyState` accordingly. (Future work — currently a stub. The
   alternative would be to extend `KeyerType` in `src/display_model.h`,
   but that conflates local-operator setting with host-driven setting.)

6. **Status byte generation** is coalesced (§ 12.4). The bridge
   constructs a status byte only when a state flag changes, not on every
   `feed()` call. This is critical to prevent a busy paddle from
   saturating the serial/BLE link.

### 16.5 Tests

- **Unit tests under `test/test_winkey_bridge/`** following the pattern
  of `test/test_key_event_bus/test_key_event_bus.cpp` and
  `test/test_radio_keyer/test_radio_keyer.cpp` — `test_framework.h`
  macros, host-side CMake build, no ESP32 dependency.
- Test cases:
  - Every command in the dispatch table has at least one positive and
    one negative (out-of-range) test. `S 200` → clamped to 99; `A 30` →
    clamped to 50 Hz; etc.
  - Host open returns `0x06`.
  - Admin open puts the bridge in admin mode; register read/write
    works.
  - Status byte coalescing doesn't drop state.
  - Buffer playback preserves pause/resume position.
  - Echo: every command byte is echoed (in WK2 mode) before the bridge
    processes it.
- **Integration test:** the host-side Python utility (§ 17) connects
  over a fake serial port and runs through the full open-then-command-
  then-status sequence.

### 16.6 Limitations and non-goals

- No WK3 USB-HID descriptor support.
- No WK3 auto-send mode, no WK3 serial-number command.
- No prosign-aware encoder yet (see § 13.2 and § 18.4).
- No weight / Farnsworth / dit-dah-ratio implementation in the audio
  path: the bytes are stored for status readback but the audio path
  uses the Paris-standard fixed timings from `src/morse_constants.h`.
  Significant audio-engineering work to wire these through
  `KeyEnvelop` and `MorseEncoder`.
- No physical speed pot (the Cardputer ADV has no pot); `V` is
  accepted and converted linearly.
- No physical pushbutton; the bridge synthesises pushbutton pin events
  from the operator's KEYING toggle (§ 14.2).

---

## 17. Test strategy and the host-side test utility

### 17.1 Firmware unit tests

New test suite `test/test_winkey_bridge/` follows the pattern of
`test/test_radio_keyer/`. Uses `test/test_framework.h` macros (`CHECK`,
`CHECK_EQ`, `RUN`).

Test categories:

- **Open sequence:** `feed(0x00)` → status reply `0x06`. `feed(0x00 0x00 0x03)` → admin mode entered.
- **One test per command letter:** `feed('S', 25)` →
  `MorseModel::wpm() == 25` (cross-check via `src/display_model.h:262`).
- **Buffer playback:** `feed('L', "CQ")`, `feed('P', 0)` →
  `MorseGenerator::isPlaying() == true`. Eventually drains.
- **Status byte coalescing:** 100 rapid paddle events produce 1 status
  byte, not 100.
- **Out-of-range parameters** are clamped — explicit per-command test.
- **Echo:** every command byte is echoed (in WK2 mode) before the
  bridge processes it.
- **Admin mode register file:** read/write round-trips for all 16
  registers; soft reset restores defaults.

### 17.2 Host-side test utility (`scripts/winkey_host.py`)

A Python script (Ruby also offered as `scripts/winkey_host.rb` for users
who prefer it) that:

- Connects to the A1Keyer's serial port (or to a fake serial port for
  unit tests of the script itself).
- Performs the host open sequence, prints the version byte.
- For each command in a test plan, sends the bytes, reads the echo,
  reads the status byte, asserts on the response.
- Test plan file (`scripts/winkey_test_plan.toml` or similar) lists
  expected behaviour per command.

Python is preferred for cross-platform support (macOS, Linux, Windows
all work without compilation); `pyserial` is the de-facto standard. Ruby
is offered as a fallback. The script runs from a developer's laptop
and does **not** need to be installed on the A1Keyer.

```
┌──────────────┐    serial      ┌──────────────┐
│  Python      │  /dev/tty.USB0 │  A1Keyer     │
│  test util   │◄──────────────►│  (Cardputer) │
│  (host)      │                │              │
└──────────────┘                │  WinkeyBridge│
                                │  on ESP32-S3 │
                                └──────────────┘
       ▲
       │
       │ test results:
       │   PASS: open returned 0x06
       │   PASS: S 25 → status byte reports WPM=25
       │   PASS: buffer play drains, BUFFER-EMPTY bit set
       │   ...
```

### 17.3 Manual validation flow

A short checklist the operator can follow with a paddle + a radio on the
bench:

1. Upload the firmware.
2. Connect over USB.
3. Run `python scripts/winkey_host.py --port /dev/tty.usbserial-* --plan scripts/plan_basic.toml`.
4. Verify all assertions pass.
5. Switch to BLE, repeat.
6. With a radio connected via PC817 and the operator's KEYING setting
   **On**, run the buffer-play test and confirm the radio keys at each
   dit/dah.

---

## 18. Open questions / future work

1. **BLE pairing UX.** How does the operator pair the Cardputer to a
   host? The KEYING overlay's `K`-key toggle is the obvious hook
   (`docs/keyer.md § 5`), but pressing `K` enters the KEYING settings
   overlay (existing UX). Need a new menu entry, or a different key.

2. **Multiple simultaneous hosts.** BLE-NUS allows only one connected
   client at a time. USB-CDC allows multiple but needs hub support.
   A1Keyer policy TBD.

3. **WK3 version-byte reply.** If a WK3-aware host (WriteLog with WK3
   support, hamlib's winkey driver) sends the version query and receives
   `0x06`, it may downgrade. The bridge could send `0x07` while still
   implementing WK2 semantics. K3NG precedent favours `0x06`; decision
   deferred.

4. **Prosign encoder.** A1Keyer's `MorseEncoder`
   (`src/morse_encoder.cpp`) does not expand `<SK>`, `<AR>`, etc. The
   bridge would need a pre-processor that expands prosigns before
   handing to `MorseGenerator::playText()`. Open: should this live in
   the bridge or in the encoder?

5. **Weight / Farnsworth in the audio path.** Currently the bridge
   stores the bytes for status readback but the audio path uses fixed
   Paris timing (`src/morse_constants.h`). To honour weight and
   Farnsworth the `KeyEnvelop` and `MorseEncoder` need new parameters.
   Significant audio-engineering work; future.

6. **Speed-pot simulation.** The bridge accepts `V` and converts
   linearly to WPM. But the K1EL chip's speed pot is a *physical*
   potentiometer; a host that issues `S` overrides it. A1Keyer has no
   physical pot, so the host's `S` always wins. Documented policy; not
   a bug.

7. **Paddle-event mirroring.** The bridge's paddle-echo pin events
   (§ 14.3) would need to tap into the iambic-keyer state machine
   (`src/iambic_keyer.cpp:118` — the existing `KeyEventBus::keyDown()`
   call site). Currently the bridge has no hook into that.
   Implementation: a small subscriber to KeyEventBus specifically for
   the bridge.

8. **Buffer-overflow handling.** When the host pushes more than 110
   chars into the buffer before issuing `P`, K1EL's spec says the
   chip drops the excess and sets the BUSY-OVFL status bit. A1Keyer's
   `WinkeyBuffer` will use a ring buffer sized to match; overflow
   drops bytes and sets the bit.

---

## 19. References

External sources for the protocol:

- **K1EL** — official WK2 datasheet. The primary byte-level reference
  is the WK2 datasheet v23 from October 2010, mirrored at
  [hamcrafters2.com/files/WK2_Datasheet_v23.pdf](https://hamcrafters2.com/files/WK2_Datasheet_v23.pdf).
  The companion WK2 Software Guide (PDF) is at
  [hamcrafters2.com/files/WK2_SW_Guide.pdf](https://hamcrafters2.com/files/WK2_SW_Guide.pdf).
- **hamlib** — open-source HAM-radio control library, with the
  WinKey driver at
  [`src/rig/winkey.c`](https://github.com/Hamlib/Hamlib/blob/master/src/rig/winkey.c).
  Independent byte-level reference.
- **K3NG arduino-keyer** — open-source Arduino CW keyer with Winkeyer
  emulation. The authoritative documentation lives in the GitHub wiki
  at [370-Feature:-Winkey](https://github.com/k3ng/k3ng_cw_keyer/wiki/370-Feature:-Winkey);
  the implementation source is
  [`k3ng_keyer/k3ng_winkey.cpp`](https://github.com/k3ng/k3ng_cw_keyer/blob/master/k3ng_keyer/k3ng_winkey.cpp).
  The blog overview at
  [blog.radioartisan.com/arduino-cw-keyer/](https://blog.radioartisan.com/arduino-cw-keyer/)
  is a useful entry point but explicitly defers to the wiki for
  authoritative details.
- **TeensyWinkeyEmulator** — another open-source WK2 emulator, by
  DL1YCF, at
  [github.com/dl1ycf/TeensyWinkeyEmulator](https://github.com/dl1ycf/TeensyWinkeyEmulator).
- **N1MM Logger+** — Winkeyer interface documentation. `n1mm.com`.
- **Win-Test** — Winkeyer chapter. `win-test.com`.
- **fldigi** — Winkeyer support notes. `w1hkj.com` (Dave Freese, W1HKJ).
- **groups.io "k1elsystems" group** archives — historical context and
  edge-case discussions.

A1Keyer internal cross-references:

- [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) — consolidated firmware
  architecture reference.
- [`docs/keyer.md`](keyer.md) — radio keying output guide (PC817 +
  GPIO4); producer/consumer ASCII diagram that § 16.2 extends.
- [`docs/TESTING.md`](TESTING.md) — CMake / CTest runbook.
- [`src/key_event_bus.h`](key_event_bus.h) — central CW dispatcher.
- [`src/radio_keyer.h`](radio_keyer.h), [`src/radio_keyer.cpp`](radio_keyer.cpp) — PC817 sink.
- [`src/morse_generator.h`](morse_generator.h), [`src/morse_generator.cpp`](morse_generator.cpp) — async text player; future buffer-playback producer.
- [`src/iambic_keyer.h`](iambic_keyer.h), [`src/iambic_keyer.cpp`](iambic_keyer.cpp) — Iambic B FSM; tap point for paddle-echo pin events.

---

*Document version: 0.2 (verified). Status-byte bit map, host-open
sequence, admin command set, and WK3 buffer size cross-checked against
the K1EL WK2 datasheet v23 (October 2010) and the K3NG wiki. The A1Keyer
architecture in § 16 references verified source lines. Specific values
marked `[unverified]` or `[verify]` remain to be confirmed against the
K3NG source / hamlib source on a follow-up revision.*