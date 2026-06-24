# Changelog

All notable changes to A1Keyer are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
