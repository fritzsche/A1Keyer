#pragma once
/**
 * usb_reset.h — always-on auto-reset for the composite USB-CDC device.
 *
 * Why this exists
 * ---------------
 * The ESP32-S3 Arduino core's built-in auto-reset
 * (USBCDC::_onLineState / USBCDC::_onLineCoding in USBCDC.cpp) does
 * not work for the Cardputer ADV because:
 *
 *   1. The DTR/RTS state machine only fires on the esptool.js sequence
 *      `!dtr&&rts → dtr&&rts → dtr&&!rts → !dtr&&!rts`, which is not
 *      what PlatformIO's esptool.py sends.
 *
 *   2. usb_persist_restart(RESTART_BOOTLOADER) calls
 *      usb_switch_to_cdc_jtag() (esp32-hal-tinyusb.c:638-642) before
 *      esp_restart(). On the Cardputer ADV the USB-C connector is
 *      wired to USB-OTG (GPIO19/20); USB-Serial-JTAG is not brought
 *      out externally, so the host loses the device even if the reset
 *      had fired.
 *
 * What this does
 * --------------
 * The module polls `Serial.dtr`, `Serial.rts`, and `Serial.bit_rate`
 * from `loop()` so it does not depend on the framework's event posting
 * (which is filtered by the failed state machine). It detects the
 * esptool.py DTR/RTS dance (≥ 3 alternating DTR/RTS transitions within
 * 500 ms of the first transition) and the Arduino-IDE 1200-baud touch.
 *
 * On a positive match, it drives GPIO0 LOW *and* enables the
 * GPIO pad hold (`gpio_hold_en`) BEFORE calling `esp_restart()`. The
 * pad hold keeps the pin state across the software reset, which is
 * essential: the strap pin is sampled by the external circuit at the
 * moment of reset, not by the GPIO peripheral's latches. Without the
 * hold, the on-board pull-up would pull GPIO0 HIGH before the ROM
 * bootloader sampled it, and the chip would boot normally instead of
 * into the download mode.
 *
 * The new firmware must call `gpio_hold_dis(GPIO_NUM_0)` at the very
 * top of `setup()` to release the hold; otherwise the device is
 * permanently stuck in download mode. The patch in main.cpp does
 * exactly that — it is the first statement of `setup()`.
 *
 * Pattern detector robustness
 * ---------------------------
 * The threshold (≥ 3 transitions within 500 ms) tolerates the
 * esptool.py v4 default sequence, which fires 4 transitions spaced
 * 50–100 ms apart. A regular terminal opens the port and leaves
 * DTR/RTS stable, producing 0–1 transitions, so it never reaches the
 * threshold.
 *
 * Usage
 * -----
 *   - Call UsbReset::begin() in setup() BEFORE Serial.begin().
 *   - Call UsbReset::poll()  in loop().
 *   - gpio_hold_dis(GPIO_NUM_0) at the very top of setup().
 */
class UsbReset {
public:
    /// Disable the framework's auto-reset on CDC0 and seed the polling
    /// state. Idempotent.
    static void begin();

    /// Poll CDC0's DTR/RTS/baud for the upload-reset patterns. Call
    /// once per iteration of loop().
    static void poll();

private:
    UsbReset() = delete;
};
