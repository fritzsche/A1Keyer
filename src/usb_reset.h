#pragma once
/**
 * usb_reset.h — keep the framework's USB auto-reset enabled.
 *
 * Why this exists
 * ---------------
 * With ARDUINO_USB_MODE=0 (TinyUSB composite, two CDC ports) the
 * arduino-esp32 core's built-in USB auto-reset on `Serial` (CDC0) is the
 * correct upload mechanism:
 *
 *   - USBCDC::_onLineState() implements the esptool DTR/RTS reset
 *     sequence (IDLE → !dtr&rts → dtr&rts → dtr&!rts → !dtr&!rts) and
 *     USBCDC::_onLineCoding() implements the 1200-baud touch. Both call
 *     usb_persist_restart(RESTART_BOOTLOADER). This IS the sequence
 *     PlatformIO's esptool sends.
 *
 *   - On the ESP32-S3, usb_persist_restart(RESTART_BOOTLOADER) sets
 *     RTC_CNTL_FORCE_DOWNLOAD_BOOT and switches the native USB pins
 *     (GPIO19/20 → the USB-C connector) to the USB-Serial-JTAG
 *     controller. USB-OTG and USB-Serial-JTAG share those pins, so the
 *     ROM download comes up over the SAME USB-C cable — the standard
 *     ESP32-S3 flashing path.
 *
 * It is enabled by default (reboot_enable = true). This module exists
 * only to make that explicit, in case some other code path disables it.
 *
 * Historical note: an earlier version of this module DISABLED the
 * framework path (enableReboot(false)) and drove GPIO0 low + esp_restart()
 * instead — on the wrong assumption that the core state machine didn't
 * match esptool and that USB-Serial-JTAG wasn't reachable. Both were
 * incorrect. GPIO0 is the UART-download strap, not USB, so that hack
 * never entered USB download mode; esptool then talked to the running
 * app's CDC0 and reported "Invalid head of packet (0x5B)" (our log text).
 *
 * Usage
 * -----
 *   - Call UsbReset::begin() in setup().
 *   - UsbReset::poll() is a no-op (kept for API symmetry).
 */
class UsbReset {
public:
    /// Ensure the framework's auto-reset on CDC0 is enabled. Idempotent.
    static void begin();

    /// No-op — reset detection lives in the core's USBCDC. Kept for
    /// API symmetry with the loop() call site.
    static void poll();

private:
    UsbReset() = delete;
};
