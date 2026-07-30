#pragma once
/**
 * ble_nus.h — Nordic UART Service (NUS) over BLE for A1Keyer.
 *
 * A1Keyer exposes a single Bluetooth Low Energy GATT service that mimics
 * the well-known Nordic UART Service. The service presents two
 * characteristics:
 *
 *   - RX  (6E400002-B5A3-F393-E0A9-E50E24DCCA9E)
 *          Host → Device. Host writes bytes here; the firmware appends
 *          them to an internal ring buffer the rest of the firmware
 *          drains via read().
 *   - TX  (6E400003-B5A3-F393-E0A9-E50E24DCCA9E)
 *          Device → Host. Firmware writes bytes here; they are pushed
 *          to the host via notify. Each write() sends one notification.
 *
 * The wire-level framing is therefore identical to NUS on any other
 * BLE-capable host (nRF Toolbox, LightBlue, a Mac running Core
 * Bluetooth). This is the transport a future Winkeyer bridge will sit
 * on top of; this first step only exposes the byte-level pass-through
 * so the user can sanity-check connectivity manually.
 *
 * Step-by-step plan:
 *   1. (this file) Toggle BLE on/off with the 'b'/'B' Cardputer key.
 *   2. Confirm a Mac can find the device, connect, and round-trip bytes.
 *   3. Plug the WinkeyBridge (see docs/winkey.md § 16) onto this
 *      transport as an additional producer on top of NUS.
 *
 * Concurrency: the BLE callback runs on a FreeRTOS task the Arduino BLE
 * library owns. That callback writes into the RX ring buffer. read(),
 * available(), write() run from loop() on the main task. The ring
 * buffer is a SPSC queue protected by a critical section (disable
 * interrupts) on both sides — short critical sections are fine for a
 * 64-byte buffer.
 *
 * Dependencies: this module uses only the Arduino BLE library that is
 * already bundled in the espressif32 Arduino framework, so no external
 * dependencies are added.
 *
 * Build layout:
 *   - ble_nus.h         — public API.
 *   - ble_nus.cpp       — platform-agnostic state + ring buffer +
 *                         read/available/resetForTest.
 *   - ble_nus_esp32.cpp — Arduino BLE library plumbing (server,
 *                         service, characteristics, advertising,
 *                         notify). Compiled ONLY into the device
 *                         firmware. The host test build sees a no-op
 *                         stub so the symbol surface stays linkable.
 */
#include <stdint.h>
#include <stddef.h>

class BleNus {
public:
    /// BLE state visible from loop(). Off = uninitialised or advertising
    /// stopped. Advertising = visible, no connection. Connected = a host
    /// has subscribed to TX. Error = BLE stack reported a fault.
    enum class State : uint8_t {
        Off = 0,
        Advertising = 1,
        Connected = 2,
        Error = 3,
    };

    /// Bring up the BLE stack and create the NUS service. Idempotent.
    /// Returns true on success, false if the stack could not be
    /// brought up. Does NOT start advertising — call startAdvertising()
    /// explicitly.
    static bool begin(const char* deviceName = kDefaultDeviceName);

    /// Start (or restart) advertising. No-op when already connected.
    static void startAdvertising();

    /// Stop advertising. If a host is connected, the link is dropped
    /// at the supervisor's pace (the Arduino BLE library does not
    /// expose disconnect()).
    static void stopAdvertising();

    /// Current BLE state. Cheap atomic load.
    static State state();

    /// Number of bytes available in the RX ring buffer (host wrote).
    static int available();

    /// Pop one byte from the RX ring buffer. Returns -1 if empty.
    static int read();

    /// Push one byte to the TX path. On a connected host, this fires
    /// a notify. On an unconnected host, the byte is dropped.
    /// Returns 1 on notify, 0 on drop.
    static int write(uint8_t byte);

    /// Reset state. Used by host unit tests between RUN() cases so
    /// state from a previous test does not leak.
    static void resetForTest();

    /// NUS service + characteristic UUIDs (canonical, public).
    static const char* const kServiceUuid;
    static const char* const kRxUuid;
    static const char* const kTxUuid;

    /// Default advertised device name (visible in macOS Bluetooth
    /// settings, iOS LightBlue, Windows Bluetooth). Short on
    /// purpose: a longer name costs a full extra scan-response slot.
    static const char* const kDefaultDeviceName;

    /// Capacity of the host → device ring buffer. Small on purpose.
    static constexpr int kRxBufferSize = 64;

private:
    BleNus() = delete;  // static-only façade
};

/// Functions exposed via the friend declaration below. Used by the
/// platform-specific TU's BLE callback to push RX bytes and update
/// state without exposing the internals across the public header.
namespace ble_nus_internal {
    /// Push one host-side byte into the RX ring buffer. Returns true
    /// if stored, false if the buffer was full.
    bool pushRxByte(uint8_t b);

    /// Set the BLE state from BLE callback contexts.
    void setState(BleNus::State s);

    /// True if the firmware is initialised (begin() succeeded).
    bool isInitialised();

    /// True if the user has asked for advertising to be running. Used
    /// by the NimBLE gap callback to know whether to re-arm advertising
    /// after a timeout / disconnect.
    bool advertisingWanted();
}
