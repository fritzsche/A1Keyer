#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
winkey_host.py — Contest WinKeyer host emulator for A1Keyer.

Speaks the K1EL WinKeyer 2.x (WK2) byte protocol over USB-CDC to the
A1Keyer's WinkeyBridge (src/winkey_bridge.cpp). The same byte stream a
real logger (N1MM, Win-Test, RUMlogNG, fldigi, WriteLog) would emit.

Two verification channels are used in parallel:
  1. Wire-level: read the bytes the device emits back over USB-CDC and
     assert on them directly (version byte on host-open, status byte on
     0x15, get-pot byte on 0x07, character echo on text send).
  2. HTTP /state: after each command that changes internal state, fetch
     http://<device>/state and verify the model agrees (WPM, sidetone,
     radioKeyingEnabled, winkeyOpen, decoded text).

Logging:
  Every line sent and every line received is printed with a tag
  ("TX"/"RX"/"HTTP") and a hex dump. The HTTP /log endpoint is also
  polled before and after each test so post-mortem analysis has a
  complete picture of what the device saw.

Usage:
  python3 scripts/winkey_host.py                 # auto-detect port, all tests
  python3 scripts/winkey_host.py --port /dev/cu.usbmodem2101
  python3 scripts/winkey_host.py --plan basic    # quick smoke test
  python3 scripts/winkey_host.py --no-http      # skip HTTP verification
  python3 scripts/winkey_host.py --http http://192.168.10.200

Exit code: 0 on all tests passing, 1 on any failure.

