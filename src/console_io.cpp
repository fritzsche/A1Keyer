/**
 * console_io.cpp — mode-gated serial owner + log replay ring.
 *
 * See console_io.h for the design. Device build talks to the Arduino
 * `Serial` (hardware USB-Serial-JTAG). Host (UNIT_TEST) build is a plain
 * no-op/stdout-free passthrough so Log.h links without Arduino.
 */
#include "console_io.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <HWCDC.h>
#include <cstdio>
#include "Log.h"
#if ENABLE_WIFI_DEBUG
#include "log_ring.h"
#endif

namespace {

// kRingSize is the hard cap on the in-RAM replay buffer that captures
// every Console::write() byte while in WinKey mode. When the operator
// toggles back to Console mode the ring is drained onto the wire. The
// size is deliberately small (2 KB) — enough to retain a representative
// slice of recent activity, never enough to balloon memory. For full
// history while in WinKey mode, use the HTTP /log endpoint (LogRing is
// a separate 4 KB ring, also evict-oldest, populated by the same tap).
constexpr size_t kRingSize = 2048;

Console::Mode _mode = Console::Mode::WinKey;

// Private CDC instance. ARDUINO_USB_CDC_ON_BOOT=0 (see platformio.ini)
// leaves the USB-Serial-JTAG peripheral in its ROM-emitted JTAG state at
// boot, so the framework does NOT define the global `HWCDCSerial`. The
// `HWCDC` *class* is always available (HWCDC.h line 46 declares it
// whenever SOC_USB_SERIAL_JTAG_SUPPORTED), so we instantiate our own.
// All Console I/O goes through this instance; the framework's `Serial`
// (Serial0 / UART0) is intentionally untouched. There is only one USB
// CDC peripheral on the chip, so the static buffers inside HWCDC.cpp
// (tx_ring_buf, rx_queue) are bound to that single hardware endpoint —
// having one `HWCDC` instance vs. the framework's default global
// doesn't change anything at the hardware level.
HWCDC _cdc;

// Evict-oldest ring buffer (models the MorseModel decoded-text ring).
char   _ring[kRingSize];
size_t _ringHead = 0;   // index of oldest byte
size_t _ringLen  = 0;   // bytes stored

// Cross-core guard: the audio task (Core 1) and loop() (Core 0) both log.
portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

void ringPush(uint8_t b) {
    if (_ringLen < kRingSize) {
        _ring[(_ringHead + _ringLen) % kRingSize] = (char)b;
        ++_ringLen;
    } else {
        // Full: overwrite oldest, advance head.
        _ring[_ringHead] = (char)b;
        _ringHead = (_ringHead + 1) % kRingSize;
    }
}

// Replay the ring to the wire and clear it. Called on WinKey → Console.
//
// MUST be called with _mux RELEASED. _cdc.write() on USB-Serial-JTAG
// blocks on a FreeRTOS ring buffer (xRingbufferSend), which parks the
// task on an event list. Doing that inside portENTER_CRITICAL()
// disables interrupts, so the tick and the USB ISR that would drain the
// FIFO can never run — the task blocks forever and the interrupt
// watchdog panics the core. We therefore copy out of the ring in small
// chunks under short critical sections and write each chunk with the
// lock released.
void ringReplay() {
    char hdr[48];
    portENTER_CRITICAL(&_mux);
    size_t pending = _ringLen;
    portEXIT_CRITICAL(&_mux);
    if (pending == 0) return;

    int n = snprintf(hdr, sizeof(hdr),
                     "\r\n--- %u buffered log bytes ---\r\n",
                     (unsigned)pending);
    if (n > 0) _cdc.write((const uint8_t*)hdr, (size_t)n);

    for (;;) {
        char   chunk[64];
        size_t n2 = 0;
        portENTER_CRITICAL(&_mux);
        while (n2 < sizeof(chunk) && _ringLen > 0) {
            chunk[n2++] = _ring[_ringHead];
            _ringHead = (_ringHead + 1) % kRingSize;
            --_ringLen;
        }
        portEXIT_CRITICAL(&_mux);
        if (n2 == 0) break;
        _cdc.write((const uint8_t*)chunk, n2);   // lock released here
    }

    _cdc.write((const uint8_t*)"\r\n--- end buffered ---\r\n", 24);
}

}  // namespace

