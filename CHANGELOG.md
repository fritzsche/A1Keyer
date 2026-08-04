# Changelog

All notable changes to A1Keyer are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
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

### Changed
- **CI removed.** `.github/workflows/ci.yml` (the matrix of
  Ubuntu/macOS/Windows × g++/clang++ unit-test runs) has been
  deleted. The project is a single-maintainer, MIT-licensed
  open-source effort and the maintainer does not pay for hosted
  CI. Unit tests are now run on the developer's own machine via
  the new `run_tests.sh` wrapper (see "Running the unit tests" in
  the README). The README's "CI policy" section documents the
  rationale and lists the GitHub paid features (Code Quality, …)
  that should remain disabled.

### Added
- `run_tests.sh` — convenience wrapper around `cmake -B build &&
  cmake --build build && ctest --test-dir build
  --output-on-failure`. Picks a parallel-job count from `nproc` /
  `sysctl`, accepts `--clean`, `--verbose`, `--build-only`, and
  `--jobs N`. The README's "Repository layout" tree already
  referenced this file; it's now an actual script.

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
