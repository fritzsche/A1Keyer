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
#include <cstdio>

namespace {

constexpr size_t kRingSize = 2048;   // replay buffer for WinKey-mode logs

Console::Mode _mode = Console::Mode::Console;

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
// MUST be called with _mux RELEASED. Serial.write() on USB-Serial-JTAG
// blocks on a FreeRTOS ring buffer (xRingbufferSend), which parks the task
// on an event list. Doing that inside portENTER_CRITICAL() disables
// interrupts, so the tick and the USB ISR that would drain the FIFO can
// never run — the task blocks forever and the interrupt watchdog panics
// the core. We therefore copy out of the ring in small chunks under short
// critical sections and write each chunk with the lock released.
void ringReplay() {
    char hdr[48];
    portENTER_CRITICAL(&_mux);
    size_t pending = _ringLen;
    portEXIT_CRITICAL(&_mux);
    if (pending == 0) return;

    int n = snprintf(hdr, sizeof(hdr),
                     "\r\n--- %u buffered log bytes ---\r\n",
                     (unsigned)pending);
    if (n > 0) Serial.write((const uint8_t*)hdr, (size_t)n);

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
        Serial.write((const uint8_t*)chunk, n2);   // lock released here
    }

    Serial.write((const uint8_t*)"\r\n--- end buffered ---\r\n", 24);
}

}  // namespace

void Console::begin() {
    Serial.begin(115200);
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
        // and ringReplay() is free to block in Serial.write() because no
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
        Serial.write(byte);
    } else {
        portENTER_CRITICAL(&_mux);
        ringPush(byte);
        portEXIT_CRITICAL(&_mux);
    }
}

void Console::write(const uint8_t* data, size_t len) {
    if (_mode == Mode::Console) {
        Serial.write(data, len);
    } else {
        portENTER_CRITICAL(&_mux);
        for (size_t i = 0; i < len; ++i) ringPush(data[i]);
        portEXIT_CRITICAL(&_mux);
    }
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
    Serial.write(byte);
}

int Console::available() {
    return Serial.available();
}

int Console::read() {
    return Serial.read();
}

#else  // UNIT_TEST — host passthrough stub (no Arduino, no output)

namespace { Console::Mode _mode = Console::Mode::Console; }

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
