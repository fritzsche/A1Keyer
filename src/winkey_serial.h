#pragma once
/**
 * winkey_serial.h — WinKeyer transport over the shared USB serial line.
 *
 * A1Keyer uses ONE hardware USB-Serial-JTAG port (ARDUINO_USB_MODE=1) for
 * both the debug console and the WinKeyer protocol, switched by the 'D'
 * key. This module is a thin byte-pipe over that same `Serial`, routed
 * through the Console mode gate (src/console_io.h):
 *
 *   - available()/read() — raw serial RX (the caller, Winkey::poll, only
 *     feeds these to the bridge while in WinKey mode).
 *   - write()            — via Console::rawWinkeyWrite(), the one path
 *     allowed to write real bytes to the wire in WinKey mode, so the WK2
 *     stream is never polluted by log output.
 *
 * The protocol logic lives in WinkeyBridge, which is transport-agnostic
 * (bytes in via feed(), bytes out via an OutputFn), so it is unit-tested
 * on the host with no USB dependency. See docs/winkey.md § 4.4 and § 16.
 *
 * On host unit-test builds (UNIT_TEST) every method is a no-op stub.
 */
#include <stdint.h>
#include <stddef.h>

class WinkeySerial {
public:
    /// Bring up the second CDC interface (interface index 1) on the
    /// TinyUSB composite device. Must be called from setup() after the
    /// USB stack is started. Idempotent.
    static void begin();

    /// Number of bytes waiting in the CDC1 RX buffer.
    static int available();

    /// Pop one RX byte. Returns -1 if none available.
    static int read();

    /// Write one byte to CDC1 TX (host ← device). Returns 1 on success,
    /// 0 if the interface is not open / not writable.
    static int write(uint8_t byte);

    /// Write a buffer to CDC1 TX. Returns the number of bytes written.
    static size_t write(const uint8_t* data, size_t len);

    /// Flush the CDC1 TX buffer.
    static void flush();

    /// True once begin() has run and the interface exists.
    static bool isReady();

private:
    WinkeySerial() = delete;  // static-only façade
};
