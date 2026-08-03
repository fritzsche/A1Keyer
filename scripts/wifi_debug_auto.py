#!/usr/bin/env python3
# wifi_debug_auto.py — PlatformIO pre-build script.
#
# Sets ENABLE_WIFI_DEBUG based on the presence of src/secrets.h:
#
#   src/secrets.h missing  -> ENABLE_WIFI_DEBUG=0  (shipping build,
#                             no WiFi/HTTP stack pulled in)
#   src/secrets.h present  -> ENABLE_WIFI_DEBUG=1  (dev build, HTTP
#                             console + WiFi brought up in main.cpp)
#
# Why this exists:
#   The user wants to opt in to the dev network console WITHOUT
#   touching platformio.ini every time (and without committing the
#   flag change to git). The natural signal of intent is "I created
#   secrets.h" — that file is already git-ignored, so toggling the
#   feature is purely local: create secrets.h to enable, delete it
#   to disable.
#
# platformio.ini only needs to reference the script:
#   extra_scripts = pre:scripts/wifi_debug_auto.py
# No ENABLE_WIFI_DEBUG line in build_flags — the script is the
# single source of truth.
#
# If the script itself fails (Python error, Import problem), the
# macro is undefined and the C/C++ `#if ENABLE_WIFI_DEBUG` evaluates
# to false, which is the safe shipping default.

import os
import sys
from pathlib import Path

Import("env")  # PlatformIO-provided build env


def log(msg: str) -> None:
    print(f"[wifi_debug_auto] {msg}", file=sys.stderr)


project_dir = Path(env["PROJECT_DIR"])
secrets_path = project_dir / "src" / "secrets.h"

if secrets_path.is_file():
    env.Append(CPPDEFINES=[("ENABLE_WIFI_DEBUG", "1")])
    log(f"src/secrets.h found -> ENABLE_WIFI_DEBUG=1")
else:
    env.Append(CPPDEFINES=[("ENABLE_WIFI_DEBUG", "0")])
    log(f"src/secrets.h NOT found -> ENABLE_WIFI_DEBUG=0")