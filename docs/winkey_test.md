# Testing the WinKeyer Interface

The K1EL WinKeyer 2.x (WK2) protocol that A1Keyer exposes is verified at
two levels:

1. **Host-side Python harness** (`scripts/winkey_host.py`) — drives the
   full WK2 byte stream a contest logger would send, asserts on every
   response, and cross-checks state via the dev HTTP console.
2. **C++ unit tests** (`test/test_winkey_bridge/`) — verify the protocol
   state machine on the host without any USB dependency.

This document covers the Python harness and the operational issues
around getting it to talk to the device. For the protocol reference
itself, see [`winkey.md`](winkey.md). For the build / upload / monitor
flow, see [`connecting.md`](connecting.md) and [`TESTING.md`](TESTING.md).

---

## Quick start

```bash
# 1. Device must be flashed and reachable on WiFi (default IP 192.168.10.200)
curl -s http://192.168.10.200/state | python3 -m json.tool

# 2. Plug the Cardputer into USB-C (or already plugged).
ls /dev/cu.usbmodem*        # macOS — should show one entry

# 3. Run the harness.
python3 scripts/winkey_host.py                       # full plan (27 tests)
python3 scripts/winkey_host.py --plan basic          # smoke test (12 tests)
python3 scripts/winkey_host.py --port /dev/cu.usbmodem2101
python3 scripts/winkey_host.py --no-http             # skip state cross-check
python3 scripts/winkey_host.py --ignore-errors       # continue past failures
```

The harness exits **0** on full pass, **1** on any failure, **2** on a
port-not-found error, **3** if the device is in Console mode (and so
bytes won't be parsed as WK2 — toggle with **D** on the keyboard and
re-run).

---

## What it tests

| Group | Tests |
|---|---|
| **Open** | `0x00 0x02` host-open returns the version byte (`0x17`); `0x00 0x0B` enters WK2 status mode. |
| **Status / pot** | `0x15` returns one byte with 3-MSB tag `110` (`0xC0..0xDF`); `0x07` returns `0x80` (no-physical-pot sentinel). |
| **Parameters** | `0x02` WPM, `0x01` sidetone (preset 5 → 800 Hz), `0x04` PTT times (2 params), `0x0E` setmode, `0x05` set-pot (3 params), `0x0F` load-defaults (15 params), `0x03` weighting, `0x0D` Farnsworth, `0x17` ratio, `0x06` pause/resume, `0x18`-`0x1F` buffered commands. |
| **Echo** | Text bytes are echoed back upper-cased (after keying). Command and parameter bytes are silent. |
| **Buffer ops** | `0x08` backspace is silent, `0x0A` clear is silent. |
| **Keying** | `0x0B 1` / `0x0B 0` flip `radioKeyingEnabled` on the device (verify via `/state`). |
| **Close / reset** | `0x00 0x03` closes the host; commands and text are then ignored. `0x00 0x01` resets to defaults (WPM 20, host-closed). |

Every "silent" command is verified by sending it and then sending a
**text** byte that must echo — that proves the parser consumed the
correct number of parameter bytes and didn't swallow the next byte as
a stray param. So a passing run means the parser is wired correctly
all the way from `Winkey::poll()` through `WinkeyBridge::feed()`.

The harness prints per-test **PASS** / **FAIL** with hex dumps of
every byte sent and received, and a summary tally. With
`--ignore-errors` it runs the whole plan even after the first
failure, which is useful when chasing a wiring bug that cascades.

---

## How verification works

The harness uses two channels in parallel:

| Channel | What it checks |
|---|---|
| **Wire (`/dev/cu.usbmodem*`)** | Bytes the device actually emits back: the version byte after host-open, the status byte after `0x15`, the pot sentinel after `0x07`, and the per-character echo after a text send. |
| **HTTP (`http://<device>/state`)** | Internal state changes that don't emit bytes: `winkeyWpm`, `winkeySidetoneHz`, `radioKeyingEnabled`, `winkeyOpen`. The HTTP `/log?n=200` endpoint also mirrors every `[WK2 RX]` / `[WK2 TX]` byte, so post-mortem analysis of a failed run is straightforward. |

If `--no-http` is passed, only the wire channel is used — useful when
testing against a build that doesn't include `ENABLE_WIFI_DEBUG`.

---

## Why the harness exists

The bridge code itself is host-unit-tested in `test/test_winkey_bridge/`
with spy callbacks — that covers the protocol state machine. The Python
harness covers what the unit tests **can't**:

- That `Winkey::poll()` is actually wired into `loop()`.
- That the USB-CDC RX path delivers bytes to the bridge (this is the
  bug that motivated writing the harness — see below).
- That the `Console::rawWinkeyWrite` path gets the device's reply bytes
  back out to the host.
- That every effect callback (`setWpm`, `setSidetoneHz`,
  `setOutputEnable`, `sendText`, `stopSending`) propagates through
  `MorseModel` / `AudioEngine` / `RadioKeyer` to the observable HTTP
  state.

So a passing harness run means the **whole stack** works, end to end,
not just the bridge.

---

## The USB-CDC startup issue (read this if RX "doesn't work")

There is a build-flag pitfall on ESP32-S3 that **silently breaks
host-to-device RX** while still showing every log line on the host's
USB monitor. It looks like RX is wired correctly (you see the device's
TX in `pio device monitor`), but bytes you send never reach the
bridge — no `[WK2 RX]` lines appear in `/log`, no version byte comes
back from host-open.

### What causes it

The Arduino core's `Serial` symbol aliases based on two flags:

```cpp
// cores/esp32/HardwareSerial.h
#if ARDUINO_USB_CDC_ON_BOOT
  #define Serial HWCDCSerial    // USB-Serial-JTAG CDC
#else
  #define Serial Serial0        // UART0 (hardware pins)
#endif
```

The Cardputer build needs the USB-Serial-JTAG CDC peripheral to actually
work (the WinKeyer bridge reads bytes from it), but it also needs the
Mac's CDC driver to enumerate cleanly without a manual reset. After
several false starts, the working combination turned out to be:

- `ARDUINO_USB_MODE=1` — single USB-Serial-JTAG port (no native USB
  stack, no port-switch fragility).
- `ARDUINO_USB_CDC_ON_BOOT=0` — the framework's
  `printBeforeSetupInfo()` does **not** call `Serial.begin()` early
  and does **not** touch the USB-Serial-JTAG peripheral during boot.
  That keeps the host's CDC driver bound to a stable JTAG-emitted
  descriptor and avoids the macOS enumeration race.
- We instantiate our own `HWCDC` in `src/console_io.cpp` and call
  `_cdc.begin(115200)` from `Console::begin()`, which `main.cpp`
  invokes **after** `M5.begin()` + `M5Cardputer.begin()` have settled
  pin ownership. The peripheral-manager pin-dance race that left the
  Mac's CDC driver bound to a stale descriptor (and forced a manual
  reset) only happens if CDC is brought up **before** M5.

### Why the prior `CDC_ON_BOOT=1` choice broke Mac enumeration

The earlier "fix" flipped `ARDUINO_USB_CDC_ON_BOOT=1` so that
`Serial` aliased to `HWCDCSerial` and the framework called
`Serial.begin()` (= `HWCDCSerial.begin()`) inside
`printBeforeSetupInfo()` — which runs **before** `setup()`. The
sequence on boot was:

1. `printBeforeSetupInfo()` → `Serial.begin()` reconfigures the
   USB-Serial-JTAG peripheral for CDC, sets D+ pullup, arms
   interrupts, claims USB pins 19/20 via `perimanSetPinBus()`.
2. `Serial.setDebugOutput(true)` reroutes framework `log_*` calls
   through the CDC TX ring buffer.
3. The framework prints the chip-debug report (~2 KB of text) into
   the 256-byte CDC TX ring. The Mac hasn't finished enumeration
   yet, isn't reading, and the ring fills up.
4. `setup()` runs. `M5.begin()` and `M5Cardputer.begin()` may try to
   release/reclaim GPIO 19/20 via the peripheral manager. The CDC
   `perimanSetPinBus()` registration gets torn down and the device
   has to re-enumerate.
5. The Mac's CDC driver is now bound to a stale interface descriptor.
   Until a bus reset (DTR pulse, RST button), the host's view of the
   device doesn't match its real state, and any host-side serial
   open fails.

The "2 second wait in printBeforeSetupInfo" the original commit
message mentioned is a separate, bounded delay, not the cause of the
manual-reset symptom.

### The current fix (late CDC init)

In `platformio.ini`:

```
build_flags =
    ...
    -DARDUINO_USB_CDC_ON_BOOT=0
    -DARDUINO_USB_MODE=1
```

