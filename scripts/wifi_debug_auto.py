#!/usr/bin/env python3
# wifi_debug_auto.py — PlatformIO pre-build script.
#
# Sets ENABLE_WIFI_DEBUG based on the presence of src/wifi_debug.enable:
#
#   src/wifi_debug.enable missing -> ENABLE_WIFI_DEBUG=0  (shipping build,
#                                no WiFi/HTTP stack pulled in)
#   src/wifi_debug.enable present -> ENABLE_WIFI_DEBUG=1  (dev build, HTTP
#                                console + WiFi brought up in main.cpp)
#
# Why this exists:
#   The dev network console (ConsoleServer + WifiDebug) is gated by
#   ENABLE_WIFI_DEBUG so shipping firmware has zero networking stack
#   beyond what WiFi brings. We want it to be a local opt-in: touch
#   src/wifi_debug.enable to enable, rm it to disable.
#
# History note: this script used to gate on the presence of
# src/secrets.h. That conflated two unrelated concerns (hardcoded
# WiFi credentials vs. the HTTP debug console). Credentials now
# live exclusively in NVS via the on-device keyboard flow, and
# secrets.h has been retired. The new marker file is empty and
# exists only to express "I want the dev console in this build".
#
# platformio.ini only needs to reference the script:
#   extra_scripts = pre:scripts/wifi_debug_auto.py
# No ENABLE_WIFI_DEBUG line in build_flags — the script is the
# single source of truth.
#
# If the script itself fails (Python error, Import problem), the
# macro is undefined and the C/C++ `#if ENABLE_WIFI_DEBUG` evaluates
# to false, which is the safe shipping default.

import sys
from pathlib import Path

Import("env")  # PlatformIO-provided build env


def log(msg: str) -> None:
    print(f"[wifi_debug_auto] {msg}", file=sys.stderr)


project_dir = Path(env["PROJECT_DIR"])
marker_path = project_dir / "src" / "wifi_debug.enable"

if marker_path.is_file():
    env.Append(CPPDEFINES=[("ENABLE_WIFI_DEBUG", "1")])
    log(f"src/wifi_debug.enable found -> ENABLE_WIFI_DEBUG=1")
else:
    env.Append(CPPDEFINES=[("ENABLE_WIFI_DEBUG", "0")])
    log(f"src/wifi_debug.enable NOT found -> ENABLE_WIFI_DEBUG=0")