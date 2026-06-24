# Testing

A1Keyer's platform-independent library code (`morse_encoder`, `key_envelop`,
`morse_generator`, `iambic_keyer`, `straight_keyer`, `morse_decoder`,
`MorseTable`, `display_model`, `fast_math`) is covered by a native C++
unit-test suite. The tests compile those sources against a minimal Arduino
stub (`test/mocks/Arduino.h`) — **no ESP32 toolchain is required**, and
the suite runs on macOS, Linux, Windows / MSVC, and Windows / MinGW.

The tests are not run by any hosted CI service — the maintainer runs them
on their own machine via `./run_tests.sh` before any commit lands on
`main` or `release`. PRs that arrive without a passing local test run will
be asked to demonstrate one.

---

## Prerequisites

- CMake ≥ 3.14
- A C++17 compiler: GCC, Clang, AppleClang, or MSVC / MinGW on Windows

---

## Quick start

```bash
./run_tests.sh                  # configure + build + run
```

`run_tests.sh` is a thin wrapper around the three commands below. It picks
a sensible parallel-job count, supports `--clean`, `--verbose`,
`--build-only`, and `--jobs N`, and is what the maintainer runs before
every commit.

If you'd rather call CMake directly:

```bash
cmake -B build                          # configure
cmake --build build                     # compile all test binaries
ctest --test-dir build --output-on-failure   # run the suite
```

CMake auto-selects a generator based on the host:

| Platform | Generator | Notes |
|---|---|---|
| macOS / Linux | Unix Makefiles (default) | `cmake -G Ninja …` works too if Ninja is installed. |
| Windows (MSVC) | MSBuild / Ninja | Run from a *Developer Command Prompt for VS* so the MSVC compiler is on `PATH`. |
| Windows (MinGW) | MinGW Makefiles | `cmake -B build -G "MinGW Makefiles"`. |

### `run_tests.sh` flags

| Flag | Effect |
|---|---|
| `--clean` | `rm -rf build/` before configuring. |
| `--verbose` | Run `ctest -V` (stream every test's output). |
| `--build-only` | Stop after `cmake --build` — don't run the suite. |
| `--jobs N` | Parallel build jobs (default: `nproc` on Linux, `sysctl -n hw.ncpu` on macOS, `2` otherwise). |
| `-h`, `--help` | Print the usage block at the top of the script. |

---

## Expected output

A successful run prints one line per suite and a final summary:

```
Test project /Users/thomas/devel/arduino/A1Keyer/build
    Start  1: test_morse_encoder
1/6 Test #1: test_morse_encoder .............   Passed    0.02 sec
    Start  2: test_key_envelop
2/6 Test #2: test_key_envelop ...............   Passed    0.01 sec
    Start  3: test_morse_generator
3/6 Test #3: test_morse_generator ...........   Passed    0.20 sec
    Start  4: test_iambic_keyer
4/6 Test #4: test_iambic_keyer ..............   Passed    0.27 sec
    Start  5: test_morse_decoder
5/6 Test #5: test_morse_decoder .............   Passed    0.22 sec
    Start  6: test_display_model
6/6 Test #6: test_display_model .............   Passed    0.22 sec

100% tests passed, 0 tests failed out of 6

Total Test time (real) =   1.0 sec
```

CTest shows pass/fail per binary. To see the per-assertion detail
(`pass test_xxx` lines and the `N/N assertions passed` summary each
binary prints), either run the binary directly:

```bash
./build/test_morse_encoder     # macOS / Linux
build\test_morse_encoder.exe   # Windows
```

…or stream every test's output with `ctest -V`:

```bash
ctest --test-dir build -V
```

---

## Useful CTest options

| Flag | Effect |
|---|---|
| `-V` | Stream each test's stdout/stderr as it runs. |
| `--output-on-failure` | Stream output only on failure. |
| `-j N` | Run N tests in parallel. |
| `-R regex` | Run only tests whose name matches the regex. |
| `-E regex` | Exclude tests matching the regex. |
| `--repeat until-fail:3` | Run the suite up to 3 times, stopping on first failure. |

---

## Adding a new test suite

1. Drop `test/<name>/test_<name>.cpp` and use the helpers from
   `test/test_framework.h` (`CHECK`, `CHECK_EQ`, `RUN`, `test_summary`).
2. Register it in `CMakeLists.txt` — add `test_<name>` to the
   `foreach(SUITE IN ITEMS …)` list. CTest picks it up on the next
   configure; no other wiring is required.

Each `test_<name>.cpp` compiles against the platform-independent
`LIB_SOURCES` list in `CMakeLists.txt` and the `test/mocks/Arduino.h`
stub, so test files can `#include <Arduino.h>` (for `Serial`) and link
against any of the library sources without an ESP32 toolchain.

---

## Test framework macros

Tests use a self-contained header (`test/test_framework.h`) with no
external dependencies. Available macros:

| Macro | Purpose |
|-------|---------|
| `CHECK(cond)` | Fail if condition is false |
| `CHECK_EQ(a, b)` | Fail if `a != b` (prints both values) |
| `CHECK_NEAR(a, b, eps)` | Fail if `|a - b| > eps` |
| `CHECK_STR_EQ(a, b)` | Fail if strings differ |
| `CHECK_NOT_NULL(p)` | Fail if pointer is null |
| `RUN(fn)` | Run a test function and report pass/fail |
| `test_summary()` | Print totals; returns 0 (pass) or 1 (fail) |

`test/mocks/Arduino.h` provides a no-op `Serial` stub so the source files
can call `Serial.println()` / `Serial.printf()` without linking against
any ESP32 library.