# Changelog

All notable changes to A1Keyer are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Paddle polarity setting (`S`) — Normal / Reversed.** Press `S` on the
  decoder screen to open the **POLARITY** overlay, `;` for Normal, `.` for
  Reversed, `Enter` to save. When Reversed, the physical dit lever sounds a
  dah and vice versa, exactly as if the paddle cable had been rewired — which
  is the point: an operator whose paddle is wired backwards (or a left-handed
  operator who prefers the other orientation) no longer has to rewire the
  Grove cable or reflash. The change is applied to the running keyer
  immediately and persisted to the `morse` NVS namespace under the key
  `polarity` (bool, default `false` = Normal).

  The swap affects **only the iambic keyer**. It is implemented entirely
  inside `IambicKeyer` by mapping the `s_keyState` array subscript at the
  points where the keyer reads paddle memory, leaving the element identity
  (what is sounded and what reaches the decoder) untouched. The ISRs keep
  writing physical pin state, so the straight key, the `dit || dah`
  paddle-held checks and the decoder are all unaffected. Physical lever
  priority is preserved on a squeeze, so iambic alternation mirrors correctly.

## [0.3.0] - 2026-08-13

Bugfix release. Restores Wi-Fi connectivity after a failed first
association attempt.

### Fixed
- **Wi-Fi credentials persist on Enter, not only on success.**
  The original `connect()` saved the typed passphrase to NVS only
  after a `GOT_IP` event. If the first association attempt failed
  (wrong password, weak signal, an AP that simply never
  responded), the credentials were never written and a power
  cycle left the device in the IDLE state with no remembered
  network — the user had to re-type the entire passphrase to
  retry. The fix inverts the contract: NVS is written inside
  `connect()` immediately when the user commits the password, so
  the typed credentials survive any number of failed attempts and
  any number of reboots. The `X` (forget) key on the
  network-info screen remains the single, explicit way to remove
  them. The `test_failed_connect_does_not_store_credentials` test
  was rewritten to assert the new contract: a failed connect
  still leaves the saved credentials in place, and
  `disconnectAndForget()` is what clears them.

## [0.2.0] - 2026-08-13

### Added
- **On-device Wi-Fi configuration via the Cardputer keyboard.**
  Press `C` from the decoder to run an asynchronous scan, `;` / `.`
  to move the highlight, `Enter` to connect, `X` to forget the
  stored network. Credentials are written to NVS only after the
  association succeeds, so a typo never becomes the stored
  credential. The NVS namespace `net` holds up to 4 slots; full
  design in `docs/network.md`. Replaces the previous
  `src/secrets.h`-bundled path.
- **Dev-only HTTP console for state queries and log tail.**
  `GET /`, `GET /state`, `GET /log?n=N` expose a JSON view of the
  keyer state and the last N `Log::*` lines. The listener starts
  lazily from `loop()` once the link is up. Compile with
  `touch src/wifi_debug.enable` to enable the console in the next
  build; `rm` it to ship a clean image. The marker is git-ignored.
- **Wi-Fi passphrase masking.** The password input screen
  renders each character as `*` by default; `Shift+Space` toggles
  reveal so the user can sanity-check what they typed without
  leaving the plaintext visible to a shoulder-surfer.
- `run_tests.sh` — convenience wrapper around `cmake -B build &&
  cmake --build build && ctest --test-dir build
  --output-on-failure`. Picks a parallel-job count from `nproc` /
  `sysctl`, accepts `--clean`, `--verbose`, `--build-only`, and
  `--jobs N`. The README's "Repository layout" tree already
  referenced this file; it's now an actual script.

### Changed
- **Wi-Fi credentials are no longer bundled with the firmware.**
  The `src/secrets.h` compile-time credential source has been
  removed in favour of the on-device keyboard flow. `secrets.h`
  used to double as the dev-console toggle; that role is now
  served by the dedicated `src/wifi_debug.enable` marker file.
- **`CORE_DEBUG_LEVEL` dropped from 4 to 2 for the Cardputer
  build.** Production web-flasher images no longer stream
  ESP-IDF `Debug`-level logs to the USB monitor. Bump back to 4
  when you need a deep field trace and have a serial monitor
  attached.
- **CI removed.** `.github/workflows/ci.yml` (the matrix of
  Ubuntu/macOS/Windows × g++/clang++ unit-test runs) has been
  deleted. The project is a single-maintainer, MIT-licensed
  open-source effort and the maintainer does not pay for hosted
  CI. Unit tests are now run on the developer's own machine via
  the new `run_tests.sh` wrapper (see "Running the unit tests" in
  the README). The README's "CI policy" section documents the
  rationale and lists the GitHub paid features (Code Quality, …)
  that should remain disabled.

