# Architecture

This document is the consolidated technical reference for the firmware: the
audio path, the Blackman-Harris envelope maths, the iambic and straight-keyer
state machines, the decoder protocol, the ES8388 codec initialisation, the
display subsystem, and the multi-device porting story.

It is the canonical place to look when a "why is it done this way" question
comes up. Source comments are intentionally terse — this document carries the
narrative.

## Contents

1. [Hardware overview](#1-hardware-overview)
2. [Audio path & PCM format](#2-audio-path--pcm-format)
3. [Click-free envelope (Blackman-Harris)](#3-click-free-envelope-blackman-harris)
4. [Morse timing (Paris standard)](#4-morse-timing-paris-standard)
5. [Iambic B keyer](#5-iambic-b-keyer)
6. [Straight key](#6-straight-key)
7. [CW decoder](#7-cw-decoder)
8. [Text playback (MorseGenerator)](#8-text-playback-morsegenerator)
9. [Display subsystem](#9-display-subsystem)
10. [Volume control](#10-volume-control)
11. [ES8388 codec initialisation (Tab5)](#11-es8388-codec-initialisation-tab5)
12. [Why direct I²S, not M5.Speaker](#12-why-direct-i²s-not-mspeaker)
13. [Cross-core synchronisation](#13-cross-core-synchronisation)
14. [Multi-device porting (Tab5 ↔ Cardputer ADV)](#14-multi-device-porting-tab5--cardputer-adv)
15. [cmorse reference (extern/)](#15-cmorse-reference-extern)

---

## 1. Hardware overview

| Feature | Tab5 | Cardputer ADV |
|---|---|---|
| MCU | ESP32-P4, dual-core LX9 @ 360 MHz | ESP32-S3, dual-core LX7 @ 240 MHz |
| RAM | 32 MB PSRAM, 768 KB SRAM | 8 MB PSRAM, 512 KB SRAM |
| Flash | 16 MB | 16 MB |
| Display | 5" 1280×800 IPS capacitive | 1.14" 240×135 ST7789V2 |
| Audio codec | ES8388 (I²C 0x10) + PI4IOE5V6408 amp | NS4168 I²S amp (mono, no digital vol) |
| Headphone | 3.5 mm TRS (no detect GPIO) | none |
| I²S MCLK | GPIO 30 | not used |
| I²S BCLK | GPIO 27 | GPIO 41 |
| I²S LRCLK | GPIO 29 | GPIO 43 |
| I²S DOUT | GPIO 26 | GPIO 42 |
| I²C SDA / SCL | GPIO 31 / 32 | — |
| Paddle DIT / DAH | GPIO 1 / GPIO 2 | GPIO 1 / GPIO 2 (Grove Port.A) |

Both environments are built from the same source tree. The
`BOARD_TAB5` / `BOARD_CARDPUTER` macros (set in `platformio.ini`) gate
hardware-specific code paths.

---

## 2. Audio path & PCM format

The I²S peripheral and both codecs use **signed 16-bit two's complement PCM**:

| Value | Meaning |
|---|---|
| `0x0000` | Silence — speaker membrane at rest |
| `0x7FFF` (+32767) | Maximum positive displacement |
| `0x8000` (−32768) | Maximum negative displacement |

The ES8388 is AC-coupled; the digital `0x0000` maps to the mechanical rest
position. **No DC offset is needed in the digital domain.**

A click (audible transient) appears whenever the speaker membrane is forced to
move abruptly — i.e., whenever the PCM value steps suddenly. Sources include:

- Hard key-on / key-off (step from 0 to ±N or back)
- Timing mismatch — silence too long / too short, leaving a gap where the
  envelope hasn't decayed to zero
- DC component — waveform average non-zero

A sine wave has zero mean, so click-free CW requires that the amplitude
envelope **starts and ends at zero** and **transitions smoothly**. That is
the job of the Blackman-Harris envelope described next.

### Audio task

A single high-priority FreeRTOS task (Core 1, priority 22) loops:

```
fillBuffer(monoBuf, 64)        // produces 64 int16 samples
  → scale by s_cachedAmplitude // software volume
  → copy L=R into stereoBuf    // 64 frames × 2 ch interleaved
  → i2s_channel_write(...)     // DMA ring of 2 buffers
```

`fillBuffer()` dispatches to one of three producers based on `KeyerType` and
`isPlaying()`:

| Priority | Condition | Producer |
|----------|-----------|----------|
| 1 | `KeyerType::STRAIGHT` and straight keyer exists and `!morseGen.isPlaying()` | `StraightKeyer::fillSamples` |
| 2 | `KeyerType::PADDLE` (i.e. not STRAIGHT) and keyer memory or active | `IambicKeyer::fillSamples` |
| 3 | `morseGen.isPlaying()` | `MorseGenerator::fillSamplesMono` |
| 4 | else | silence (Tab5: samples forced to 1 to keep the DAC awake) |

`MorseGenerator` always takes priority over the keyers when it has text to
play, even in straight-key mode (bug fix from 2026-05-22).

### Latency

```
latency_ms = (DMA_BUF_COUNT * DMA_BUF_LEN) / SAMPLE_RATE * 1000
           = (2 * 64) / 48000 * 1000
           ≈ 2.67 ms  (worst case: key pressed just after a buffer write starts)
```

End-to-end on Tab5: 2.67 ms (DMA) + 0.5 ms (ES8388 analog) ≈ **3.2 ms**.

---

## 3. Click-free envelope (Blackman-Harris)

`KeyEnvelop` (in `src/key_envelop.{h,cpp}`) builds pre-computed amplitude
envelopes for DIT and DAH elements. The shape is the **integrated
Blackman-Harris window** — the cumulative sum of a 4-term cosine window
forms an S-shaped step function with zero first and second derivatives at
both ends.

The 4-term BH window coefficients:

```
a0 = 0.35875
a1 = 0.48829
a2 = 0.14128
a3 = 0.01168
```

```
BH(x) = a0 − a1·cos(2πx) + a2·cos(4πx) − a3·cos(6πx)
```

`buildRiseRamp()` computes `out[i] = cumulative_sum(BH(i/len))` then
normalises so the last sample is 1.0. The fall ramp is the time-reversed
rise ramp.

### Envelope layout (cmorse-compatible)

The envelope includes the tone **and the following intra-character silence**
as one contiguous buffer — the key difference from a "tone only + separate
silence" design.

```
DIT envelope (2 × ditLen samples):
  [ramp | flat (ditLen − 2·ramp) | ramp | silence (ditLen − ramp)]
  |--------- tone (1 dit) ----------|-- trailing silence (1 dit) --|

DAH envelope (4 × ditLen samples):
  [ramp | flat (3·ditLen − 2·ramp) | ramp | silence (3·ditLen − ramp)]
  |--------- tone (3 dit) ----------|--- trailing silence (3 dit) ---|
```

- DIT: `envelopeSize(DIT) = 2 × ditLen` (e.g. 5760 samples at 20 WPM / 48 kHz)
- DAH: `envelopeSize(DAH) = 4 × ditLen` (e.g. 11520 samples at 20 WPM / 48 kHz)

The trailing silence is what provides the intra-character gap, so no separate
silence mechanism is needed between consecutive elements. This matches the
`cmorse` reference exactly.

### Ramp-length cap

`rampTimeSec` defaults to 0.005 s (5 ms). The ramp in samples is
`round(rampTimeSec × sampleRate)` but is **capped to `ditLen / 4`** so the
flat-top portion is at least `ditLen / 2`. At 20 WPM / 48 kHz:

```
rampLen = round(0.005 × 48000) = 240 samples
ditLen  = round(48000 × 1.2 / 20) = 2880 samples
cap     = ditLen / 4 = 720 samples
240 < 720 → use 240
```

A `_dirty` flag triggers lazy regeneration whenever `_wpm` or `_rampTimeSec`
changes. Regeneration uses `new (std::nothrow)` to avoid `std::bad_alloc` →
`abort()` on heap fragmentation, and updates size metadata only after all
allocations succeed (so on OOM the old buffer + matching size stay in use).

---

## 4. Morse timing (Paris standard)

```
1 dit unit   = 1.2 / wpm seconds
DIT          = 1 unit (mark)
DAH          = 3 units (mark)
Element gap  = 1 unit (within character)
Char gap     = 3 units (between characters)
Word gap     = 7 units (between words)
```

At 20 WPM: 1 unit = 60 ms = 2880 samples @ 48 kHz.

The constants are in `src/morse_constants.h`:

| Constant | Value | Used by |
|----------|-------|---------|
| `DIT_UNITS` | 1 | envelope |
| `DAH_UNITS` | 3 | envelope |
| `ELEMENT_SPACE_UNITS` | 1 | inside the envelope itself |
| `CHAR_SPACE_UNITS` | 3 | `MorseEncoder` adds these between chars |
| `WORD_SPACE_UNITS` | 7 | `MorseEncoder` adds these at word boundaries; `IambicKeyer` detects a gap > 7 ditts |
| `WORD_SPACE_DITS` | 7 | gap threshold for word-space detection |

**Edge attribution:** the rising edge (key-down) starts a mark. The falling
edge (key-up) is the operator releasing the key — not part of the mark. The
BH ramp-down is baked into the envelope (so it starts during the mark and
tails into the trailing silence), and the silence is sized to keep total
timing on the Morse grid.

### What "envelope includes silence" means for timing

After the iambic keyer finishes a DIT (2880 samples tone + 2880 samples
silence baked in), it has already produced the 1-dit intra-character gap.
Consecutive element starts back-to-back produce the correct character
timing without explicit "element space" tracking. The 3-unit inter-character
gap and 7-unit word gap are added by the encoder when it strings multiple
characters into a string, or by the keyer's word-space detection on the
fly when keying from a paddle.

---

## 5. Iambic B keyer

`IambicKeyer` (`src/iambic_keyer.{h,cpp}`) implements the Iambic B state
machine. It reads the `KeyState` written by `MorseKey` ISRs and produces
CW samples through the shared `KeyEnvelop`. Source: ported from
`cmorse/extern/main.c::paddle_key_callback`.

### Data flow

```
ISR (any core, GPIO1/DIT, GPIO2/DAH)
    │  atomic_store: s_keyState.memory[DIT/DAH] = SET   (on press only)
    │  atomic_store: s_keyState.state[DIT/DAH]  = SET/UNSET (every edge)
    ▼
Audio Task (Core 1, fillBuffer loop)
    │
    │  if s_keyState.memory[DIT] or s_keyState.memory[DAH]
    │    → IambicKeyer::fillSamples
    │  else if s_morseGen->isPlaying()
    │    → MorseGenerator (unchanged)
    │  else
    │    → silence
    ▼
```

ISR **only writes** atomics. Audio task **only reads** and clears `memory[]`
when an element ends and the paddle is released. No mutexes in the
real-time path.

### KeyState (defined in `iambic_keyer.cpp`)

```cpp
struct KeyState {
    std::atomic<int> memory[2];  // DIT=0, DAH=1: ISR sets (press only),
                                //              audio clears (element end + paddle released)
    std::atomic<int> state[2];   // DIT=0, DAH=1: ISR sets every edge, audio reads
};
extern KeyState s_keyState;
```

### Iambic B decision tree

At every element boundary, exactly one of three things happens:

| Branch | Condition | Action |
|--------|-----------|--------|
| **SWAP** | `memory[opposite]` is SET | Immediately start the opposite element — no silence between. Squeezed paddles alternate DIT-DAH-DIT-DAH. |
| **AUTOREPEAT** | `memory[opposite]` UNSET, `memory[own]` SET | Set `_interElementSilenceSamples = ditSamples`; the next `fillSamples()` drains the silence, then IDLE restarts the same element. Held paddle repeats with 1-dit gap. |
| **END_OF_CHAR** | neither memory set | Set `_currentElement = NONE`, record `_lastElementEndFrame`, write `END_OF_CHAR ('*')` to the decoder ring buffer. |

The decision uses `if` / `else if` / `else` (mutually exclusive) so SWAP and
AUTOREPEAT are correctly prioritised.

### Word-space detection

Word space is detected at the **start** of a new element, not during idle:

```cpp
if (_lastElementEndFrame != 0) {
    if (_totalSamplesRendered - _lastElementEndFrame
        > WORD_SPACE_DITS * ditSamples) {
        rbWrite(SPACE_CHAR);                 // ' '
        _lastElementEndFrame = _totalSamplesRendered;
    }
}
```

This is checked before consuming memory. A `_spaceWrittenInIdle` flag
prevents writing multiple spaces per idle period. After a character ends,
the audio task is kept "active" (`isActive() == true`) so the gap counter
keeps advancing until the threshold is met.

### Bounce handling

The audio task's release check at element boundaries
(`if (!state[own]) clear memory[own]`) protects the iambic keyer from most
contact bounce: a spurious rising edge while the paddle is actually released
sets `memory[SET]`, but the audio thread only clears memory when the element
ends AND the paddle is released, so a stuck tone cannot result.

For the straight-keyer (next section) a one-shot GPIO verification is
added at each state transition because the straight key has no
state-machine-level guard.

### Decoder ring buffer

`IambicKeyer::_rb[256]` carries a 4-symbol alphabet from the audio task
(producer) to `loop()` (consumer):

| Symbol | Constant | Meaning |
|--------|----------|---------|
| `.` | `DIT_SYMBOL` | DIT element ended |
| `-` | `DAH_SYMBOL` | DAH element ended |
| `*` | `END_OF_CHAR` | character ended (neither memory set) |
| ` ` | `SPACE_CHAR` | word space detected (gap > 7 ditt) |

`decoderRead(char* out)` is non-blocking. `loop()` drains this buffer and
forwards to `MorseDecoder::accumulate()`.

---

## 6. Straight key

`StraightKeyer` (`src/straight_keyer.{h,cpp}`) is a four-state machine
(IDLE → RISE → HOLD → FALL → IDLE) that uses the **same `KeyEnvelop`**
ramp tables as the iambic keyer. The operator controls both timing and
content by holding the key down.

### State machine

```
IDLE
  on memory[DIT]==SET:
    → verify with gpio_get_level() (bounce guard)
    → if GPIO released (bounce): clear memory, stay IDLE
    → else: RISE (rampPos = 0)

RISE
  sample = sine × _riseRamp[rampPos]
  on rampPos >= rampSamples:
    → HOLD

HOLD
  sample = sine × full amplitude
  on state[DIT]==UNSET:
    → verify with gpio_get_level() (bounce guard)
    → if GPIO still pressed (bounce): stay in HOLD
    → else: write DOWN event to ring buffer, FALL

FALL
  sample = sine × _fallRamp[rampPos]
  on rampPos >= rampSamples:
    → write UP event to ring buffer, IDLE
```

`RAMP_LEN = 256`; `rampSamples = round(0.005 × sampleRate)` ≈ 240 at 48 kHz.

### Bounce guard

A **one-shot GPIO verification** at every state transition:

- **IDLE→RISE**: if `gpio_get_level()` shows the pin released, the rising
  edge was bounce — clear `memory` and stay IDLE.
- **HOLD→FALL**: if `gpio_get_level()` shows the pin still pressed, it was
  bounce — stay in HOLD.

Mechanical contact bounce averages 1.6 ms, worst-case 6 ms. The check
catches the specific failure mode where bounce fires a spurious rising
edge after a real release, re-setting `memory` before the audio loop has
transitioned IDLE→RISE.

### Adaptive DIT/DAH classification

A rolling buffer of the last 10 element durations drives a median
threshold. After a key release:

```
if duration < _framesPerDit * 3/2  →  DIT ('.')
else                                →  DAH ('-')
```

The median is re-computed as new durations arrive, so the threshold
adapts to the operator's actual keying speed. Re-classified elements
also update the threshold for future classifications.

### Decoder ring buffer (KeyEvent)

The straight keyer has a **second** ring buffer in addition to the
synthesised `.`/`-`/`*`/` ` symbol stream — a `KeyEvent` ring buffer
that records raw DOWN/UP events with audio frame timestamps:

```cpp
struct KeyEvent {
    char   event;   // 0 = DOWN, 1 = UP
    uint64_t frame;  // _totalSamplesRendered at time of event
};
```

A separate `decodeFromLoop()` task (Core 0) reads these events, classifies
the duration between DOWN and UP, detects intra-character and word gaps,
and writes decoded characters to the model. This separation lets the
audio thread produce samples and the decoder thread reason about timing
without lock contention.

---

## 7. CW decoder

`MorseDecoder` (`src/morse_decoder.{h,cpp}`) is a **singleton** (static
methods) that consumes symbols from the iambic keyer's ring buffer and
turns them into text. The straight keyer feeds into the same accumulator
through its own decoder stream.

```
while (decoderRead(&sym)) {
    switch (sym) {
        case '.':
        case '-':  accumulate(sym); break;
        case '*':  flush();         break;   // end of character
        case ' ':  print(' ');      break;   // word space
    }
}
```

`flush()` calls `MorseEncoder::charFromMorse()` which performs a binary
search on the morse alphabet table (shared with `MorseEncoder` via
`MorseTable`). Unknown sequences are printed as `?[...]`.

`MorseModel::appendDecodedChar(c, fromPlayer)` records the character and
its source (keyer vs player) in a per-character attribute buffer, which
the renderer reads to colour keyer (white) and player (green) text
differently in the same scroll line.

---

## 8. Text playback (MorseGenerator)

`MorseGenerator` (`src/morse_generator.{h,cpp}`) is the async text player.
`playText("Hello Morse!")` (called from `loop()` or `setup()`) starts
streaming CW samples through the audio path.

The flow:

1. `MorseEncoder::encode(text)` → `std::vector<Element>`
   (DIT / DAH / ELEMENT_SPACE / CHAR_SPACE / WORD_SPACE)
2. `MorseGenerator::fillSamplesMono()` walks elements one sample at a time:
   - Each DIT/DAH: `sample = env[elSamplePos] × sineLUT[phase]`
   - Each space: `sample = 0`
3. `MorseModel::instance().setEncoderChar(c)` is called per finished
   character so the display can render playback characters in the player
   colour.
4. `MorseModel::instance().appendDecodedChar(c, /*fromPlayer=*/true)`
   appends the character to the text buffer when the character ends.
   `playText()` calls `resetPlayerHead()` first so a fresh playback
   session starts cleanly.

The sine phase advances **continuously** during silence so the next tone
starts at the correct phase rather than always at 0. This prevents a
phase-jump click at tone onset (the envelope also fades to zero, so the
effect is small but real).

`setWPM(int)` mutates WPM on both the encoder and the shared `KeyEnvelop`
at the next element boundary — the existing element finishes, the
envelopes are lazily regenerated, and the next element uses the new WPM.

---

## 9. Display subsystem

The display is a Model-View-Controller-style subsystem: a single
`MorseModel` singleton holds atomic state, the `DisplayTask` thread polls
its change counter and re-renders, and `loop()` updates model state and
calls `requestRender()` for immediate redraws.

### Threads

| Core | Task | Role |
|------|------|------|
| Core 0 | `loop()` | Keyboard / button input, model state updates, `requestRender()` |
| Core 0 | `DisplayTask` | Polls `changeCounter` every 50 ms (~20 fps); re-renders on change |
| Core 1 | `fillBuffer` (audio) | Writes `MorseModel` for `keyerPatternPercent` and `encoderChar` only |

All `MorseModel` fields are `std::atomic` — no mutexes.

### Text buffer

A 200-character circular buffer with two accessors:

- `textAt(idx)` — raw character
- `attrAt(idx)` — `'K'` (keyer) or `'P'` (player)

Renderer reads both in order, switching `setTextColor()` whenever the
attribute changes. This naturally handles multiple keyer↔player↔keyer
transitions within a single visible line — no fragile arithmetic over
`playerHead` vs `scrollOffset`.

### Settings persistence

`Preferences` (NVS) stores WPM, frequency, volume, and keyer type. Saved
on Enter from each settings screen, loaded on boot.

### Screen-saver

Display dims after 5 minutes of inactivity. Any key press or paddle
event calls `DisplayTask::wakeFromScreensaver()` and resets the
inactivity timer via `MorseModel::instance().touch()`.

### Cardputer (240×135)

- Monospace `FreeMono24pt7b` font, `maxChars = (SCREEN_W − 2) / textWidth("M")`
- Per-character keyer / player colour attribute
- Scrolling: when `textLen > maxChars`, the visible window is the
  rightmost `maxChars` characters of the buffer
- Keyboard: W (WPM settings), F (freq), V (volume), M (mode), P (play
  "Hello Morse!"), `;`/`.` (increment/decrement), Shift+W/Shift+F
  (WPM ± 1), Enter / Button A (save & dismiss overlay)

### Tab5 (1280×800)

Capacitive touch buttons in the top row control volume and trigger
playback. The detailed touch layout is in `src/ui_layout.h`.

---

## 10. Volume control

Volume is a percentage 0–100 (default 50) stored on `AudioEngine` as
`std::atomic<int>`. It is applied independently on each output path.

| Path | Implementation | Hardware |
|------|----------------|----------|
| **Speaker** | PCM sample amplitude scaled in `fillBuffer()` before I2S | Both |
| **Headphone** (Tab5) | ES8388 DAC L/R digital attenuator regs `0x1A`/`0x1B`, 0.5 dB per code | Tab5 |
| **Cardputer** | Software amplitude only (NS4168 has no digital volume) | Cardputer |

### Logarithmic mapping

```
dB       = 20 × log10(percent / 100)
amp      = 32767 × 10^(dB / 20)
codec    = round(−2 × dB)         // ES8388: 0.5 dB per code
```

| Percent | dB    | Amplitude | Codec code |
|---------|-------|-----------|------------|
| 100 %   | 0 dB  | 32767     | `0x00` |
| 50 %    | −6 dB | 16384     | `0x0C` |
| 25 %    | −12 dB| 8192      | `0x18` |
| 1 %     | −40 dB| 328       | `0x50` |
| 0 %     | −∞    | 0         | `0xFF` (mute) |

### Real-time safety

A pre-computed `s_cachedAmplitude` is read by the audio task — no
floating-point math in the real-time path. `setVolumePercent()` recomputes
the amplitude on the UI thread and stores the int16 result.

`applyCodecVolume()` is **not** called on Cardputer (NS4168 has no digital
volume control — software amplitude is the only path).

### ES8388 reg 0x1C warning

The ES8388 has **no "click-free ramp" register**. Writing to register
`0x1C` (DACCONTROL4 / `DACLRCKDIV` / `DACFSMODE`) to attempt a ramp
briefly misconfigures the DAC clock and produces an audible glitch on
every volume button press. Volume changes only write `0x1A` and `0x1B`.

### Tab5 zero-silence guard

On Tab5, the audio task replaces every zero sample with `1` to prevent
the ES8388 DAC from entering power-saving mode during silence. This
applies to the buffer before volume scaling; inaudible at any
non-mute volume.

---

## 11. ES8388 codec initialisation (Tab5)

The ES8388 (I²C 0x10) is configured as an **I²S slave** — the ESP32-P4
drives all clocks. Init happens in `AudioEngine::initCodec()` after
`M5.begin()` is called with `cfg.internal_spk = false` (so M5Unified
doesn't grab `I2S_NUM_0`).

| Reg | Value | Purpose |
|-----|-------|---------|
| `0x00` | `0x80` then `0x00` | Reset, then release |
| `0x01` | `0x58` | VMID 50k, VREF, IBIASGEN on |
| `0x04` | `0x3C` | DAC power: L/R DAC, LOUT1/2, ROUT1/2 on |
| `0x0B` | `0x02` | I²S Philips slave, 16-bit, MCLK=256×Fs |
| `0x13` | `0x10` | System: enable HP output drive |
| `0x19` | `0x20` | DAC unmute L+R |
| `0x1A` | `0x00` | LDACVOL: 0 dB (max) |
| `0x1B` | `0x00` | RDACVOL: 0 dB (max) |
| `0x27` | `0xB8` | Left output mixer: DAC direct |
| `0x2A` | `0xB8` | Right output mixer: DAC direct |
| `0x2E`–`0x31` | `0x21` | LOUT1/ROUT1/LOUT2/ROUT2: 0 dB, unmute |

### Amplifier enable (PI4IOE5V6408)

External speaker amplifier is gated by the PI4IO GPIO expander at I²C
`0x43`, bit 1 of register `0x05`. `bitOn(0x43, 0x05, 0b00000010)`
enables it. A read-modify-write avoids disturbing other bits controlled
by M5Unified.

### Init order

```
M5.begin(cfg)                    // M5Unified (Display, IMU, RTC, etc.)
  ↓
I²C bus on GPIO 31/32 (M5 does this in M5.begin)
  ↓
AudioEngine::initCodec()         // ES8388 + PI4IOE5V6408
  ↓
I²S channel_new + init_std_mode   // pin assignment, 48 kHz, 16-bit
  ↓
spk_task (FreeRTOS, Core 1) starts; i2s_channel_enable() at first buffer
  ↓
AUDIO_DMA_BUF_COUNT × AUDIO_DMA_BUF_LEN sample latency
```

The ES8388 must be configured before the I²S peripheral starts driving
clocks, because the codec needs MCLK from I²S to operate.

### Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| No audio | I²C failure on `0x10`, AMP bit not set, wrong MCLK ratio |
| Distorted audio | Wrong sample rate, magnification too high, buffer wrap mid-sine-cycle |
| Buzzy / harsh | Sine buffer doesn't contain an integer number of cycles |
| Click on start/stop | Abrupt amplitude change (envelope ramps too short) |
| Click on volume change | `0x1C` written (it shouldn't be) |

---

## 12. Why direct I²S, not M5.Speaker

`M5.Speaker` (in M5Unified) is designed for **audio clip playback**, not
real-time synthesis:

- Maintains 8–16 KB internal ring buffers
- Drains in 256+ sample DMA chunks with 8+ buffer queue depth
- No audio-callback registration — you hand it a complete buffer and the
  library schedules it internally
- Typical end-to-end latency: 50–150 ms (unacceptable for CW)

`AudioEngine` uses ESP-IDF 5.x's new I²S API directly:

```cpp
i2s_new_channel()           // allocate channel + DMA ring
i2s_channel_init_std_mode() // set clock, bit-width, pins
i2s_channel_enable()        // start clocks
i2s_channel_write()         // blocking write: returns when DMA accepts
```

A single FreeRTOS task runs `fillBuffer() → i2s_channel_write()` in a
tight loop. This is functionally identical to an audio callback called
every `AUDIO_DMA_BUF_LEN` samples.

The legacy API (`i2s_driver_install`, `i2s_set_pin`, `i2s_write`) does
**not exist** on ESP32-P4 / ESP-IDF 5.x — using it on this chip is a
compile error.

### M5Unified coexistence

M5Unified is still used for Display, IMU, RTC, etc. The conflict is only
the Speaker subsystem. M5Unified's Tab5 speaker pin configuration
attaches to `I2S_NUM_0` if `cfg.internal_spk = true`. Passing
`cfg.internal_spk = false` before `M5.begin()` makes M5Unified skip
those pin registrations, so `M5.Speaker.begin()` becomes a harmless
no-op and `AudioEngine` owns `I2S_NUM_0` exclusively.

---

## 13. Cross-core synchronisation

All cross-core state lives in `std::atomic`. No mutexes in the real-time
audio path.

| Atomic | Producer | Consumer | Ordering |
|--------|----------|----------|----------|
| `s_keyState.memory[2]` | ISR (rising edge only) | Audio task (`fillSamples`) | `memory_order_relaxed` |
| `s_keyState.state[2]` | ISR (every edge) | Audio task (release check) | `memory_order_relaxed` |
| `s_toneActive` | ISR (set only) | Audio task (read) | `memory_order_relaxed` |
| `MorseModel::_textBuf/_textAttr` | Audio + loop threads | `DisplayTask` (read) | `seq_cst` + `atomic_thread_fence` around head/len stores |
| `AudioEngine::s_volumePercent` | `loop()` | Audio task (read on volume change) | `memory_order_relaxed` |
| `AudioEngine::s_cachedAmplitude` | UI thread (write) | Audio task (read) | `memory_order_relaxed` |

The `seq_cst` fences around the circular-text-buffer head/len stores were
added after a transient-inconsistent state crash (`head=N, len=N` while
`_textAttr[N−8..N−1]` still held stale data from a `clear()`). The fences
guarantee the head/len update is visible atomically with the underlying
buffer write.

### Why no mutexes

The audio task is pinned to Core 1 at priority 22 and runs continuously
in a tight loop. A mutex on a Core-0 ISR would cause I²S DMA underrun
(priority inversion). Atomic flags avoid this entirely; the only
"shared" state is a single bool or a 2-element int array.

---

## 14. Multi-device porting (Tab5 ↔ Cardputer ADV)

The codebase supports both boards from a single source tree via
`#ifdef BOARD_TAB5` / `#ifdef BOARD_CARDPUTER` guards set by the
PlatformIO build flags.

### Adding a new environment

1. **Add an `[env:NAME]` section to `platformio.ini`** with the board,
   framework, upload speed, and a unique `-DBOARD_<NAME>` build flag.
2. **Add `#ifdef BOARD_<NAME>` guards** in the header/source for any
   hardware-specific constants:

   ```cpp
   #ifdef BOARD_TAB5
       static constexpr gpio_num_t PIN_I2S_MCK = GPIO_NUM_30;
       // ...
   #elif defined(BOARD_CARDPUTER)
       static constexpr gpio_num_t PIN_I2S_MCK = GPIO_NUM_NC;
       // ...
   #endif
   ```
3. **Conditionally include hardware-specific headers**:

   ```cpp
   #ifdef BOARD_TAB5
       #include <driver/gpio.h>
       #include <driver/i2s_std.h>
   #endif
   ```
4. **Provide a per-device display implementation** behind
   `DisplayInterface` if the screen geometry or input model differs.
5. **Keep shared logic device-agnostic** — `MorseEncoder`,
   `KeyEnvelop`, `MorseGenerator`, `IambicKeyer`, `StraightKeyer`,
   `MorseDecoder`, `MorseTable`, `morse_constants` should compile
   unchanged for any new device.

### Cardputer-specific notes

- **NS4168 mono I²S amp** — no digital volume control. Skip
  `applyCodecVolume()`; the software amplitude path is the only volume
  control.
- **No headphone detect** — `isHeadphoneInserted()` always returns
  `false`.
- **240×135 ST7789V2 display** — compact UI layout in `ui_layout.h`.
- **56-key matrix keyboard + Button A** — used instead of the touch
  screen for input.
- **`-DBOARD_CARDPUTER`** is added to the `esp32s3_cardputer` environment
  build flags. The `esp32-s3-devkitc-1` board definition is the closest
  supported match to the Cardputer's ESP32-S3 chip.

### Tab5-specific notes

- **ES8388 codec + PI4IOE5V6408 amp** — both required.
- **1280×800 capacitive touch display** — touch buttons in the top row.
- **`-DBOARD_TAB5`** plus the pioarduino platform pin (to dodge the
  MIPI-DSI backlight flicker regression).
- **`cfg.internal_spk = false`** before `M5.begin()` to keep
  `AudioEngine` in control of `I2S_NUM_0`.

---

## 15. cmorse reference (`extern/`)

The iambic keyer state machine and decoder ring buffer were ported from
the cmorse project (Alain M. Lafon, 2018, MIT licence) in `extern/`. The
port is line-for-line equivalent for the FSM transitions; the C++ side
adds:

- `std::atomic` key state for cross-core ISR ↔ audio-task communication
- A `KeyEnvelop` class that pre-computes the BH envelope tables once
- A shared `s_cachedAmplitude` so the audio task never calls
  `log10f` / `powf`

The four-symbol alphabet (`.`, `-`, `*`, ` `) is byte-for-byte compatible
with the cmorse decoder — symbols are written to the same ring buffer and
consumed by the same `MorseDecoder::accumulate` / `flush` / `print`
sequence.

`extern/` is built only when explicitly opted in (the native test build
does not link it). It serves as a readable reference for anyone trying to
understand the original cmorse timing decisions.
