# Radio Keying Output

A1Keyer can drive a real transceiver's CW key input through a
galvanically-isolated optocoupler. The on-air output mirrors the local
sidetone as a straight-key signal: HIGH during any dit/dah, LOW during
spaces. This document describes the hardware, the wiring, and the
on-device settings that control the feature.

---

## Contents

1. [Overview](#1-overview)
2. [Required hardware](#2-required-hardware)
3. [Wiring](#3-wiring)
   - 3.1 [Cardputer side](#31-cardputer-side)
   - 3.2 [Radio side](#32-radio-side)
   - 3.3 [Module modifications (Hailege 2-channel variant)](#33-module-modifications-hailege-2-channel-variant)
   - 3.4 [Output side — pull-down resistor](#34-output-side--pull-down-resistor)
4. [Settings](#4-settings)
5. [Behaviour](#5-behaviour)
6. [Central key-event bus](#6-central-key-event-bus)
7. [Safety](#7-safety)

---

## 1. Overview

- **Function:** when enabled, the on-air output toggles HIGH during any
  CW tone (iambic dit, iambic dah, straight-key closure, or a held
  `K` key on the Cardputer keyboard) and LOW during any space
  (inter-element, inter-character, inter-word, or release).
- **Galvanic isolation:** the output drives a PC817 optocoupler module
  whose transistor side keys the radio. The Cardputer's ground is
  never connected to the radio's ground.
- **Default:** **Off**. The radio is never keyed unless the user
  explicitly turns the feature on and saves the setting.
- **Target:** M5Stack Cardputer ADV only. The Tab5 (ESP32-P4) build
  compiles the relevant code out — see [`ARCHITECTURE.md` § 15](ARCHITECTURE.md#15-multi-device-porting-tab5--cardputer-adv).
- **Wire format:** straight key. Even when the device is in iambic
  paddle mode, the on-air output is HIGH while the sidetone is on and
  LOW while it is off — i.e. the operator (or the radio's internal
  keyer) sees a single contact closing for the duration of every
  element.
- **Verified on:** Icom IC-705, Yaesu FTX-1 (with the Hailege 2-channel
  PC817 module as wired in §3).

---

## 2. Required hardware

| Component | Notes |
|---|---|
| M5Stack Cardputer ADV | ESP32-S3; the source of the keying signal. |
| PC817 optocoupler module | Tested with the Hailege 2-channel variant ([B0CJY5PL4C](https://www.amazon.de/dp/B0CJY5PL4C)). Other PC817 modules with a built-in input resistor work after the modifications in §3.3. Bare PC817 chips are *not* a drop-in replacement. |
| Two-wire cable | From the Cardputer EXT 2.54-14P header to the PC817 module's input side. |
| Radio with a KEY input | Any transceiver with a CW / straight-key input that accepts a contact closure. Verified on Icom IC-705 and Yaesu FTX-1. |

The cable is wired to the **EXT 2.54-14P** header on the back of the
Cardputer ADV, not the Grove connector. The pins used are adjacent on
that header (G4 and GND), which keeps the wiring tidy.

---

## 3. Wiring

> ### ⚠ End-user responsibility — no warranty, no liability
>
> The wiring and module modifications described below were tested
> successfully with the Hailege 2-channel PC817 module driving an
> Icom IC-705 and a Yaesu FTX-1. **They are not guaranteed to work
> with any other module, transceiver, antenna system, or electrical
> environment.**
>
> - You are solely responsible for verifying that the wiring, the
>   module modifications, and the connection to your radio are safe
>   and correct **before** connecting the output to a live
>   transceiver.
> - The PC817 optocoupler provides galvanic isolation between the
>   Cardputer and the radio, but it does **not** protect against
>   miswiring, reverse polarity, over-voltage on the radio side, RF
>   feedback, or damage caused by an incorrect module modification.
> - Module modifications in §3.3 (parallel resistor, shorted
>   indicator LED) are performed at your own risk. A wiring mistake
>   can damage the Cardputer, the module, or the radio.
> - The author(s) of A1Keyer accept **no liability** for damage to
>   equipment, interference with other stations, regulatory
>   violations, or injury resulting from following these
>   instructions.
>
> If you are not comfortable with basic electronics prototyping
> (soldering, reading a multimeter, identifying SMD pads), please
> consult a licensed amateur radio operator or an electronics
> technician before proceeding.

The PC817 module has **two sides**, galvanically isolated from each
other:

- **Input side** (LED + on-module resistor): connected to the Cardputer.
- **Output side** (transistor collector + emitter): connected to the
  radio. The two sides share no electrical conductor.

The Cardputer GPIO drives the input LED through the on-module resistor;
the radio's KEY input sees a floating-contact closure on the output
side. Most modern transceivers provide their own pull-up on the KEY
line, so the output side needs no additional components.

### 3.1 Cardputer side

Wires go to the **EXT 2.54-14P** header on the back of the Cardputer
ADV. Only two wires are required:

| Pin | Wire colour | Connects to |
|---|---|---|
| `G4` (GPIO4) | White | PC817 module **signal input** (the LED anode side after the on-module resistor) |
| `GND`         | Black | PC817 module **input ground** |

The firmware drives GPIO4 HIGH to key the radio. While idle the line
is LOW.

> **Pin choice.** GPIO4 is convenient because it sits next to a GND
> pin on the EXT 2.54-14P header, so both wires fit cleanly. The
> ESP32-S3 can source enough current through the PC817 LED at 3.3 V
> after the modifications in §3.3.

### 3.2 Radio side

| PC817 output pin | Connects to |
|---|---|
| Collector (C) | Radio **KEY** input |
| Emitter (E)   | Radio **KEY GND** |

The PC817 transistor is normally **OFF** (no keying). When the firmware
drives GPIO4 HIGH, the LED conducts, the transistor turns ON, and the
radio sees a closed contact between KEY and KEY GND.

> **Pull-up.** Most transceivers (including the Icom IC-705 and Yaesu
> FTX-1) provide an internal pull-up on the KEY line. If yours does
> not, add a 4.7 kΩ to 10 kΩ pull-up from KEY to KEY-pullup-voltage
> (typically the radio's own +5 V or +13.8 V accessory rail — check
> the manual). The PC817 transistor must be rated for the pull-up
> voltage; the PC817 is rated to 35 V collector-emitter.

### 3.3 Module modifications (Hailege 2-channel variant)

The Hailege 2-channel PC817 module ([B0CJY5PL4C](https://www.amazon.de/dp/B0CJY5PL4C))
ships with two limitations that prevent reliable keying of a real
transceiver from a 3.3 V GPIO:

1. **The on-module series resistor is too high** (~1 kΩ). At 3.3 V
   the LED current is only ~2 mA — enough to faintly light the
   indicator LED but not enough to fully saturate the output
   transistor. The radio's KEY pull-up is then a marginal voltage
   divider and the transceiver does not see a clean contact closure.
2. **The on-module LED indicator is in series with the IR LED.** The
   indicator LED adds an extra ~1.8 V forward-voltage drop, eating
   most of the 3.3 V supply.

Both issues are fixed by two small modifications:

1. **Solder a 220 Ω resistor in parallel with the on-module resistor**
   on the input side. The combined resistance is ~180 Ω, which
   yields ~11 mA through the IR LED at 3.3 V — comfortably in the
   PC817's saturated-on region.

2. **Short the indicator LED on the input side** (a single solder
   blob across its two pads). This removes the extra forward-voltage
   drop and leaves the IR LED as the only load. The on-module
   indicator no longer lights, but the IR LED brightness is no longer
   your diagnostic — verify keying on the radio side instead.

> **Why modify the module rather than rewire.** Adding a third wire
> (VCC) and inverting the GPIO logic would also work, but the two
> modifications above preserve the simple "GPIO HIGH = keyed" wiring
> the firmware assumes and use the module as a passive driver.

### 3.4 Output side — pull-down resistor

Some modules ship with a pull-down resistor on the output transistor
emitter (a few kΩ to ground). This resistor holds the output LOW at
idle, which is fine for direct connection to most transceivers, but
on radios where the KEY line has a strong pull-up the resistor
forms a voltage divider that prevents the line from reaching the
"keyed" threshold.

If your radio does not key reliably with the modifications in §3.3
applied, **short this output-side pull-down resistor** as well.
Tested radios (Icom IC-705, Yaesu FTX-1) work either way because
their internal KEY pull-ups dominate.

---

## 5. Settings

The keying output is configured through the same settings-overlay
pattern as WPM, frequency, volume, and keyer mode.

| Step | Key | Effect |
|---|---|---|
| Open the keying settings screen | `K` (on the decoder screen) | Shows the **KEYING** overlay with current value |
| Toggle radio keying **On** | `;` (semicolon) | The display flips to **On** in red so you notice the GPIO is now live |
| Toggle radio keying **Off** | `.` (period) | The display flips to **Off**; GPIO is forced LOW immediately |
| Save and close | `Enter` (or `Button A`) | Persists the setting to NVS and returns to the decoder screen |

The setting is stored in the **`morse`** NVS namespace under the key
**`keying`** as a boolean. On boot the value is read with
`getBool("keying", false)` and applied to the model and to
`RadioKeyer::setEnabled()` after `RadioKeyer::begin()`. The default is
**`false`** (Off) — the firmware never keys the radio on a fresh
device.

The 10-second overlay timeout also applies; if you open the screen
and walk away it auto-dismisses without changing the running setting
(only `Enter` saves).

### The `K` key when keying is enabled

When the **KEYING** setting is **On** and the decoder screen is
showing (i.e. **not** the keying settings overlay), holding the `K`
key on the Cardputer keyboard mirrors a straight-key press: GPIO4 goes
HIGH for as long as `K` is held and goes LOW on release. This is
useful for sending a fixed test string (e.g. "VVV TEST TEST") with
just the keyboard without touching a paddle.

The `K` key that opens the settings overlay is **edge-triggered** and
consumed: after opening or closing the overlay, the next `K` press
must be released before it arms as a key. This prevents the radio
from being keyed the instant you close the settings screen with `K`
still held.

---

## 6. Behaviour

Mono summary of when GPIO4 is HIGH, assuming keying is enabled:

| Situation | GPIO4 |
|---|---|
| Iambic paddle — DIT pressed | HIGH for one dit duration |
| Iambic paddle — DAH pressed | HIGH for three dit durations |
| Iambic paddle — squeezed DIT-DAH | HIGH-LOW-HIGH-LOW with the standard inter-element spacing |
| Straight key — pressed | HIGH while held |
| Cardputer `K` key — held (decoder screen) | HIGH while held |
| Inter-element, inter-character, inter-word spaces | LOW |
| Keying setting is **Off** | LOW (always, regardless of the above) |

The release-from-spaces timing is computed per element, not per
envelope. The `KeyEnvelop` DIT/DAH tables embed trailing silence
(DIT = 2 × dit, DAH = 4 × dit), so the radio would otherwise stay
keyed through the gap. The iambic keyer tracks
`_elementKeyedSamples` (1 dit for DIT, 3 dits for DAH) and releases
the line at the keyed-sample boundary with a per-element latch so the
release fires exactly once per element.

If the user toggles keying **Off** mid-element, the GPIO is forced
LOW immediately and the bus is drained (`KeyEventBus::forceAllUp()`)
so any subsequent producer transitions cannot re-key the line. The
GPIO stays LOW until keying is re-enabled **and** a fresh dit/dah
starts.

### What is **not** routed to the radio (yet)

- **The text player (`P` key, "Hello Morse!" demo).** The
  `MorseGenerator` does not call `KeyEventBus` in this revision —
  the player still emits audio but does not key the radio. The
  `KeyEventBus` hook point is documented in
  `morse_generator.cpp` so the future Winkey-compatible player (PC
  bridge) can emit `keyDown()` / `keyUp()` at every dit/dah boundary
  the same way the keyers do.
- **Paddle echoes from the decoder back into the on-air signal.** The
  decoder is read-only.

---

## 7. Central key-event bus

A single small module — `KeyEventBus` — is the only place where CW
producers and consumers meet. This keeps the keyers ignorant of the
radio-keyer and vice versa, which is what makes adding a Winkey
bridge, a MIDI keyer, or a logging sink a one-line change.

```
         ┌────────────────────┐
         │   IambicKeyer      │
         │   StraightKeyer    │  producers (call keyDown / keyUp)
         │   MorseGenerator   │
         │   WinkeyBridge     │  (future)
         └─────────┬──────────┘
                   │
                   ▼
         ┌────────────────────┐
         │   KeyEventBus      │  ref-counted 0→1 / 1→0 edges
         └─────────┬──────────┘
                   │
                   ▼
         ┌────────────────────┐
         │   RadioKeyer       │  sinks (subscribe once)
         │   Logger           │  (future)
         │   MIDI keyer       │  (future)
         └────────────────────┘
```

Properties:

- **Reference counted.** Multiple producers can overlap (e.g. held
  `K` keyboard + iambic paddle). The aggregate is "down" from the
  first `keyDown` until the last matching `keyUp`.
- **Edge-triggered sinks.** Sinks only see 0→1 and 1→0 transitions.
  Holding a paddle for the duration of a DIT calls `keyDown` once
  and `keyUp` once.
- **Atomic.** The reference count is `std::atomic<int>`; producers
  on the audio task (Core 1) and the loop task (Core 0) can call
  concurrently without locking.
- **Bounded sinks.** Up to 8 concurrent subscribers
  (`KeyEventBus::kMaxSinks`). Subscribe / unsubscribe are not
  expected on the real-time audio path — `RadioKeyer::begin()` runs
  once at setup.

The bus is unit-tested in `test/test_key_event_bus/`:
balanced `keyDown` / `keyUp` drives sinks through 0→1→0, overlapping
demand requires matching ups, `forceAllUp()` immediately drives
sinks to 0, and `unsubscribe()` removes a sink from further
transitions.

---

## 8. Safety

> **Read §3 first.** The disclaimer at the top of §3 (no warranty,
> no liability, end-user responsibility) applies to everything in
> this section. Do not skip it.

Treat this as a transmitter interface, not a GPIO.

1. **You are responsible for your own wiring.** The wiring and
   module modifications described in §3 were tested with one
   specific module (Hailege 2-channel PC817) and two specific
   transceivers (Icom IC-705, Yaesu FTX-1). Other modules and
   radios may need a different resistor value, a different wiring
   topology, or an additional buffer stage. Verify the
   specification of **your** module and **your** radio before
   connecting them. If you are uncertain, stop and ask someone
   who can measure the result.
2. **Verify radio key voltage and current before connecting.** Open
   the radio's KEY input specification. Most modern transceivers
   expect a contact closure to ground and tolerate a few mA at
   5–13 V, but a few require a positive voltage on KEY relative to
   KEY GND. The PC817 transistor output is a floating contact — if
   your radio expects a positive voltage on the KEY line, this
   circuit will not work; you need a different interface.
3. **Check polarity.** PC817 modules are usually marked with `+` and
   `-` on the input side. Reversing the LED wires will not damage
   the Cardputer (the internal ESD diode clamps) but the optocoupler
   will never turn on and the radio will never key — silent failure
   is the worst kind.
4. **Multimeter-test the wiring before going on-air.** With the
   Cardputer powered and keying **On**, hold the `K` key. Measure
   the resistance between the PC817 output pins. It should swing
   from open-circuit (released) to a few hundred ohms (collector →
   emitter saturation). If it does not, fix the wiring before
   connecting the radio.
5. **This is CW keying, not RF PTT.** The output mirrors the sidetone
   envelope. It does not switch the radio's RF chain. You still need
   the radio set to CW mode on the band you want to transmit on.
6. **Disable in shared environments.** If you are practising in a
   club setting with a shared antenna, leave the keying setting
   **Off** unless you intend to transmit. The default behaviour is
   Off precisely for this reason.
7. **No RF feedback.** The PC817 provides galvanic isolation, but it
   does not provide RF isolation. If you observe RF in the shack
   (common on the higher bands with a mismatched antenna), add a
   small RC snubber across the PC817 output or use a shielded cable
   for the radio-side wiring.