### Fixed
- **Wi-Fi IP shown on screen matches the DHCP lease.** The
  `esp_netif` GOT_IP event delivers `ip_info.ip.addr` in the
  host-byte-order layout of the four octets on this little-endian
  ESP32-S3, even though the underlying type is documented as
  network byte order. Without the conversion the displayed IP
  came back with its bytes reversed (e.g. `135.10.168.132` for a
  `192.168.10.135` lease). `notifyGotIp` now wraps the address
  in `htonl()` at the storage site so every consumer
  (`WifiMgr::localIP()`, `MorseModel::wifiLocalIP()`, the HTTP
  `/state` endpoint) sees the same big-endian layout the
  `>>24`-yields-first-octet contract documents.
- **Wi-Fi reconnect after reboot no longer loops through
  `CONNECT_FAILED`.** `WiFi.begin()` while a previous association
  is still torn down fires a synchronous `ASSOC_LEAVE` (reason 8)
  DISCONNECTED event before the new association completes; the
  state machine then dropped into `CONNECT_FAILED` and the
  follow-on `GOT_IP` was silently discarded because it was only
  handled inside the `CONNECTING` branch. The fix moves the
  `GOT_IP` consumer to the top of `poll()` with a state guard
  covering `CONNECTING | CONNECT_FAILED | DISCONNECTED`, and
  also clears any pending `DISCONNECTED` queued for the same
  tick so the new connection isn't immediately torn down. New
  tests in `test/test_network_manager/`.
- **WinKey prime gate: stray bytes from RUMlogNG's init sequence
  are no longer keyed as CW at boot.** The wire trace from RUMlogNG
  (captured on the live device) shows the host sends
  `00 02 00 0B 00 0F 00 01 01 10 00 0E 44 09 04 ... 07 15` during
  open. Our parser correctly consumed every command and parameter,
  but the `00 0E` (admin 14, "Send Standalone Message" — ignored by
  A1Keyer, no standalone messages stored) was followed immediately
  by `0x44` = 'D', which the parser then routed as text into the
  send buffer. `cbSendText("D")` played `-..` at boot and the
  display lit up with a character the user hadn't typed.
  RUMlogNG streams bytes from its outgoing-CW buffer during its
  init, so any text pre-populated there was being keyed
  unsolicited. Fix: `WinkeyBridge` now ignores text bytes until
  the host has probed us with `GET_POT` (0x07) or `REQ_STATUS`
  (0x15) — both of which every shipping WK2 host sends as part of
  its init. The gate resets on soft reset so a defensive
  `0x00 0x01` from the host re-enters the unprimed state. Five new
  unit tests in `test/test_winkey_bridge/` cover the gate; full
  trace in `docs/winkey.md § 13.9`.
- **WinKey soft reset no longer closes the host interface.**
  `WinkeyBridge::handleAdmin(ADMIN_RESET)` previously called
  `resetForTest()`, which set `_open=false`. RUMlogNG (and every
  other shipping WK2 host — N1MM, fldigi, WriteLog) issues a
  defensive `0x00 0x01` as part of its init sequence and then
  immediately sends `0x07` (GetPot), `0x15` (ReqStatus), or text.
  With the old behaviour every byte after the reset was silently
  dropped, RUMlogNG got the version byte on its initial open but
  then timed out on its post-reset probe and reported "Interface is
  not available". The fix splits the reset work into a new
  `resetParams()` helper that restores parameter defaults, idles
  the parser, and clears the send buffer, but does NOT touch
  `_open` (or `_wk2Mode`). The test entry point
  `resetForTest()` still forces `_open=false` so each test case
  starts clean. Matches K3NG and `hamlib/rig/winkey.c`
  behaviour. Documented in `docs/winkey.md § 5.2`; the original
  failing wire trace from RUMlogNG is reproduced there.
- **WinKey playback no longer restarts the audio player on every
  byte.** Hosts stream text as fast as the serial line can carry
  it; the old `poll()` called `cbSendText` for every character,
  which unconditionally called `MorseGenerator::playText()` and
  reset the element cursor — producing a per-character audio
  click and visibly broken playback. The fix adds a
  `canAcceptText()` query to `WinkeyBridge::Callbacks`. When it
  returns false (audio player busy), `poll()` skips draining the
  buffer; text accumulates and the next idle poll drains the
  whole pending chunk in one `sendText()` call. `cbSendText`
  itself also re-checks `gen->isPlaying()` as defence in depth.
  Documented in `docs/winkey.md § "Text playback and the audio
  click bug"`.
