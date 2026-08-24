A1Keyer v0.5.0 — see CHANGELOG.md for the full list. Headline changes:

- **Access-point (AP) mode** — press `A` to turn the Cardputer into a Wi-Fi access point (`A1Keyer`, `192.168.73.1/24`). The passphrase is persisted in NVS so reboots do not require retyping. Press `N` for the connected-station count. Mode is persisted: a reboot comes back in the mode the user last chose.
- **Forget-confirmation overlay** — in STA mode, `X` on the network-info screen now opens a Y/N modal before erasing credentials. In AP mode, `X` drops the AP without erasing the passphrase (or touching STA credentials).
- **Paddle polarity setting (`S`)** — swap dit/dah on the paddle levers without rewiring. Normal / Reversed, persisted to NVS, applied immediately. Iambic keyer only.
- On-device web interface with live decode, settings, and CW memory editing/playback.
- WinKey WK2 emulation for logger-driven CW (RUMlogNG, N1MM, fldigi, WriteLog).

Shipped as two binaries: `a1keyer-cardputer-v0.5.0.bin` (app update, offset `0x10000`) and `a1keyer-cardputer-v0.5.0-merged.bin` (full flash with bootloader+partitions+otadata, offset `0x0`).
