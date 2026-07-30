/**
 * ble_nus.cpp — Platform-agnostic state + ring buffer for the NUS module.
 *
 * This TU deliberately avoids every Arduino BLE header so the host
 * test build can compile and link it without any ESP32 dependency.
 * It defines the unambiguous members of BleNus (state, ring buffer,
 * read/available/resetForTest) and the ble_nus_internal:: helpers
 * that the on-device TU's BLE callback uses.
 *
 * The BLE-stack members (begin / startAdvertising / stopAdvertising /
 * write) are defined in ble_nus_esp32.cpp — real definitions when
 * compiled for the device, no-op stubs when compiled under UNIT_TEST
 * for the host.
 */
#include "ble_nus.h"

#include <atomic>
#include <string.h>

// ---------------------------------------------------------------------------
// NUS UUIDs (Nordic standard, public)
// ---------------------------------------------------------------------------
const char* const BleNus::kServiceUuid = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const char* const BleNus::kRxUuid     = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
const char* const BleNus::kTxUuid     = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
const char* const BleNus::kDefaultDeviceName = "A1Keyer";

namespace {

// Ring buffer: head = next write slot, tail = next read slot.
volatile uint8_t _rxBuffer[BleNus::kRxBufferSize];
volatile size_t  _rxHead = 0;
volatile size_t  _rxTail = 0;
volatile size_t  _rxCount = 0;

// The atomic is the BLE state value visible to both TUs. The state
// transitions happen in ble_nus_esp32.cpp's callbacks; readers (state(),
// write()) come from both TUs.
std::atomic<BleNus::State> _state{BleNus::State::Off};

}  // namespace

// ---------------------------------------------------------------------------
// Internal helpers — exposed for the platform-specific TU
// ---------------------------------------------------------------------------
namespace ble_nus_internal {

bool pushRxByte(uint8_t b) {
    if (_rxCount >= BleNus::kRxBufferSize) return false;
    _rxBuffer[_rxHead] = b;
    _rxHead = (_rxHead + 1) % BleNus::kRxBufferSize;
    ++_rxCount;
    return true;
}

void setState(BleNus::State s) {
    _state.store(s, std::memory_order_release);
}

}  // namespace ble_nus_internal

// Note: ble_nus_internal::isInitialised() and advertisingWanted() are
// defined in ble_nus_esp32.cpp (device TU) and in the host stub block
// there. ble_nus.cpp deliberately does not provide them so the device
// TU owns the file-local state.

// ---------------------------------------------------------------------------
// Public API — members that exist on both device and host
// ---------------------------------------------------------------------------
BleNus::State BleNus::state() {
    return _state.load(std::memory_order_acquire);
}

int BleNus::available() {
    return (int)_rxCount;
}

int BleNus::read() {
    if (_rxCount == 0) return -1;
    uint8_t b = _rxBuffer[_rxTail];
    _rxTail = (_rxTail + 1) % BleNus::kRxBufferSize;
    --_rxCount;
    return (int)b;
}

void BleNus::resetForTest() {
    _rxHead = 0;
    _rxTail = 0;
    _rxCount = 0;
    _state.store(State::Off, std::memory_order_release);
}