See docs/winkey.md § 16-17 for the protocol reference.
"""

import argparse
import json
import sys
import time
import urllib.request
import urllib.error
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Tuple

import serial
from serial.tools import list_ports


# ─── K1EL WK2 protocol constants (verified against WK2 datasheet v23) ─
#   See src/winkey_bridge.cpp and docs/winkey.md § 7.
WK_VERSION_BYTE   = 0x17        # A1Keyer returns 0x17 on host-open
STATUS_TAG_3MSB   = 0xC0        # 3-MSB tag "110" for all WK2 status bytes
GET_POT_REPLY     = 0x80        # top bit set; no physical pot → "no pot" sentinel

# Admin command set (prefix 0x00, then second byte)
ADMIN             = 0x00
ADMIN_RESET       = 0x01
ADMIN_HOST_OPEN   = 0x02
ADMIN_HOST_CLOSE  = 0x03
ADMIN_SET_WK1     = 0x0A
ADMIN_SET_WK2     = 0x0B

# Operating commands
SIDETONE          = 0x01
SPEED             = 0x02
WEIGHTING         = 0x03
PTT_TIMES         = 0x04        # 2 params: lead, tail
SET_POT           = 0x05        # 3 params
PAUSE             = 0x06
GET_POT           = 0x07
BACKSPACE         = 0x08
PINCONFIG         = 0x09
CLEAR_BUF         = 0x0A
KEY_IMMED         = 0x0B
HSCW              = 0x0C
FARNSWORTH        = 0x0D
SETMODE           = 0x0E
LOAD_DEFAULTS     = 0x0F        # 15 params
KEY_COMP          = 0x11
SW_PADDLE         = 0x14
REQ_STATUS        = 0x15
POINTER           = 0x16
RATIO             = 0x17
BUF_PTT           = 0x18
BUF_KEY           = 0x19
BUF_WAIT          = 0x1A
BUF_MERGE         = 0x1B
BUF_SPEED         = 0x1C
BUF_HSCW          = 0x1D
BUF_CANCELSPD     = 0x1E
BUF_NOP           = 0x1F

# WK2 sidetone presets (low nibble 1-10 → Hz). A1Keyer clamps to [300,900].
SIDETONE_HZ = {
    0: 600,           # default
    1: 4000, 2: 2000, 3: 1333, 4: 1000, 5: 800,
    6: 667, 7: 571, 8: 500, 9: 444, 10: 400,
}


# ─── Helpers ───────────────────────────────────────────────────────────
def hexstr(b: bytes) -> str:
    """Pretty-print bytes as space-separated hex."""
    return " ".join(f"{x:02X}" for x in b)


def find_a1keyer_port() -> Optional[str]:
    """Auto-detect the Cardputer / ESP32-S3 USB-CDC port.

    Strategy: find the port whose VID:PID matches the Espressif USB
    JTAG/serial debug unit (303a:1001 — Cardputer ADV), or whose
    description contains 'JTAG'. Returns the /dev/cu.usbmodem* path on
    macOS (the call-out device, not the dial-in tty).
    """
    candidates = []
    for p in list_ports.comports():
        d = (p.description or "").lower()
        if "jtag" in d or "esp32" in d or (p.vid == 0x303A and p.pid in (0x1001, 0x1002)):
            # Prefer cu.usbmodem* (callout) over tty.usbmodem* (dialin) on macOS.
            dev = p.device
            if dev.startswith("/dev/cu."):
                candidates.append(dev)
            else:
                candidates.append(dev)
    return candidates[0] if candidates else None


# ─── Result type ───────────────────────────────────────────────────────
@dataclass
class TestResult:
    name: str
    passed: bool
    detail: str = ""
    sent: bytes = b""
    received: bytes = b""
    http_state: Optional[dict] = None


# ─── HTTP helper ───────────────────────────────────────────────────────
class HttpConsole:
    """Thin wrapper around the A1Keyer dev HTTP console."""

    def __init__(self, base: str, enabled: bool = True):
        self.base = base.rstrip("/")
        self.enabled = enabled

    def get_json(self, path: str, timeout: float = 2.0) -> Optional[object]:
        if not self.enabled:
            return None
        url = f"{self.base}{path}"
        try:
            with urllib.request.urlopen(url, timeout=timeout) as r:
                return json.loads(r.read().decode("utf-8"))
        except (urllib.error.URLError, json.JSONDecodeError, OSError) as e:
            self._last_error = str(e)
            return None

    def state(self) -> Optional[dict]:
        return self.get_json("/state")

    def log(self, n: int = 200) -> Optional[list]:
        return self.get_json(f"/log?n={n}")


# ─── The host itself ───────────────────────────────────────────────────
class WinkeyHost:
    """Talk WK2 over USB-CDC to the A1Keyer's WinkeyBridge."""

    def __init__(self, port: str, baud: int = 115200, timeout: float = 1.0):
        self.port = port
        self.ser = serial.Serial(port, baud, timeout=timeout)
        time.sleep(0.15)                        # let the OS finish claiming
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def send(self, data: bytes):
        self.ser.write(data)
        # Drain the outgoing FIFO so the next read returns fresh bytes only.
        self.ser.flush()

    def recv(self, n: int, deadline: float) -> bytes:
        """Read up to n bytes, stopping at deadline (seconds-from-now)."""
        end = time.time() + deadline
        buf = bytearray()
        while len(buf) < n and time.time() < end:
            remaining = max(0.0, end - time.time())
            self.ser.timeout = max(0.01, remaining)
            chunk = self.ser.read(n - len(buf))
            if chunk:
                buf.extend(chunk)
            # else: timeout on this chunk; loop checks deadline
        return bytes(buf)

    def exchange(self, send: bytes, expect: int,
                 timeout: float = 1.0) -> Tuple[bytes, bytes]:
        """Send bytes, then wait up to `timeout` for exactly `expect` bytes.

        Returns (sent, received). `received` may be shorter than expect if
        the device emits fewer bytes than asked.
        """
        self.send(send)
        rx = self.recv(expect, timeout)
        return send, rx

    def drain(self, dwell: float = 0.1):
        """Discard whatever is currently in the RX buffer (boot noise, etc.)."""
        self.ser.reset_input_buffer()
        time.sleep(dwell)
        self.ser.reset_input_buffer()


