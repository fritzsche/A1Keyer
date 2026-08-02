/**
 * winkey_serial.cpp — second USB-CDC interface (CDC1) for WinkeyBridge.
 *
 * Device build: owns a USBCDC instance bound to TinyUSB interface index
 * 1. Interface 0 is the boot CDC the arduino-esp32 core auto-creates and
 * maps to `Serial` (ARDUINO_USB_CDC_ON_BOOT=1), used for upload +
 * monitor + Log.h. We take interface 1 for the WinKeyer byte stream.
 *
 * Host (UNIT_TEST) build: no-op stubs so the link surface stays intact
 * without the ESP32 USB stack.
 *
 * NOTE on the init sequence: with ARDUINO_USB_CDC_ON_BOOT=1 the core has
 * already called USB.begin() before setup() runs, so we only construct
 * and begin() the second CDC here. If a future build sets CDC_ON_BOOT=0,
 * an explicit USB.begin() would be needed before the first CDC's begin().
 */
#include "winkey_serial.h"

#ifndef UNIT_TEST

#include <USB.h>
#include <USBCDC.h>

namespace {
// TinyUSB CDC interface index 1 (index 0 is the boot `Serial`). Requires
// CFG_TUD_CDC >= 2 in the build flags — see platformio.ini.
USBCDC _cdc1(1);
bool   _ready = false;
}  // namespace

void WinkeySerial::begin() {
    if (_ready) return;
    _cdc1.begin();
    _ready = true;
}

int WinkeySerial::available() {
    if (!_ready) return 0;
    return _cdc1.available();
}

int WinkeySerial::read() {
    if (!_ready) return -1;
    return _cdc1.read();
}

int WinkeySerial::write(uint8_t byte) {
    if (!_ready || !_cdc1) return 0;  // operator bool() → CDC line open
    return (int)_cdc1.write(byte);
}

size_t WinkeySerial::write(const uint8_t* data, size_t len) {
    if (!_ready || !_cdc1) return 0;
    return _cdc1.write(data, len);
}

void WinkeySerial::flush() {
    if (_ready) _cdc1.flush();
}

bool WinkeySerial::isReady() {
    return _ready;
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
