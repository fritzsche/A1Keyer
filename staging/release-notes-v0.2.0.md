A1Keyer v0.2.0 — see CHANGELOG.md for the full list. Headline changes:

- On-device Wi-Fi configuration via the Cardputer keyboard (`C` for scan, `N` for status, `X` to forget). The compiled-in `src/secrets.h` path is gone.
- Dev HTTP console (`GET /`, `/state`, `/log?n=N`) compiled in via the `src/wifi_debug.enable` marker file.
- Wi-Fi passphrase masking with `Shift+Space` reveal.
- Wi-Fi IP display byte-order fix; reconnect no longer loops through `CONNECT_FAILED`.
- WinKey prime gate (RUMlogNG init no longer keys `D` at boot), soft-reset no longer closes the host, text playback no longer clicks per byte, inter-chunk boundary silence preserved.
- `CORE_DEBUG_LEVEL` dropped from 4 to 2 in the production Cardputer build.
- CI workflow removed; unit tests run locally via `run_tests.sh`.

Shipped as two binaries: `a1keyer-cardputer-v0.2.0.bin` (app update, offset `0x10000`) and `a1keyer-cardputer-v0.2.0-merged.bin` (full flash with bootloader+partitions+otadata, offset `0x0`).