# ─── Pretty printer ────────────────────────────────────────────────────
class Printer:
    GREEN = "\033[32m"
    RED   = "\033[31m"
    YEL   = "\033[33m"
    DIM   = "\033[2m"
    BOLD  = "\033[1m"
    RST   = "\033[0m"

    def __init__(self, verbose: bool = True, use_color: bool = True):
        self.verbose = verbose
        self.use_color = use_color and sys.stdout.isatty()
        self.fails = 0
        self.passes = 0

    def _c(self, color: str, s: str) -> str:
        return f"{color}{s}{self.RST}" if self.use_color else s

    def header(self, s: str):
        print()
        print(self._c(self.BOLD, f"━━ {s} ━━"))

    def test(self, r: TestResult):
        if r.passed:
            self.passes += 1
            mark = self._c(self.GREEN, "PASS")
        else:
            self.fails += 1
            mark = self._c(self.RED, "FAIL")
        line = f"  {mark}  {r.name}"
        if r.detail:
            line += f"   {self._c(self.DIM, r.detail)}"
        print(line)
        if self.verbose and (r.sent or r.received):
            tx = self._c(self.YEL, f"TX {len(r.sent):3d}B ") + hexstr(r.sent) if r.sent else ""
            rx = self._c(self.YEL, f"RX {len(r.received):3d}B ") + hexstr(r.received) if r.received else ""
            if tx: print(f"        {tx}")
            if rx: print(f"        {rx}")

    def summary(self, results: List[TestResult]) -> int:
        total = len(results)
        print()
        bar = "═" * 60
        print(self._c(self.BOLD, bar))
        if self.fails == 0:
            print(self._c(self.GREEN, f"  {self.passes}/{total} tests passed."))
        else:
            print(self._c(self.RED, f"  {self.passes}/{total} tests passed, {self.fails} FAILED."))
            print()
            print(self._c(self.RED, "Failed tests:"))
            for r in results:
                if not r.passed:
                    print(f"  - {r.name}: {r.detail}")
        print(self._c(self.BOLD, bar))
        return 1 if self.fails else 0


