/**
 * usb_reset.cpp — composite-USB-aware auto-reset handler.
 *
 * Bypasses the framework's auto-reset (which routes via
 * USB-Serial-JTAG and is not externally wired on the Cardputer ADV,
 * and which has a state machine that doesn't match the esptool.py
 * PlatformIO sends). Registers LINE_STATE and LINE_CODING event
 * handlers on CDC0 and drives GPIO0 LOW with the pad-hold function
 * before esp_restart(), so the ROM bootloader sees the strap pin LOW
 * at boot and enumerates over USB-OTG. See usb_reset.h for the design
 * rationale.
 */
#include "usb_reset.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <USB.h>
#include <USBCDC.h>
#include <driver/gpio.h>

namespace {

// ─── Sliding-window DTR/RTS toggle counter ─────────────────────────────
// esptool.py fires 3–5 DTR/RTS transitions in ~50–100 ms when opening
// the port to signal "reset into download mode". A regular terminal
// opens the port and leaves DTR/RTS stable (0 or 1 edge).
uint32_t _firstChangeMs = 0;
uint32_t _lastChangeMs  = 0;
int      _toggleCount   = 0;
bool     _lastDtr       = false;
bool     _lastRts       = false;

// ─── Reset action ──────────────────────────────────────────────────────
void enterDownloadMode(const char* reason) {
    Serial.printf("\n[USB] %s → entering download mode\n", reason);
    Serial.flush();

    // Drive GPIO0 LOW and HOLD it across the reset. The strap pin is
    // sampled by the external circuit at the moment of reset, so we
    // must use the pad-hold function — not just digitalWrite, which
    // would be reset by the GPIO peripheral before the ROM bootloader
    // sampled the pin.
    gpio_config_t io_conf;
    io_conf.intr_type   = GPIO_INTR_DISABLE;
    io_conf.mode        = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_0);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(GPIO_NUM_0, 0);
    gpio_hold_en(GPIO_NUM_0);

    delay(50);
    esp_restart();
}

// ─── Event handler ─────────────────────────────────────────────────────
void onCdcEvent(void* /*handler_arg*/, esp_event_base_t /*base*/,
                int32_t event_id, void* event_data) {
    if (!event_data) return;
    auto* data = static_cast<arduino_usb_cdc_event_data_t*>(event_data);

    // ─── DTR/RTS dance (esptool.py default_reset) ─────────────────────
    if (event_id == ARDUINO_USB_CDC_LINE_STATE_EVENT) {
        const bool dtr = data->line_state.dtr;
        const bool rts = data->line_state.rts;
        const uint32_t now = millis();

        // Re-arm the window if the gap between transitions is too large
        // (> 200 ms) — that means the previous burst is over and we are
        // starting a fresh observation window.
        if (_toggleCount == 0 || (now - _lastChangeMs) > 200) {
            _firstChangeMs = now;
            _toggleCount   = 0;
        }
        if (dtr != _lastDtr) {
            _toggleCount++;
            _lastDtr = dtr;
            _lastChangeMs = now;
        }
        if (rts != _lastRts) {
            _toggleCount++;
            _lastRts = rts;
            _lastChangeMs = now;
        }

        // esptool.py fires ≥ 3 edge transitions within ~50–100 ms of
        // the first transition. A terminal that opens the port and
        // waits produces 0–1 transitions and never reaches this.
        if (_toggleCount >= 3 && (now - _firstChangeMs) < 500) {
            enterDownloadMode("esptool DTR/RTS reset");
        }
        return;
    }

    // ─── 1200-baud touch (Arduino IDE serial-monitor upload) ──────────
    if (event_id == ARDUINO_USB_CDC_LINE_CODING_EVENT) {
        if (data->line_coding.bit_rate == 1200) {
            enterDownloadMode("1200-baud touch");
        }
        return;
    }
}

}  // namespace

void UsbReset::begin() {
    // Disable the framework's auto-reset path. Its DTR/RTS state
    // machine does not match PlatformIO's sequence, and its
    // usb_persist_restart(RESTART_BOOTLOADER) routes through
    // usb_switch_to_cdc_jtag() — which is not wired to the USB-C
    // connector on the Cardputer ADV.
    Serial.enableReboot(false);

    // Seed the detector state.
    _lastDtr       = false;
    _lastRts       = false;
    _firstChangeMs = 0;
    _lastChangeMs  = 0;
    _toggleCount   = 0;

    // Register our handlers on CDC0. With enableReboot(false) the
    // framework's state machine is bypassed, so LINE_STATE events are
    // posted on every DTR/RTS change rather than being filtered.
    Serial.onEvent(ARDUINO_USB_CDC_LINE_STATE_EVENT,  onCdcEvent);
    Serial.onEvent(ARDUINO_USB_CDC_LINE_CODING_EVENT, onCdcEvent);
}

void UsbReset::poll() {
    // Detection is event-driven; poll() is a no-op kept for symmetry
    // with the original API and is expected to be called from loop().
}

#else  // UNIT_TEST — no-op stubs

void UsbReset::begin() {}
void UsbReset::poll() {}

#endif  // UNIT_TEST
