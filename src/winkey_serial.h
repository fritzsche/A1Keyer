#pragma once
/**
 * winkey_serial.h — second USB-CDC transport for the WinKeyer bridge.
 *
 * The firmware runs as a TinyUSB composite device with TWO CDC-ACM
 * interfaces (ARDUINO_USB_MODE=0, CFG_TUD_CDC=2 in platformio.ini):
 *
 *   CDC0  →  Arduino `Serial`  — firmware upload, `pio device monitor`,
 *            and Log.h debug output. Untouched by this module.
 *   CDC1  →  WinkeySerial       — the raw WinKeyer 2.x byte stream a host
 *            logger (e.g. RUMlogNG on macOS) opens as a second serial
 *            port. This module owns that interface.
 *
 * WinkeySerial is a thin byte-pipe: available()/read()/write()/flush().
 * The protocol logic lives in WinkeyBridge, which is transport-agnostic
 * (it takes bytes via feed() and emits via an output callback) so it can
 * be unit-tested on the host with no USB dependency. See docs/winkey.md
 * § 4.4 and § 16.
 *
 * On host unit-test builds (UNIT_TEST) every method is a no-op stub so
 * the symbol surface stays linkable without the ESP32 USB stack.
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