In `src/console_io.cpp` we declare a private `HWCDC _cdc` instance
(no `HWCDCSerial` global is defined when `CDC_ON_BOOT=0`), and
`Console::begin()` calls `_cdc.begin(115200)`. All Console I/O goes
through `_cdc` — the framework's `Serial` (Serial0 / UART0) is
intentionally untouched.

In `src/main.cpp::setup()`:

```cpp
M5.begin(cfg);
M5Cardputer.begin(true);   // settle pin ownership
Console::begin();          // late CDC init
```

We deliberately do **not** call `_cdc.setDebugOutput(true)`. That
would reroute framework `log_*` calls into our CDC TX, bypassing the
mode gate and corrupting the WK2 stream when a framework log fires
during a WinKey session. Framework logs continue to flow via the
chip's default `ets_putc2` path (UART0 TX → USB-Serial-JTAG mirror),
which is independent of our CDC.

If you see the manual-reset symptom again after this change, check
`src/console_io.cpp` for an accidental `Serial.X` call (everything
should go through `_cdc.X`) and `src/main.cpp::setup()` to confirm
`Console::begin()` still runs **after** `M5Cardputer.begin()`.

---

## Interpreting a failed run

A typical failure mode and what to check:

| Symptom in harness output | Where to look |
|---|---|
| All tests fail with `(nothing)` back, `/log` shows no `[WK2 RX]` lines | The CDC startup issue above — `Console::begin()` not running, or `Console::write/read/available` is using `Serial.X` instead of `_cdc.X`. |
| `host_open_returns_0x17` fails with `got 0xNN` (not 0x17) | Version byte mismatch. A1Keyer returns `0x17` (WK2 rev 2.3); some hosts accept `>= 0x10` as "a WinKeyer" but if the value drifted, update `kVersion` in `src/winkey_bridge.h`. |
| `host_close_silent` fails with `HTTP winkeyOpen=True` | Bridge didn't close — likely `handleAdmin(ADMIN_HOST_CLOSE)` not setting `_open=false` (shouldn't happen on current code; check `src/winkey_bridge.cpp`). |
| `req_status_shape` fails with `3-MSB=0xE0` (or other non-`0xC0` tag) | Status byte tag wrong — `statusByte()` base in `src/winkey_bridge.cpp` has drifted. |
| `send_text_*_echo` fails with `echoed 0/N bytes` | The `cbSendText` hook's echo loop in `src/winkey.cpp:59-61` isn't running, or `Winkey::poll()` isn't being called from `loop()`. |
| HTTP state never changes (`winkeyWpm` stays at 20) | A parameter was stored in the bridge but the effect callback (`cbSetWpm`) wasn't fired. Check `src/winkey_bridge.cpp::applyCommand` for that command. |

For a permanent record, capture both the harness output and a
`curl -s http://<device>/log?n=200` immediately after the failure —
the `[WK2 RX]` / `[WK2 TX]` trace in the log will pinpoint whether
the bytes left the host, whether they reached the bridge, and whether
the bridge emitted the expected reply.

---

## Adding new tests

Each test is a function in `scripts/winkey_host.py` with the
signature `fn(host: WinkeyHost, http: HttpConsole, p: Printer) ->
TestResult`. The function:

1. Calls `host.exchange(send_bytes, expect=N, timeout=...)` to send
   bytes and read replies. Returns `(sent, received)`.
2. Optionally calls `http.state()` to cross-check observable state.
3. Returns a `TestResult(name=..., passed=..., detail=..., sent=...,
   received=..., http_state=...)`.

Then add the function to one of the two plans at the bottom of the
file:

- `BASIC_PLAN` — short smoke test (~12 tests) for a quick "is it
  alive?" check.
- `ALL_PLAN` — comprehensive sweep (~27 tests).

Use the existing tests as templates; the `ptt_times_two_params`,
`load_defaults_consumes`, and `set_pot_3_params_then_text` cases
demonstrate the "send N params then a text byte and assert the text
echoed" pattern that proves parameter-count parsing is correct.

---

## Cross-references

- [`winkey.md`](winkey.md) § 16.5 — C++ unit-test layout for the bridge
  itself; matches the structure of `test/test_winkey_bridge/`.
- [`winkey.md`](winkey.md) § 17.2 — the original spec for this
  harness (cross-checked against the implementation in
  `scripts/winkey_host.py`).
- [`TESTING.md`](TESTING.md) — host-side C++ test runner (`./run_tests.sh`).
- [`connecting.md`](connecting.md) — USB cable, mode toggle, port
  enumeration.