void Console::begin() {
    // Late CDC init. With ARDUINO_USB_CDC_ON_BOOT=0 (see platformio.ini)
    // the framework's `printBeforeSetupInfo()` does NOT bring up the CDC
    // peripheral, so `Serial` stays aliased to Serial0 (UART0, no
    // physical pins on Cardputer) and the USB-Serial-JTAG peripheral
    // remains in its ROM-emitted JTAG state. That keeps macOS's CDC
    // enumeration clean (the host sees a stable device descriptor until
    // we explicitly switch to CDC mode below).
    //
    // We bring up our own HWCDC instance (`_cdc`) AFTER M5.begin() has
    // settled pin ownership (see main.cpp::setup()), to avoid the
    // peripheral-manager pin-dance race that left the Mac's CDC driver
    // bound to a stale descriptor and required a manual reset.
    //
    // We deliberately do NOT call `_cdc.setDebugOutput(true)`. That would
    // reroute framework ets_printf traffic (log_e/log_w/log_i/log_d) into
    // our CDC TX, bypassing the mode gate and corrupting the WinKey
    // protocol stream when a framework log fires during a WinKey session.
    // Framework logs continue to flow via the chip's default ets_putc2
    // path (UART0 TX → USB-Serial-JTAG mirror), which is independent of
    // our CDC and cannot interfere with WK2 bytes.
    _cdc.begin(115200);
}

Console::Mode Console::mode() {
    return _mode;
}

bool Console::isWinKey() {
    return _mode == Mode::WinKey;
}

void Console::setMode(Mode m) {
    if (m == _mode) return;
    if (_mode == Mode::WinKey && m == Mode::Console) {
        // Flip the mode FIRST, under the lock, then replay with the lock
        // released. Flipping first matters twice over: concurrent loggers
        // go straight to the wire instead of refilling the ring (a chatty
        // task would otherwise keep the drain loop alive indefinitely),
        // and ringReplay() is free to block in _cdc.write() because no
        // critical section is held. Cost is that a log line raced in
        // during replay may interleave with the buffered text — cosmetic.
        portENTER_CRITICAL(&_mux);
        _mode = m;
        portEXIT_CRITICAL(&_mux);
        ringReplay();
        return;
    }
    // Entering WinKey mode (or any other transition): just switch. Start
    // the capture window from empty so replay shows only this session.
    portENTER_CRITICAL(&_mux);
    _ringHead = 0;
    _ringLen  = 0;
    _mode = m;
    portEXIT_CRITICAL(&_mux);
}

void Console::write(uint8_t byte) {
    if (_mode == Mode::Console) {
        _cdc.write(byte);
    } else {
        portENTER_CRITICAL(&_mux);
        ringPush(byte);
        portEXIT_CRITICAL(&_mux);
    }
#if ENABLE_WIFI_DEBUG
    // Passive tap for the HTTP /log endpoint. Mirrors every byte that
    // reaches the wire (or the WinKey replay ring) into LogRing. Kept
    // outside the critical section above for the same reason
    // ringReplay() is — see the comment above ringReplay().
    LogRing::instance().write(byte);
#endif
}

void Console::write(const uint8_t* data, size_t len) {
    if (_mode == Mode::Console) {
        _cdc.write(data, len);
    } else {
        portENTER_CRITICAL(&_mux);
        for (size_t i = 0; i < len; ++i) ringPush(data[i]);
        portEXIT_CRITICAL(&_mux);
    }
#if ENABLE_WIFI_DEBUG
    for (size_t i = 0; i < len; ++i) LogRing::instance().write(data[i]);
#endif
}

void Console::vprintf(const char* fmt, va_list args) {
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n <= 0) return;
    size_t len = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
    write((const uint8_t*)buf, len);
}

void Console::rawWinkeyWrite(uint8_t byte) {
    // Always to the wire, regardless of mode. Only the WinkeyBridge calls
    // this, and only while in WinKey mode.
    _cdc.write(byte);

    // Protocol-trace tap. Logged as a single byte per line so the wire
    // activity is greppable from the serial monitor and from the HTTP
    // /log endpoint while in WinKey mode (the LogRing tap is below in
    // Console::write, but rawWinkeyWrite bypasses that — we mirror it
    // here explicitly). One [INFO] line per byte keeps the format simple
    // for offline parsers.
    Log::info("[WK2 TX] %02X", byte);
}

int Console::available() {
    return _cdc.available();
}

int Console::read() {
    return _cdc.read();
}

#else  // UNIT_TEST — host passthrough stub (no Arduino, no output)

namespace { Console::Mode _mode = Console::Mode::WinKey; }

void         Console::begin() {}
Console::Mode Console::mode() { return _mode; }
bool         Console::isWinKey() { return _mode == Mode::WinKey; }
void         Console::setMode(Mode m) { _mode = m; }
void         Console::write(uint8_t) {}
void         Console::write(const uint8_t*, size_t) {}
void         Console::vprintf(const char*, va_list) {}
void         Console::rawWinkeyWrite(uint8_t) {}
int          Console::available() { return 0; }
int          Console::read() { return -1; }

#endif  // UNIT_TEST
