#!/usr/bin/env python3
# merge_bin.py — PlatformIO post-build script.
#
# Runs `esptool.py merge-bin` against the artifacts in
# `.pio/build/<env>/` to produce `firmware-merged.bin`. The web
# flasher (../../js/A1Keyer-Flasher) uses this single file when the
# user picks "Full flash" — writes bootloader + partition table +
# otadata + app to a bare ESP32-S3 in one shot.
#
# Adds to platformio.ini under [env:esp32s3_cardputer]:
#   extra_scripts = post:scripts/merge_bin.py
#
# Requires `esptool` on PATH (pip install esptool). Silently skips
# if esptool is not installed, so CI environments without it still
# build cleanly.

import os
import shutil
import subprocess
import sys
from pathlib import Path

Import("env")  # PlatformIO-provided build env

def log(msg):
    print(f"[merge_bin] {msg}", file=sys.stderr)

def find_esptool():
    return shutil.which("esptool") or shutil.which("esptool.py") or shutil.which("esptool3") or shutil.which("esptool-3") or "python3 -m esptool"

def run(cmd):
    log("$ " + " ".join(cmd) if isinstance(cmd, list) else cmd)
    subprocess.run(cmd, check=True)

def main():
    build_dir = Path(env["PROJECT_BUILD_DIR"]) / env["PIOENV"]
    bootloader = build_dir / "bootloader.bin"
    partitions = build_dir / "partitions.bin"
    firmware   = build_dir / "firmware.bin"
    merged_out = build_dir / "firmware-merged.bin"

    for f in (bootloader, partitions, firmware):
        if not f.exists():
            log(f"skip: missing {f}")
            return

    esptool = find_esptool()
    if esptool == "python3 -m esptool":
        cmd = ["python3", "-m", "esptool", "--chip", "esp32s3", "merge-bin",
               "-o", str(merged_out),
               "--flash-mode", "dio", "--flash-size", "8MB", "--flash-freq", "80m",
               "0x0",     str(bootloader),
               "0x8000",  str(partitions),
               "0x10000", str(firmware)]
    else:
        cmd = [esptool, "--chip", "esp32s3", "merge-bin",
               "-o", str(merged_out),
               "--flash-mode", "dio", "--flash-size", "8MB", "--flash-freq", "80m",
               "0x0",     str(bootloader),
               "0x8000",  str(partitions),
               "0x10000", str(firmware)]

    try:
        run(cmd)
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        log(f"esptool merge-bin failed ({e}); skipping. Install esptool to enable merged-binary output.")
        return

    log(f"wrote {merged_out}")

main()
