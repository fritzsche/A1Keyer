## A1Keyer 0.1.0 — first public release (Cardputer ADV)

Low-latency Morse code trainer firmware for the M5Stack Cardputer ADV.

**Target:** M5Stack Cardputer ADV (ESP32-S3 @ 240 MHz, NS4168 I2S amp,
1.14" 240×135 ST7789V2, 56-key matrix + Button A + GPIO paddle).

> **Note:** This is a 0.x release while the feature set stabilises on real
> hardware. v1.0.0 will follow once it has had more time.
>
> The Tab5 (ESP32-P4) target compiles but is not yet considered
> release-ready and will follow in a later release.

### Features

- **Iambic B keyer** — dual-paddle squeeze keying with proper SWAP /
  AUTOREPEAT / END-OF-CHAR logic and 7-dit word-space detection.
- **Straight key** — single-contact keying with bounce-guard verification
  and adaptive DIT/DAH classification based on a rolling median.
- **CW decoder** — turns paddle activity into text via a lock-free ring
  buffer and a 200-character circular display buffer.
- **Text playback (encoder)** — `playText("…")` renders typed text as CW
  through the same audio path.
- **Sub-3 ms audio latency** end-to-end.

### Flashing

Flash `a1keyer-cardputer-v0.1.0.bin` to the Cardputer ADV with `esptool.py`
or the M5Stack burner:

```bash
esptool.py --chip esp32s3 -p /dev/tty.usbserial-* \
    --baud 1500000 write_flash 0x0 a1keyer-cardputer-v0.1.0.bin
```

### Verification

```
SHA256: 672fc9255cd58ddde670481037e0e8dda7587e27cc03a2fc7f02d0c2fd3711bf
Size:   601,392 bytes (587 KB)
```

### Webflasher

The webflasher should hit GitHub's built-in "latest release" endpoint,
which always resolves to the most recent published release:

```
GET https://api.github.com/repos/fritzsche/A1Keyer/releases/latest
```

The response's `assets[].browser_download_url` is the direct download URL
of the current Cardputer binary. When v0.2.0 ships, the same API call
returns the new binary — no webflasher changes needed.

(The repository also carries a floating `latest` git tag pointing at the
most recent release commit. That tag is auto-moved on every push of a
`vX.Y.Z` tag by the
[`Update latest tag`](https://github.com/fritzsche/A1Keyer/actions/workflows/release-latest-tag.yml)
workflow — so you can also clone or check it out with
`git clone --branch latest`. Pre-release tags like `v0.2.0-rc1` are
intentionally excluded.)

### License

MIT — see [`LICENSE`](https://github.com/fritzsche/A1Keyer/blob/v0.1.0/LICENSE)
in the source tree.

### Changelog

See [`CHANGELOG.md`](https://github.com/fritzsche/A1Keyer/blob/v0.1.0/CHANGELOG.md)
for the full list of changes.
