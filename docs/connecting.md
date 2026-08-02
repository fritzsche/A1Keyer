# Connecting to A1Keyer

A1Keyer connects to a computer over **USB-C**. With a single cable the
firmware presents **two independent serial ports** (a TinyUSB composite
device):

| Port | Purpose | Speed |
|---|---|---|
| **CDC0** | Firmware upload, `pio device monitor`, debug logs (`[INFO]…`) | 115200 |
| **CDC1** | WinKeyer 2.x interface for logging software (RUMlogNG, N1MM, fldigi) | any |

Both appear from one USB-C cable — on macOS as two `/dev/cu.usbmodem*`
devices, on Windows as two `COMx` ports, on Linux as two `/dev/ttyACM*`.

> **Bluetooth note.** Earlier firmware experimented with BLE. It was
> removed: BLE is not an OS-level serial port on any platform (a BLE
> peripheral never becomes a COM port / `/dev/tty`), so logging software
> could not use it. USB CDC is the transport. The historical BLE
> investigation is kept in `docs/ble_error.md` for reference only.

---

## Identifying the two ports

Plug in the Cardputer, then:

### macOS
```bash
ls /dev/cu.usbmodem*
# e.g. /dev/cu.usbmodem1101   /dev/cu.usbmodem1103
```
The two entries are CDC0 and CDC1. The lower-numbered one is usually
CDC0 (upload/monitor); if unsure, open one in a terminal — the port that
prints `[INFO]`/`[setup]` log lines at boot is CDC0.

### Windows
Open **Device Manager → Ports (COM & LPT)**. Two `USB Serial Device
(COMx)` entries appear. The one that streams boot log text is CDC0.

### Linux
```bash
ls /dev/ttyACM*
# e.g. /dev/ttyACM0  /dev/ttyACM1
```

---

## CDC0 — upload, monitor, and debug logs

This is the normal PlatformIO workflow — unchanged from before:

```bash
pio run -e esp32s3_cardputer -t upload      # flash firmware
pio device monitor -b 115200                # watch [INFO]/[setup] logs
```

`pio device monitor` auto-selects CDC0. If it picks the wrong port, pass
`--port /dev/cu.usbmodemXXXX` (macOS/Linux) or `--port COMx` (Windows).

> **Upload note (TinyUSB mode).** The firmware runs USB in TinyUSB mode
> (`ARDUINO_USB_MODE=0`) so it can expose two CDC ports. Normal uploads
> work as usual. Only if a build crashes very early in `setup()` (before
> USB initialises) might the upload port not appear — recover by holding
> **G0/BOOT** while pressing **RESET** to enter the ROM bootloader, then
> upload once. This is rare, not a per-upload step.

---

## CDC1 — WinKeyer 2.x interface

CDC1 speaks the K1EL WinKeyer WK2 protocol (see `docs/winkey.md`). Point
your logging software's WinKeyer/CW settings at the **second** serial
port.

### RUMlogNG (macOS)

1. Flash the firmware and connect the Cardputer over USB-C.
2. In RUMlogNG → **Preferences → CW/WinKeyer** (or the CW keyer settings),
   select the WinKeyer device and choose the CDC1 port
   (`/dev/cu.usbmodem*` — the one that is *not* printing debug logs).
3. RUMlogNG performs the WinKeyer host-open handshake; A1Keyer replies
   with version `0x06` (WK2). Set your speed; sending CW from RUMlogNG
   now keys A1Keyer.

### N1MM+ / fldigi / other loggers (Windows/Linux)

Same idea: in the WinKeyer configuration, select CDC1's COM port
(`COMx`) / `/dev/ttyACM1`. The device identifies as a WK2 keyer.

### Quick manual check (no logger needed)

Any serial terminal can exercise the handshake. Open CDC1 and send the
host-open bytes `0x00 0x02`; the device echoes them and replies `0x06`.
`docs/winkey.md § 5` documents the full sequence, and
`scripts/winkey_host.py` (a Python test utility, if present) automates it.

---

## Radio keying

When A1Keyer keys CW (from the paddle, straight key, or a WinKeyer
message), it drives the PC817 optocoupler output to key a real
transceiver — **only** if the operator has turned KEYING on in the
device settings. A host's WinKeyer "output enable" cannot re-enable RF
that the operator disabled locally. See `docs/keyer.md` and
`docs/winkey.md § 16.4`.
