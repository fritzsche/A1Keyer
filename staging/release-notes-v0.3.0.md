Bugfix release.

## What's in v0.3.0

Restores Wi-Fi connectivity after a failed first association attempt.
NVS is now written when the user commits the password (not after
`GOT_IP`), so the typed credentials survive any number of failed
attempts and any number of reboots. The `X` (forget) key on the
network-info screen remains the single, explicit way to remove them.

## Flash modes

The web flasher picks the right binary for you:

- **App update only** — preserves your saved settings and Wi-Fi
  credentials. Use this to upgrade an already-configured device.
- **Full flash** — wipes everything (keyer settings + Wi-Fi
  credentials). Use this on a new device or to recover from a
  misconfiguration.

For manual flashing with `esptool.py`, the canonical filenames are
listed below.
