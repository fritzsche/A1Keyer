#!/usr/bin/env bash
# run_tests.sh — native unit-test runner for the A1Keyer firmware.
#
# Compiles the platform-independent library sources against the
# `test/mocks/Arduino.h` stub (no ESP32 toolchain needed) and runs
# the CTest suite. Intended for the developer's own machine — there
# is no GitHub Actions CI on this repo (see README → "Running the
# unit tests" for context).
#
# Usage:
#   ./run_tests.sh                  # configure + build + run
#   ./run_tests.sh --clean          # wipe the build/ dir first
#   ./run_tests.sh --verbose        # ctest -V (stream every test's output)
#   ./run_tests.sh --build-only     # stop after cmake --build
#   ./run_tests.sh --jobs N         # parallel build jobs (default: nproc)
#
# Requires CMake ≥ 3.14 and a C++17 compiler (GCC, Clang, AppleClang,
# or MSVC/MinGW on Windows — see README for Windows notes).

set -euo pipefail

cd "$(dirname "$0")"

CLEAN=0
VERBOSE=0
BUILD_ONLY=0
JOBS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)    CLEAN=1 ;;
    --verbose)  VERBOSE=1 ;;
    --build-only) BUILD_ONLY=1 ;;
    --jobs)     JOBS="$2"; shift ;;
    --jobs=*)   JOBS="${1#*=}" ;;
    -h|--help)
      sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "Unknown flag: $1" >&2; exit 2 ;;
  esac
  shift
done

if [[ -z "$JOBS" ]]; then
  if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"
  elif command -v sysctl >/dev/null 2>&1; then JOBS="$(sysctl -n hw.ncpu)"
  else JOBS="2"; fi
fi

if [[ $CLEAN -eq 1 && -d build ]]; then
  echo ">>> rm -rf build"
  rm -rf build
fi

if [[ ! -d build ]]; then
  echo ">>> cmake -B build"
  cmake -B build
fi

echo ">>> cmake --build build --parallel $JOBS"
cmake --build build --parallel "$JOBS"

if [[ $BUILD_ONLY -eq 1 ]]; then
  exit 0
fi

if [[ $VERBOSE -eq 1 ]]; then
  echo ">>> ctest --test-dir build -V"
  ctest --test-dir build -V
else
  echo ">>> ctest --test-dir build --output-on-failure"
  ctest --test-dir build --output-on-failure
fi
