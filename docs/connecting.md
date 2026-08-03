# Connecting to A1Keyer

A1Keyer connects to a computer over a **single USB-C serial port**
(hardware USB-Serial-JTAG). That one port serves two runtime modes,
switched on the device with the **'D' key**:

| Mode | Purpose | Default |
|---|---|---|
| **Console** (dev) | Firmware upload, `pio device monitor`, debug logs | ✅ |
| **WinKey** | Speaks the K1EL WinKeyer WK2 protocol for a logger (RUMlogNG, N1MM, fldigi) | |

Press **D** on the Cardputer keyboard to toggle. The status line shows
a **`WK`** tag (accent colour) while in WinKey mode.

> **Why one port + a toggle** (not two ports): the ESP32-S3 has a single
> USB PHY shared between the USB-OTG controller and the hardware
> USB-Serial-JTAG controller — only one runs at a time. Using hardware
> JTAG gives rock-solid uploads (the normal ESP32-S3 path) but only one
> serial port, so the WinKeyer interface time-shares that port via the
> mode toggle. (An earlier two-port TinyUSB design made flashing
> fragile; see git history / `docs/ble_error.md` for the BLE detour.)

---

## Finding the port

Plug in the Cardputer:

- **macOS:** `ls /dev/cu.usbmodem*` → one entry, e.g. `/dev/cu.usbmodem1101`
- **Windows:** Device Manager → Ports (COM & LPT) → one `USB Serial Device (COMx)`
- **Linux:** `ls /dev/ttyACM*` → `/dev/ttyACM0`

There is only one port in every mode — the mode changes what the port
*speaks*, not how many ports exist.

---

## Console mode (default) — upload, monitor, debug

Normal PlatformIO workflow, no manual steps:

```bash
pio run -e esp32s3_cardputer -t upload   # flash firmware
pio device monitor -b 115200             # watch [INFO]/[setup] logs
```

Uploads use the hardware USB-Serial-JTAG bootloader — no BOOT/RESET
button dance, no port-switch issues.

---

## WinKey mode — driving CW from a logger

1. Flash + connect over USB-C (Console mode).
2. On the Cardputer, press **D** → status line shows `WK`. The serial
   port now speaks the WinKeyer WK2 protocol; debug output is silently
   buffered (not written to the wire) so it can't corrupt the protocol.
3. In your logger's WinKeyer/CW settings, select the **same** serial
   port and connect:
   - **RUMlogNG (macOS):** Preferences → CW/WinKeyer → pick the
     `/dev/cu.usbmodem*` device. It performs the host-open handshake;
     A1Keyer replies with WK2 version `0x17`. Set speed and send CW.
   - **N1MM+ / fldigi (Windows/Linux):** select the COM port /
     `/dev/ttyACM0` in the WinKeyer config.
4. Press **D** again to return to Console mode. Any debug output that
   occurred during the WinKey session is **replayed** to the terminal
   (bracketed by `--- N buffered log bytes ---`).

> Close the logger's serial connection (or just switch back to Console
> mode) before running `pio device monitor` — one host program owns the
> port at a time.

---

## Radio keying

When A1Keyer keys CW (from the paddle, straight key, or a WinKeyer
message) it drives the PC817 optocoupler output to key a real
transceiver — **only** if the operator has turned KEYING on in the
device settings. A host's WinKeyer output-enable cannot re-enable RF
that the operator disabled locally. See `docs/keyer.md` and
`docs/winkey.md § 16.4`.
