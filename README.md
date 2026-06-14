# A1Keyer

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Changelog](https://img.shields.io/badge/changelog-0.1.0-blue.svg)](CHANGELOG.md)

Low-latency Morse code trainer firmware for M5Stack devices. Plays, decodes, and
displays CW through a paddle or straight key with sub-3 ms audio latency, and
renders decoded text on the device screen. **Current release: v0.1.0.**

| Device | MCU | Audio | Display | Input | Env |
|---|---|---|---|---|---|
| **M5Stack Cardputer ADV** | ESP32-S3 @ 240 MHz | NS4168 I2S amp (mono) | 1.14" 240×135 ST7789V2 | 56-key matrix + Button A + GPIO paddle (Grove) | `esp32s3_cardputer` |

The Cardputer target shares all platform-independent code (`morse_encoder`,
`key_envelop`, `morse_generator`, `iambic_keyer`, `straight_keyer`,
`morse_decoder`, `morse_constants`, `MorseTable`, `Log`). A Tab5 (ESP32-P4)
target also exists in the source tree but is not part of this release — see
the CHANGELOG for the rationale.

---

## Features

- **Iambic B keyer** — dual-paddle squeeze keying with proper SWAP / AUTOREPEAT /
  END-OF-CHAR logic and 7-dit word-space detection.
- **Straight key** — single-contact keying with bounce-guard verification and
  adaptive DIT/DAH classification based on a rolling median.
- **CW decoder** — turns paddle activity into text via a lock-free ring buffer
  and a 200-character circular display buffer with per-character player/keyer
  color attributes.
- **Text playback (encoder)** — `playText("…")` renders typed text as CW
  through the same audio path, with playback characters rendered in a
  distinct color in the display buffer.
- **Click-free envelope** — Blackman-Harris raised-cosine ramps (5 ms default)
  baked into the keyer envelopes eliminate keying transients.
- **Logarithmic volume** — 0–100 % mapped to decibels, applied as PCM
  amplitude scaling before I2S. The Cardputer's NS4168 has no digital
  volume control, so volume is software-only.
- **Settings** — WPM, tone frequency, volume, and keyer type (Paddle/Straight)
  are adjustable from the keyboard (Cardputer) and persist to NVS via
  `Preferences`. WPM and frequency are mutable while the audio task is running.
- **Screen-saver** — display dims after 5 minutes of inactivity; any key
  press or paddle touch wakes it and resets the timer.

---

## Repository layout

```
.
├── src/
│   ├── main.cpp                  # setup() / loop() — UI + keyboard input
│   ├── audio_engine.{h,cpp}      # I2S + NS4168 + FreeRTOS audio task
│   ├── iambic_keyer.{h,cpp}      # Iambic B state machine + decoder ring buffer
│   ├── straight_keyer.{h,cpp}    # Straight-key state machine + adaptive classifier
│   ├── morse_key.{h,cpp}         # GPIO ISR paddle input → atomic KeyState
│   ├── key_envelop.{h,cpp}       # Blackman-Harris envelope tables
│   ├── morse_encoder.{h,cpp}     # Text → DIT/DAH/CHAR_SPACE/WORD_SPACE sequence
│   ├── morse_generator.{h,cpp}   # Async player: encoder + envelope + sine gen
│   ├── morse_decoder.{h,cpp}     # CW-symbol ring buffer → characters
│   ├── MorseTable.{h,cpp}        # Binary-search Morse alphabet
│   ├── morse_constants.h         # Shared timing + symbol constants
│   ├── fast_math.{h,cpp}         # LUT-based sin() for the audio task
│   ├── display_model.{h,cpp}     # Atomic singleton: text buffer, settings
│   ├── display_task.{h,cpp}      # Core 0 render loop (~20 fps)
│   ├── display_interface.h       # Abstract display interface
│   ├── cardputer_display.{h,cpp} # Cardputer display implementation
│   ├── ui_layout.h               # Per-device button geometry
│   └── Log.h                     # Lightweight serial logger
├── test/
│   ├── test_framework.h          # Self-contained CHECK / RUN macros
│   ├── mocks/Arduino.h           # Serial stub (no ESP32 dependency)
│   ├── test_morse_encoder/       # Alphabet + spacing
│   ├── test_key_envelop/         # Envelope layout, WPM, ramp cap
│   ├── test_morse_generator/     # Sample stream, idle behaviour
│   ├── test_iambic_keyer/        # SWAP / AUTOREPEAT / END-OF-CHAR
│   ├── test_straight_keyer/      # Bounce guard, adaptive classifier
│   ├── test_morse_decoder/       # Symbol buffer → character conversion
│   └── test_display_model/       # Circular buffer + colour attributes
├── docs/
│   └── ARCHITECTURE.md           # Deep-dive: audio path, keyer FSMs, etc.
├── extern/                       # cmorse reference C source (build only)
├── CMakeLists.txt                # Native test build (MinGW / GCC / Clang)
├── platformio.ini                # PlatformIO environments
├── project.txt                   # Project description for PlatformIO
├── run_tests.sh                  # Convenience wrapper for native test build
└── .gitignore
```

---

## Building and flashing

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

---

## Running the unit tests (PC / native)

The unit tests compile the platform-independent library sources against a
minimal Arduino stub (`test/mocks/Arduino.h`) — no ESP32 toolchain required.
They are registered with CMake's CTest framework, so a single command
configures, builds, and runs the entire suite cross-platform.

**Prerequisites:** CMake ≥ 3.14 and a C++17 compiler
(GCC, Clang, AppleClang, or MSVC/MinGW on Windows).

### Quick start

```bash
cmake -B build                          # configure
cmake --build build                     # compile all test binaries
ctest --test-dir build --output-on-failure   # run the suite
```

CMake auto-selects a generator based on the host:

| Platform | Generator | Notes |
|---|---|---|
| macOS / Linux | Unix Makefiles (default) | `cmake -G Ninja …` works too if Ninja is installed. |
| Windows (MSVC) | MSBuild / Ninja | Run from a *Developer Command Prompt for VS* so the MSVC compiler is on `PATH`. |
| Windows (MinGW) | MinGW Makefiles | `cmake -B build -G "MinGW Makefiles"`. |

### Expected output

A successful run prints one line per suite and a final summary:

```
Test project /Users/thomas/devel/arduino/dit_dah_esp/build
    Start  1: test_morse_encoder
1/6 Test #1: test_morse_encoder .............   Passed    0.02 sec
    Start  2: test_key_envelop
2/6 Test #2: test_key_envelop ...............   Passed    0.01 sec
    Start  3: test_morse_generator
3/6 Test #3: test_morse_generator ...........   Passed    0.20 sec
    Start  4: test_iambic_keyer
4/6 Test #4: test_iambic_keyer ..............   Passed    0.27 sec
    Start  5: test_morse_decoder
5/6 Test #5: test_morse_decoder .............   Passed    0.22 sec
    Start  6: test_display_model
6/6 Test #6: test_display_model .............   Passed    0.22 sec

100% tests passed, 0 tests failed out of 6

Total Test time (real) =   1.0 sec
```

CTest shows pass/fail per binary. To see the per-assertion detail
(`pass test_xxx` lines and the `N/N assertions passed` summary each
binary prints), either run the binary directly:

```bash
./build/test_morse_encoder     # macOS / Linux
build\test_morse_encoder.exe   # Windows
```

…or stream every test's output with `ctest -V`:

```bash
ctest --test-dir build -V
```

### Useful CTest options

| Flag | Effect |
|---|---|
| `-V` | Stream each test's stdout/stderr as it runs. |
| `--output-on-failure` | Stream output only on failure. |
| `-j N` | Run N tests in parallel. |
| `-R regex` | Run only tests whose name matches the regex. |
| `-E regex` | Exclude tests matching the regex. |
| `--repeat until-fail:3` | Run the suite up to 3 times, stopping on first failure. |

### Adding a new test suite

1. Drop `test/<name>/test_<name>.cpp` and use the helpers from
   `test/test_framework.h` (`CHECK`, `CHECK_EQ`, `RUN`, `test_summary`).
2. Register it in `CMakeLists.txt` — add `test_<name>` to the
   `foreach(SUITE IN ITEMS …)` list. CTest picks it up on the next
   configure; no other wiring is required.

---

## Architecture

### Audio path

```
Text string  ─OR─  Paddle / Straight key
      │                       │
      ▼                       ▼
MorseEncoder         IambicKeyer / StraightKeyer
  (Text → Element)   (FSM + KeyEnvelop)
      │                       │
      └─────────┬─────────────┘
                ▼
          KeyEnvelop        ──  Blackman-Harris envelope tables
                │              per element (DIT 2×ditLen, DAH 4×ditLen)
                ▼
       MorseGenerator /      ──  envelope × sine LUT → int16 samples
         IambicKeyer /
         StraightKeyer
                │
                ▼
       AudioEngine           ──  FreeRTOS task (Core 1, priority 22) feeds
                │              2 × 64-frame DMA blocks to the I2S
                │              peripheral at 48 kHz
                ▼
            NS4168 (I2S amp)
                │
                ▼
              Speaker
```

The audio task is the only producer of DMA samples. All upstream state
changes (paddle edges, settings updates) cross the audio boundary through
`std::atomic` flags only — no mutexes in the real-time path.

### Timing (ITU-R M.1677 / Paris standard)

All durations are multiples of one **dit unit** (`dit_ms = 1200 / wpm`).
Constants are defined in `src/morse_constants.h`.

| Element | Units | Key state | Notes |
|---------|-------|-----------|-------|
| Dit (mark) | 1 | down | |
| Dah (mark) | 3 | down | |
| Element space | 1 | up | between dits/dahs within a character — baked into the envelope |
| Char space | 3 | up | between characters |
| Word space | 7 | up | between words — detected when gap > 7 ditt at element start |

| Speed | 1 dit | 1 dah | Char space | Word space |
|-------|-------|-------|------------|------------|
| 20 WPM | 60 ms | 180 ms | 180 ms | 420 ms |
| 25 WPM | 48 ms | 144 ms | 144 ms | 336 ms |

### Latency

```
DMA ring: 2 buffers × 64 frames @ 48 kHz = 2.67 ms worst-case output latency
```

A paddle press propagates to a tone on the speaker in < 3 ms
(1.45 ms max-buffer remaining + analog settling).

---

## Volume control

Volume is a percentage 0–100 (default 50) stored on the `AudioEngine` as
`std::atomic<int>` and applied independently on each output path.

| Path | Implementation |
|------|----------------|
| **Speaker** (Cardputer) | PCM sample amplitude is scaled in `fillBuffer()` before I2S |

Mapping: `dB = 20 × log10(percent / 100)`, then `amp = 32767 × 10^(dB / 20)`.
A pre-computed `s_cachedAmplitude` is read by the audio task — no
floating-point math in the real-time path.

The Cardputer's NS4168 has no digital volume, so the only attenuation is
the software amplitude path above.

---

## Key source files

| File | Responsibility |
|------|----------------|
| [src/audio_engine.h](src/audio_engine.h) | Hardware init, volume API, FreeRTOS audio task |
| [src/morse_key.h](src/morse_key.h) | Paddle ISR, atomic `KeyState` exchange |
| [src/iambic_keyer.h](src/iambic_keyer.h) | Iambic B FSM, decoder ring buffer |
| [src/straight_keyer.h](src/straight_keyer.h) | Straight-key FSM, adaptive classifier |
| [src/morse_encoder.h](src/morse_encoder.h) | Text → element sequence; `encode()`, `ditLengthSec()` |
| [src/morse_generator.h](src/morse_generator.h) | Async player; `playText()`, `fillSamplesMono()` |
| [src/key_envelop.h](src/key_envelop.h) | Blackman-Harris envelope tables; `setWPM()` |
| [src/morse_decoder.h](src/morse_decoder.h) | Symbol buffer → character output |
| [src/morse_constants.h](src/morse_constants.h) | Shared timing + symbol constants |
| [src/display_model.h](src/display_model.h) | Atomic state singleton, text buffer |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Deep-dive: register maps, FSMs, DMA details |

---

## Test infrastructure

Tests use a self-contained header (`test/test_framework.h`) with no external
dependencies. Available macros:

| Macro | Purpose |
|-------|---------|
| `CHECK(cond)` | Fail if condition is false |
| `CHECK_EQ(a, b)` | Fail if `a != b` (prints both values) |
| `CHECK_NEAR(a, b, eps)` | Fail if `|a - b| > eps` |
| `CHECK_STR_EQ(a, b)` | Fail if strings differ |
| `CHECK_NOT_NULL(p)` | Fail if pointer is null |
| `RUN(fn)` | Run a test function and report pass/fail |
| `test_summary()` | Print totals; returns 0 (pass) or 1 (fail) |

`test/mocks/Arduino.h` provides a no-op `Serial` stub so the source files can
call `Serial.println()` / `Serial.printf()` without linking against any ESP32
library.

---

## References

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — deep-dive: register
  maps, Blackman-Harris envelope maths, keyer FSMs, multi-device porting notes
- [extern/](extern/) — upstream `cmorse` reference (Alain M. Lafon, 2018)
  from which the iambic keyer state machine was ported