- **`MorseGenerator::_playText` now owns its buffer, not borrows
  one.** The previous `const char*` member aliased the
  stack-local `chunk[]` that `WinkeyBridge::poll()` hands to
  `cbSendText`. After `cbSendText` returned the chunk was gone,
  so the audio task read freed stack memory — MorseModel
  appended garbage bytes (`#`, `?`, NUL) to the decoded-text
  ring buffer and the display showed the wrong character. The
  fix replaces the raw pointer with a `std::string` and copies
  the text on `playText()`. No behavioural change for callers;
  decoder text now matches what was actually sent.
- **Inter-character silence is preserved across chunk
  boundaries.** RUMlogNG (and other WK2 hosts) often streams
  text in multiple bursts — e.g. `"UR 5NN"` then a pause then
  `" TU"` — and the user reported that `TU` sounded like `T·U`
  (no inter-character gap) while `"599 TU"` (with a space
  before the final word) played correctly. Root cause:
  `MorseEncoder::encode()` only emits `CHAR_SPACE` *between*
  characters within one `encode()` call, never after the last
  character, so the bridge's second `sendText()` had no leading
  silence. The fix tracks `_endedWithBoundarySilence` in
  `MorseGenerator` and prepends a `CHAR_SPACE` to the next
  chunk when the previous one ended without one
  (`src/morse_generator.cpp:89`). `stop()` resets the tracking
  so an intentional reset never inherits a synthetic gap. A
  latent bug made this fix invisible at first:
  `MorseEncoder::Element::Type` had value collisions between
  the mark types (`DIT=1`, `DAH=3`) and the silence types
  (`ELEMENT_SPACE=1`, `CHAR_SPACE=3`); the boundary-type
  comparison `(lastType == CHAR_SPACE || lastType == WORD_SPACE)`
  therefore returned TRUE for any chunk ending in a `DAH` and
  suppressed the prepend. The enum values are now unique
  (`src/morse_encoder.h`). Covered by three new tests in
  `test/test_morse_generator/test_morse_generator.cpp`; full
  trace in `docs/winkey.md § 13.8`.

## [0.1.0] - 2026-06-14

First public release. This is a fresh publication of the A1Keyer firmware
(renamed from the internal `dit_dah_esp` project) at a 0.x version while
the feature set stabilises. v1.0.0 will follow once it has had more time
on real hardware.

This release ships a pre-built binary for the **Cardputer ADV (ESP32-S3)** only. The Tab5 (ESP32-P4) target compiles but is not yet considered release-ready and will follow in a later release.

### Added
- MIT License — see `LICENSE`.
- Host-side unit tests wired into CMake's CTest framework (`cmake -B build && ctest --test-dir build`).
- CI workflow (`.github/workflows/ci.yml`) — runs CTest on every push and pull request across a matrix of Ubuntu / macOS / Windows × g++ / clang++.
- `A1KEYER_VERSION` macro emitted at boot via `Serial`.

### Changed
- Project published as **A1Keyer** (`README.md`, `CMakeLists.txt`, firmware banner, all user-facing references).
- Test workflow migrated from the platform-specific `run_tests.sh` (macOS/Linux only, hard-coded `clang++` and an absolute path) to cross-platform CTest (works on macOS, Linux, Windows / MSVC, Windows / MinGW with no per-platform script).
- `README.md` documents the Cardputer ADV as the only shipped target — Tab5 build instructions, the ES8388 codec details, and the headphone path have been removed.

### Fixed
- `test_keyer_returns_to_none_when_neither_memory_set` now drives the state machine past the 7-dit word-space threshold before asserting `!isActive()`. The test was previously checking a transient state — the keyer correctly reports "active" between elements so the audio engine keeps ticking for word-space detection.
- `straight_keyer.cpp::fillSamples()` GPIO reads are now guarded with `#ifndef UNIT_TEST`, matching the existing pattern in `iambic_keyer.cpp`. This lets the source be linked into the host-side test binaries (CMake's `LIB_SOURCES` includes all platform-independent sources in every test) without requiring a real `PIN_KEY_DIT` constant.
- `display_model.h` now `#include <cstddef>` so `size_t` is available portably. macOS / MSYS2 happen to pull it in transitively; Ubuntu's libc++-14 does not.

### Repository hygiene
- `.vscode/` and `extern/` are not tracked by Git (kept locally, ignored via `.gitignore`).

[0.1.0]: https://github.com/fritzsche/A1Keyer/releases/tag/v0.1.0
[0.2.0]: https://github.com/fritzsche/A1Keyer/releases/tag/v0.2.0
[0.3.0]: https://github.com/fritzsche/A1Keyer/releases/tag/v0.3.0