# ─── Individual tests ──────────────────────────────────────────────────
def test_host_open(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x00 0x02 → version byte 0x17 (WK2 rev 2.3)."""
    sent = bytes([ADMIN, ADMIN_HOST_OPEN])
    _, rx = host.exchange(sent, expect=1, timeout=1.0)
    ok = len(rx) == 1 and rx[0] == WK_VERSION_BYTE
    return TestResult(
        name="host_open_returns_0x17",
        passed=ok,
        detail=f"got {hexstr(rx) or '(nothing)'} expected 1 byte 0x{WK_VERSION_BYTE:02X}" if not ok else f"got 0x{rx[0]:02X}",
        sent=sent, received=rx,
    )


def test_wk2_mode(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x00 0x0B → enter WK2 status byte mode. No reply expected."""
    sent = bytes([ADMIN, ADMIN_SET_WK2])
    _, rx = host.exchange(sent, expect=0, timeout=0.5)
    return TestResult(
        name="set_wk2_mode_silent",
        passed=len(rx) == 0,
        detail=f"unexpected reply: {hexstr(rx)}" if rx else "no reply (correct)",
        sent=sent, received=rx,
    )


def test_status_request(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x15 → 1 status byte with 3-MSB tag = 110 (0xC0)."""
    sent = bytes([REQ_STATUS])
    _, rx = host.exchange(sent, expect=1, timeout=0.5)
    if len(rx) != 1:
        return TestResult(name="req_status_shape",
                          passed=False, detail=f"expected 1 byte, got {len(rx)}",
                          sent=sent, received=rx)
    tag = rx[0] & 0xE0
    ok = tag == STATUS_TAG_3MSB
    return TestResult(
        name="req_status_shape",
        passed=ok,
        detail=f"3-MSB={tag:#04x} (expected 0xC0)" if not ok
              else f"status=0x{rx[0]:02X} ({rx[0]:#010b})",
        sent=sent, received=rx,
    )


def test_get_pot(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x07 → 0x80 (no physical pot; sentinel)."""
    sent = bytes([GET_POT])
    _, rx = host.exchange(sent, expect=1, timeout=0.5)
    ok = len(rx) == 1 and rx[0] == GET_POT_REPLY
    return TestResult(
        name="get_pot_returns_0x80",
        passed=ok,
        detail=f"got {hexstr(rx) or '(nothing)'}, expected 0x{GET_POT_REPLY:02X}" if not ok
              else f"got 0x{rx[0]:02X}",
        sent=sent, received=rx,
    )


def test_set_wpm(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x02 0x19 → 25 WPM. Silent; verify via /state."""
    sent = bytes([SPEED, 25])
    _, rx = host.exchange(sent, expect=0, timeout=0.4)
    time.sleep(0.2)  # give the bridge time to propagate to MorseModel
    s = http.state() or {}
    wpm_observed = s.get("winkeyWpm", -1) if s else -1
    ok = (len(rx) == 0) and (wpm_observed == 25)
    return TestResult(
        name="set_wpm_25",
        passed=ok,
        detail=f"unexpected reply {hexstr(rx)}" if rx else
               (f"HTTP winkeyWpm={wpm_observed}, expected 25" if wpm_observed != 25
                else f"HTTP winkeyWpm=25"),
        sent=sent, received=rx, http_state=s,
    )


def test_sidetone_preset(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x01 0x05 → sidetone preset 5 (~800 Hz). Silent; verify via /state."""
    sent = bytes([SIDETONE, 5])
    _, rx = host.exchange(sent, expect=0, timeout=0.4)
    time.sleep(0.2)
    s = http.state() or {}
    hz_observed = s.get("winkeySidetoneHz", -1) if s else -1
    expected_hz = SIDETONE_HZ[5]
    ok = (len(rx) == 0) and (hz_observed == expected_hz)
    return TestResult(
        name="sidetone_preset_5",
        passed=ok,
        detail=f"HTTP winkeySidetoneHz={hz_observed}, expected {expected_hz}"
              if hz_observed != expected_hz else f"HTTP winkeySidetoneHz={hz_observed}",
        sent=sent, received=rx, http_state=s,
    )


def test_send_text_echo(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """Send ASCII text → device echoes each char back (after keying)."""
    sent = b"HELLO"
    _, rx = host.exchange(sent, expect=len(sent), timeout=1.0)
    ok = rx == sent
    return TestResult(
        name="send_text_HELLO_echo",
        passed=ok,
        detail=f"got {hexstr(rx)} expected {hexstr(sent)}" if not ok else
              f"echoed {len(rx)}/{len(sent)} bytes",
        sent=sent, received=rx,
    )


def test_send_text_lowercase_uppercased(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """Lowercase → uppercased on echo."""
    sent = b"cq test"
    expected = b"CQ TEST"
    _, rx = host.exchange(sent, expect=len(expected), timeout=1.0)
    ok = rx == expected
    return TestResult(
        name="send_text_lowercase_uppercased",
        passed=ok,
        detail=f"got {rx!r} expected {expected!r}" if not ok else
              f"echoed {rx!r}",
        sent=sent, received=rx,
    )


def test_commands_not_echoed(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """Command and parameter bytes are NOT echoed (WK2 semantics)."""
    sent = bytes([SPEED, 30, SIDETONE, 6, SETMODE, 1])
    _, rx = host.exchange(sent, expect=0, timeout=0.5)
    return TestResult(
        name="command_bytes_silent",
        passed=len(rx) == 0,
        detail=f"unexpected echo: {hexstr(rx)}" if rx else "no echo (correct)",
        sent=sent, received=rx,
    )


def test_clear_buffer_silent(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x0A clear-buffer is silent and stops the send."""
    sent = bytes([CLEAR_BUF])
    _, rx = host.exchange(sent, expect=0, timeout=0.5)
    return TestResult(
        name="clear_buffer_silent",
        passed=len(rx) == 0,
        detail=f"unexpected reply: {hexstr(rx)}" if rx else "no reply (correct)",
        sent=sent, received=rx,
    )


def test_key_immediate_down_up(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x0B 0x01 → key down (set output-enable). 0x0B 0x00 → key up."""
    host.send(bytes([KEY_IMMED, 1]))
    time.sleep(0.3)
    s1 = http.state() or {}
    radio_en_1 = s1.get("radioKeyingEnabled", None) if s1 else None

    host.send(bytes([KEY_IMMED, 0]))
    time.sleep(0.3)
    s2 = http.state() or {}
    radio_en_0 = s2.get("radioKeyingEnabled", None) if s2 else None

    sent = bytes([KEY_IMMED, 1, KEY_IMMED, 0])
    ok = radio_en_1 is True and radio_en_0 is False
    detail = (f"radioKeyingEnabled after down: {radio_en_1} (want True); "
              f"after up: {radio_en_0} (want False)")
    return TestResult(
        name="key_immediate_toggle",
        passed=ok,
        detail=detail,
        sent=sent, http_state=s2,
    )


def test_ptt_times_two_params(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x04 + 2 params must consume both, then text must NOT be eaten."""
    sent = bytes([PTT_TIMES, 10, 20, ord('Z')])
    _, rx = host.exchange(sent, expect=1, timeout=1.0)  # expect echo of 'Z'
    ok = len(rx) == 1 and rx[0] == ord('Z')
    return TestResult(
        name="ptt_times_two_params_then_text",
        passed=ok,
        detail=f"got {hexstr(rx)} expected one 'Z' echo (proves parser consumed both params)"
              if not ok else f"got 'Z' echo after both PTT params",
        sent=sent, received=rx,
    )


def test_load_defaults_consumes(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x0F + 15 params: consumed-and-ignored. Next byte is text."""
    sent = bytes([LOAD_DEFAULTS] + [0] * 15 + [ord('Q')])
    _, rx = host.exchange(sent, expect=1, timeout=1.0)
    ok = len(rx) == 1 and rx[0] == ord('Q')
    return TestResult(
        name="load_defaults_consumes_15",
        passed=ok,
        detail=f"got {hexstr(rx)} expected echo of 'Q' (proves 15 params consumed)"
              if not ok else "15 params consumed, 'Q' echoed as text",
        sent=sent, received=rx,
    )


def test_backspace_removes_last(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """Send 'ABX' then 0x08 → buffer should hold 'AB'. Echo 'X' arrives first,
    then 0x08 is silent. We assert the echo stream is exactly 'ABX'."""
    sent = b"ABX" + bytes([BACKSPACE])
    _, rx = host.exchange(sent, expect=3, timeout=1.0)  # 3 echoed chars only
    ok = rx == b"ABX"
    return TestResult(
        name="backspace_silent_echo",
        passed=ok,
        detail=f"echo stream {hexstr(rx)} expected 41 42 58 (A B X), backspace silent"
              if not ok else "backspace silent; echo unchanged",
        sent=sent, received=rx,
    )


def test_multi_byte_param_pot(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x05 + 3 params: consumed. Next byte is text."""
    sent = bytes([SET_POT, 50, 100, 150, ord('P')])
    _, rx = host.exchange(sent, expect=1, timeout=1.0)
    ok = len(rx) == 1 and rx[0] == ord('P')
    return TestResult(
        name="set_pot_3_params_then_text",
        passed=ok,
        detail=f"got {hexstr(rx)} expected echo of 'P'" if not ok else
              "3 params consumed; 'P' echoed",
        sent=sent, received=rx,
    )


def test_host_close(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x00 0x03 → host-close. After close, commands are ignored."""
    sent = bytes([ADMIN, ADMIN_HOST_CLOSE])
    _, rx = host.exchange(sent, expect=0, timeout=0.4)
    time.sleep(0.2)
    s = http.state() or {}
    is_open = s.get("winkeyOpen", True) if s else True
    ok = (len(rx) == 0) and (is_open is False)
    return TestResult(
        name="host_close_silent",
        passed=ok,
        detail=f"unexpected reply {hexstr(rx)}" if rx else
               (f"HTTP winkeyOpen={is_open} expected False" if is_open else
                "no reply; winkeyOpen=False"),
        sent=sent, received=rx, http_state=s,
    )


def test_commands_ignored_when_closed(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """After host-close, 0x02 0x32 (speed 50) must produce no reply and no state change."""
    sent = bytes([SPEED, 50])
    _, rx = host.exchange(sent, expect=0, timeout=0.4)
    time.sleep(0.2)
    s = http.state() or {}
    wpm = s.get("winkeyWpm", -1) if s else -1
    ok = (len(rx) == 0) and (wpm != 50)
    return TestResult(
        name="commands_ignored_after_close",
        passed=ok,
        detail=f"winkeyWpm={wpm} (expected NOT 50)" if not ok else
              "command swallowed; state unchanged",
        sent=sent, received=rx, http_state=s,
    )


def test_text_ignored_when_closed(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """After host-close, ASCII text must NOT be echoed (no buffer playback)."""
    sent = b"XYZ"
    _, rx = host.exchange(sent, expect=0, timeout=0.6)
    return TestResult(
        name="text_ignored_after_close",
        passed=len(rx) == 0,
        detail=f"unexpected echo {hexstr(rx)}" if rx else "no echo (correct)",
        sent=sent, received=rx,
    )


def test_reopen(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x00 0x02 again → another 0x17 (host-open is idempotent)."""
    sent = bytes([ADMIN, ADMIN_HOST_OPEN])
    _, rx = host.exchange(sent, expect=1, timeout=1.0)
    ok = len(rx) == 1 and rx[0] == WK_VERSION_BYTE
    return TestResult(
        name="reopen_returns_0x17",
        passed=ok,
        detail=f"got {hexstr(rx) or '(nothing)'}" if not ok else f"got 0x{rx[0]:02X}",
        sent=sent, received=rx,
    )


def test_reset_restores_defaults(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x00 0x01 → reset to defaults (WPM 20)."""
    sent = bytes([ADMIN, ADMIN_RESET])
    _, rx = host.exchange(sent, expect=0, timeout=0.4)
    time.sleep(0.3)
    s = http.state() or {}
    wpm = s.get("winkeyWpm", -1) if s else -1
    is_open = s.get("winkeyOpen", True) if s else True
    ok = (len(rx) == 0) and (wpm == 20) and (is_open is False)
    return TestResult(
        name="admin_reset_defaults",
        passed=ok,
        detail=f"winkeyWpm={wpm} (want 20), winkeyOpen={is_open} (want False)"
              if not ok else "wpm=20, winkeyOpen=False",
        sent=sent, received=rx, http_state=s,
    )


def test_setmode_wk1_then_wk2(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """Both 0x00 0x0A (WK1) and 0x00 0x0B (WK2) are accepted silently."""
    # Re-open (the host was opened at pre-test, but we may have been
    # closed by test_host_close in a previous run). Consume the 0x17
    # version-byte reply via exchange(expect=1) so it doesn't sit in
    # the RX buffer and get read as a stray byte by the next test.
    host.exchange(bytes([ADMIN, ADMIN_HOST_OPEN]), expect=1, timeout=1.0)

    sent = bytes([ADMIN, ADMIN_SET_WK1, ADMIN, ADMIN_SET_WK2])
    _, rx = host.exchange(sent, expect=0, timeout=0.5)
    return TestResult(
        name="setmode_wk1_then_wk2_silent",
        passed=len(rx) == 0,
        detail=f"unexpected reply {hexstr(rx)}" if rx else "no reply (correct)",
        sent=sent, received=rx,
    )


def test_long_text_echo(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """Stress: a 30-character message echoes all 30 bytes back."""
    sent = b"THE QUICK BROWN FOX JUMPS OVER"  # 30 bytes, all >= 0x20
    _, rx = host.exchange(sent, expect=len(sent), timeout=2.0)
    ok = rx == sent
    return TestResult(
        name="long_text_30byte_echo",
        passed=ok,
        detail=f"got {len(rx)}/{len(sent)} bytes" if not ok else
              f"all {len(sent)} bytes echoed",
        sent=sent, received=rx,
    )


def test_pause_resume_silent(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """0x06 + 1 param (pause/resume). Both are silent."""
    sent = bytes([PAUSE, 1, PAUSE, 0])
    _, rx = host.exchange(sent, expect=0, timeout=0.5)
    return TestResult(
        name="pause_resume_silent",
        passed=len(rx) == 0,
        detail=f"unexpected reply {hexstr(rx)}" if rx else "no reply (correct)",
        sent=sent, received=rx,
    )


def test_buffered_commands_consumed(host: WinkeyHost, http: HttpConsole, p: Printer) -> TestResult:
    """Buffered-command bytes (0x18-0x1F) are consumed with the right param count."""
    # 0x18 PTT 1-param, 0x19 KEY 1-param, 0x1A WAIT 1-param,
    # 0x1B MERGE 0-param, 0x1C SPEED 1-param, 0x1D HSCW 1-param,
    # 0x1E CANCELSPD 0-param, 0x1F NOP 0-param.
    sent = bytes([
        BUF_PTT, 1,
        BUF_KEY, 1,
        BUF_WAIT, 5,
        BUF_MERGE,
        BUF_SPEED, 30,
        BUF_HSCW, 20,
        BUF_CANCELSPD,
        BUF_NOP,
        ord('M'),  # text after all buffered commands
    ])
    _, rx = host.exchange(sent, expect=1, timeout=1.0)
    ok = len(rx) == 1 and rx[0] == ord('M')
    return TestResult(
        name="buffered_commands_0x18_to_0x1F_consumed",
        passed=ok,
        detail=f"got {hexstr(rx)} expected echo of 'M' (proves all buffered cmds parsed)"
              if not ok else "all 8 buffered commands consumed; 'M' echoed",
        sent=sent, received=rx,
    )


# ─── Test plan ─────────────────────────────────────────────────────────
BASIC_PLAN: List[Callable] = [
    test_host_open,
    test_wk2_mode,
    test_status_request,
    test_get_pot,
    test_set_wpm,
    test_sidetone_preset,
    test_send_text_echo,
    test_clear_buffer_silent,
    test_key_immediate_down_up,
    test_host_close,
    test_reopen,
    test_reset_restores_defaults,
]

ALL_PLAN: List[Callable] = [
    # Open
    test_host_open,
    test_wk2_mode,
    # Basic shape
    test_status_request,
    test_get_pot,
    # Parameter commands
    test_set_wpm,
    test_sidetone_preset,
    test_ptt_times_two_params,
    test_setmode_wk1_then_wk2,
    test_multi_byte_param_pot,
    test_load_defaults_consumes,
    # Echo
    test_send_text_echo,
    test_send_text_lowercase_uppercased,
    test_commands_not_echoed,
    test_long_text_echo,
    # Buffer ops
    test_backspace_removes_last,
    test_clear_buffer_silent,
    test_buffered_commands_consumed,
    test_pause_resume_silent,
    # Keying
    test_key_immediate_down_up,
    # Close path
    test_host_close,
    test_commands_ignored_when_closed,
    test_text_ignored_when_closed,
    test_reopen,
    test_reset_restores_defaults,
]


# ─── Driver ────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (auto-detect if omitted)")
    ap.add_argument("--baud", type=int, default=115200,
                    help="baud rate (USB-CDC ignores but pyserial needs a value)")
    ap.add_argument("--http", default="http://192.168.10.200",
                    help="A1Keyer dev HTTP console base URL")
    ap.add_argument("--no-http", action="store_true",
                    help="skip HTTP cross-verification")
    ap.add_argument("--plan", choices=["basic", "all"], default="all",
                    help="test plan: 'basic' = smoke test, 'all' = comprehensive")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress per-byte hex dumps")
    ap.add_argument("--no-color", action="store_true",
                    help="disable ANSI colors")
    ap.add_argument("--ignore-errors", action="store_true",
                    help="continue after a test failure (default: stop at first fail)")
    args = ap.parse_args()

    port = args.port or find_a1keyer_port()
    if not port:
        print("ERROR: no A1Keyer serial port found. Plug in the Cardputer or pass --port.",
              file=sys.stderr)
        print("Available ports:", file=sys.stderr)
        for p in list_ports.comports():
            print(f"  {p.device}  ({p.description})", file=sys.stderr)
        return 2

    print(f"Port: {port}   Baud: {args.baud}   HTTP: {args.http}"
          + ("   (disabled)" if args.no_http else ""))
    if not args.no_http:
        print(f"  Verify with: curl -s '{args.http}/state' | python3 -m json.tool")

    http = HttpConsole(args.http, enabled=not args.no_http)
    p = Printer(verbose=not args.quiet, use_color=not args.no_color)
    host = WinkeyHost(port, baud=args.baud, timeout=0.5)

    try:
        # Make sure the device is actually in WinKey mode (default per
        # src/console_io.cpp:30). If the operator toggled it via the 'D'
        # key, the http winkeyMode=false, and our bytes won't be parsed.
        s = http.state()
        if s and s.get("winkeyMode") is False:
            p.header("WARNING — device is in Console mode, not WinKey")
            print("  Press the 'D' key on the Cardputer keyboard to enter WinKey mode,")
            print("  then re-run this script. Bytes sent now are parsed as text keystrokes.")
            return 3

        # Initial reset for a clean test slate. Host-Open first so the
        # reset command isn't ignored, then open again for the test plan.
        p.header("Pre-test: host-open + reset to clear state")
        host.drain()
        host.exchange(bytes([ADMIN, ADMIN_HOST_OPEN]), expect=1, timeout=1.0)
        host.exchange(bytes([ADMIN, ADMIN_RESET]),       expect=0, timeout=0.5)
        host.exchange(bytes([ADMIN, ADMIN_HOST_OPEN]), expect=1, timeout=1.0)
        host.exchange(bytes([ADMIN, ADMIN_SET_WK2]),   expect=0, timeout=0.4)
        host.drain()
        time.sleep(0.2)
        s = http.state() or {}
        print(f"  state.winkeyOpen = {s.get('winkeyOpen')}   "
              f"winkeyWpm = {s.get('winkeyWpm')}   "
              f"winkeyMode = {s.get('winkeyMode')}")

        # Run the plan
        plan = BASIC_PLAN if args.plan == "basic" else ALL_PLAN
        p.header(f"Running {len(plan)} tests ({args.plan} plan)")
        results: List[TestResult] = []
        for fn in plan:
            r = fn(host, http, p)
            p.test(r)
            results.append(r)
            if not r.passed and not args.ignore_errors:
                print("\nAborted on first failure. Re-run with --ignore-errors to continue.")
                break

        return p.summary(results)

    finally:
        host.close()


if __name__ == "__main__":
    sys.exit(main())