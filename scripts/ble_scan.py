#!/usr/bin/env python3
# ble_scan.py — quick BLE peripheral scanner using `bleak`.
#
# Prints nearby BLE peripherals (name, address, RSSI) for as long as
# --duration allows, sorted by signal strength (strongest first). The
# goal is to verify that the A1Keyer's BT MAC on the air matches the
# MAC our firmware derived from the eFuse factory MAC — see
# docs/ble_error.md §8c for the static-random-address fix this script
# lets us confirm.
#
# Usage (from the A1Keyer repo root):
#
#   ~/.bleak-env/bin/activate    # if not already active
#   python3 scripts/ble_scan.py --duration 10
#   python3 scripts/ble_scan.py --duration 10 --name A1Keyer
#   python3 scripts/ble_scan.py --duration 10 --json
#
# Requires `bleak` (>= 0.21). Newer versions removed the bundled
# `bleak-scan` script; we go through the public API directly.
#
# Requires macOS Bluetooth to be enabled. If the scan comes back
# empty, check `blueutil -p` (install via `brew install blueutil`).
#
# Why a longer scan than 5s: CoreBluetooth can drop the first ADV
# report from a freshly-booted peripheral. 10s gives several chances.

import argparse
import asyncio
import json
import sys

from bleak import BleakScanner
from bleak.backends.scanner import AdvertisementData


async def scan(duration: float) -> dict:
    """Scan for `duration` seconds, return {address: (BLEDevice, AdvertisementData)}.

    Uses the callback-based scanner because RSSI lives on the
    AdvertisementData (delivered on every detection). The higher-level
    BleakScanner.discover() helper in bleak ≥ 0.21 drops RSSI on the
    CoreBluetooth backend.
    """
    found: dict = {}

    def on_detect(device, adv: AdvertisementData) -> None:
        # Multiple ADV reports may arrive for the same peripheral;
        # keep the most recent (which for the same address and same
        # power level should also be the most representative).
        found[device.address] = (device, adv)

    scanner = BleakScanner(detection_callback=on_detect)
    await scanner.start()
    try:
        await asyncio.sleep(duration)
    finally:
        await scanner.stop()

    return found


def sorted_by_rssi(found: dict) -> list:
    return sorted(
        found.values(),
        key=lambda da: da[1].rssi,
        reverse=True,  # strongest first
    )


def format_table(rows: list) -> str:
    lines = [f"{'NAME':24s} {'ADDRESS':18s} {'RSSI':>6s}",
             "-" * 50]
    for device, adv in rows:
        name = device.name or "(no name)"
        lines.append(f"{name:24s} {device.address:18s} {adv.rssi:5d} dBm")
    return "\n".join(lines)


def format_json(rows: list) -> str:
    return json.dumps(
        [{"name": d.name, "address": d.address, "rssi": adv.rssi}
         for d, adv in rows],
        indent=2,
    )


def filter_by_name(rows: list, name: str) -> list:
    target = name.lower()
    return [(d, a) for d, a in rows
            if d.name and target in d.name.lower()]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="BLE peripheral scanner (bleak).",
    )
    parser.add_argument(
        "--duration", "-d",
        type=float, default=10.0,
        help="Scan length in seconds (default: 10).",
    )
    parser.add_argument(
        "--name", "-n",
        type=str, default=None,
        help="Filter: only show peripherals whose name contains this "
             "(case-insensitive).",
    )
    parser.add_argument(
        "--json", "-j",
        action="store_true",
        help="Emit JSON instead of a table.",
    )
    args = parser.parse_args(argv)

    try:
        found = asyncio.run(scan(args.duration))
    except Exception as exc:  # noqa: BLE001 — surface any error to stdout
        print(f"scan failed: {exc!r}", file=sys.stderr)
        return 1

    rows = sorted_by_rssi(found)
    if args.name:
        rows = filter_by_name(rows, args.name)

    if args.json:
        print(format_json(rows))
    else:
        print(format_table(rows))

    if args.name and not rows:
        return 2  # a name was given but nothing matched
    return 0


if __name__ == "__main__":
    sys.exit(main())
