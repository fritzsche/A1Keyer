/**
 * usb_reset.cpp — ensure the framework's USB auto-reset is enabled.
 *
 * With ARDUINO_USB_MODE=0 (TinyUSB) + ARDUINO_USB_CDC_ON_BOOT=1, `Serial`
 * is a USBCDC on interface 0. The arduino-esp32 core's USBCDC already
 * implements the esptool DTR/RTS reset sequence and the 1200-baud touch,
 * both calling usb_persist_restart(RESTART_BOOTLOADER) — the correct way
 * to put the ESP32-S3 ROM into serial-download mode over the native USB
 * port (it sets RTC_CNTL_FORCE_DOWNLOAD_BOOT and switches to the
 * USB-Serial-JTAG controller on the same USB-C connector). This is on by
 * default (reboot_enable = true).
 *
 * An earlier version of this file DISABLED that path (enableReboot(false))
 * and substituted a hand-rolled detector that drove GPIO0 low + esp_restart()
 * — which does NOT enter USB download mode on the S3 (GPIO0 is the UART
 * download strap, not USB). That broke `pio run -t upload`: esptool ended
 * up talking to the running app's CDC0 and saw our log text as "Invalid
 * head of packet (0x5B)". The fix is simply to leave the framework default
 * enabled; this module now just makes that explicit and is otherwise inert.
 */
#include "usb_reset.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <USB.h>
#include <USBCDC.h>

void UsbReset::begin() {
    // Explicitly (re-)enable the framework's built-in auto-reset on CDC0.
    // Default is already true; we set it so no other code path can leave
    // it disabled. No custom DTR/RTS handling is needed — the core's
    // USBCDC state machine handles esptool + the 1200-baud touch.
    Serial.enableReboot(true);
}

void UsbReset::poll() {
    // Nothing to do — reset detection lives in the core's USBCDC.
}

#else  // UNIT_TEST — no-op stubs

void UsbReset::begin() {}
void UsbReset::poll() {}

#endif  // UNIT_TEST
