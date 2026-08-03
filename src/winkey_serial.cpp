/**
 * winkey_serial.cpp — WinKeyer transport over the shared USB serial line.
 *
 * A1Keyer uses ONE hardware USB-Serial-JTAG port for both the debug
 * console and the WinKeyer protocol, switched by the 'D' key (see
 * console_io.h). This transport therefore reads and writes the SAME
 * `Serial` the console uses, through the Console mode gate:
 *
 *   - RX: Console::available()/read() return raw serial bytes. The caller
 *     (Winkey::poll) only feeds them to the bridge while in WinKey mode,
 *     so terminal keystrokes in Console mode are never parsed as WK bytes.
 *   - TX: Console::rawWinkeyWrite() writes real bytes to the wire even in
 *     WinKey mode (the one path allowed to, so the WK2 stream stays clean).
 *
 * Host (UNIT_TEST) build: no-op stubs.
 */
#include "winkey_serial.h"

#ifndef UNIT_TEST

#include "console_io.h"

void WinkeySerial::begin() {
    // The serial line is already up (Console::begin in setup()); nothing
    // to do here. Kept for API symmetry.
}

int WinkeySerial::available() {
    return Console::available();
}

int WinkeySerial::read() {
    return Console::read();
}

int WinkeySerial::write(uint8_t byte) {
    Console::rawWinkeyWrite(byte);
    return 1;
}

size_t WinkeySerial::write(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) Console::rawWinkeyWrite(data[i]);
    return len;
}

void WinkeySerial::flush() {
    // Console owns Serial; a flush isn't needed for correctness here.
}

bool WinkeySerial::isReady() {
    return true;
}

#else  // UNIT_TEST — host stubs

void   WinkeySerial::begin() {}
int    WinkeySerial::available() { return 0; }
int    WinkeySerial::read() { return -1; }
int    WinkeySerial::write(uint8_t) { return 0; }
size_t WinkeySerial::write(const uint8_t*, size_t) { return 0; }
void   WinkeySerial::flush() {}
bool   WinkeySerial::isReady() { return false; }

#endif  // UNIT_TEST
