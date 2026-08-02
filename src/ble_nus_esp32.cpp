/**
 * ble_nus_esp32.cpp — NimBLE-Arduino plumbing for the NUS module.
 *
 * Compiled into the device firmware only. When built under UNIT_TEST
 * (host) every entry point becomes a no-op stub so the host parser
 * sees only standard C++.
 *
 * Responsibilities on-device:
 *   - Bring up the BT controller and NimBLE host via
 *     NimBLEDevice::init() (which calls nvs_flash_init + btStart +
 *     nimble_port_freertos_init internally — see NimBLE-Arduino's
 *     NimBLEDevice::init() in src/NimBLEDevice.cpp).
 *   - Create the GATT server, NUS service, RX (write+write-no-resp)
 *     and TX (notify+read) characteristics.
 *   - Build advertising fields and start/stop advertising.
 *   - Push RX bytes from the BLE callback into the ring buffer via
 *     ble_nus_internal::pushRxByte().
 *   - Implement BleNus::write() by issuing a notify on TX.
 *
 * Why NimBLE-Arduino and not the framework's BLEDevice wrapper?
 *
 * NimBLE-Arduino's NimBLEDevice::init() handles nvs_flash_init +
 * btStart + NimBLE host-task bring-up in a single, well-tested
 * sequence — the same one GhostBLE and ChimeraBLE use on Cardputer
 * with arduino-esp32 3.x. The framework's BLEDevice wrapper has a
 * known-broken init path on ESP32-S3 + NimBLE-only in arduino-esp32
 * 3.x (see docs/ble_error.md § 5 for the breakdown). We target
 * >= 2.1.0 (the API surface this file uses; same version
 * afpineda/NuS-NimBLE-Serial requires).
 *
 * Note: even with NimBLE-Arduino, the framework's NimBLE-only
 * sdkconfig for ESP32-S3 (the default in arduino-esp32 3.x) needs
 * adjustment before NimBLE will come up cleanly — see the file
 * "docs/ble_nimble_init.md" for the specific sdkconfig fixes we're
 * carrying in sdkconfig.defaults. Without those, esp_nimble_hci_init()
 * returns ESP_ERR_NO_MEM (257).
 */
#include "ble_nus.h"

#ifndef UNIT_TEST
#include "Log.h"
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <NimBLECharacteristic.h>
#include <NimBLEAdvertising.h>
#include <NimBLEAddress.h>
#include <esp_heap_caps.h>
#include <esp_efuse.h>
#include <esp_mac.h>
#include <string.h>

// ---------------------------------------------------------------------------
// File-local state for the BLE plumbing (device-only)
// ---------------------------------------------------------------------------
namespace {

// Canonical Nordic UART Service UUIDs (128-bit, public). We keep them
// here rather than reading the public-API constants in ble_nus.cpp
// because NimBLE-Arduino wants a string form, not a ble_uuid128_t.
const char* kNusServiceUuid = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const char* kNusRxUuid      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
const char* kNusTxUuid      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

NimBLEServer*         _pServer = nullptr;
NimBLEService*        _pService = nullptr;
NimBLECharacteristic* _pRxChar = nullptr;
NimBLECharacteristic* _pTxChar = nullptr;

bool _txSubscribed      = false;
bool _connected         = false;
bool _advertisingWanted = false;
bool _initialised       = false;
char _deviceName[32]    = "A1Keyer";

// Forward declarations for callback classes (defined below).
class ServerCallbacks;
class RxCallbacks;
class TxCallbacks;
static ServerCallbacks* _serverCb = nullptr;
static RxCallbacks*    _rxCb     = nullptr;
static TxCallbacks*    _txCb     = nullptr;

// ---------------------------------------------------------------------------
// Server callbacks — connection / disconnection transitions.
// ---------------------------------------------------------------------------
class ServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        (void)pServer;
        (void)connInfo;
        _connected = true;
        ble_nus_internal::setState(BleNus::State::Connected);
        Log::info("[BLE] host connected");
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo,
                      int reason) override {
        (void)pServer;
        (void)connInfo;
        _connected    = false;
        _txSubscribed = false;
        Log::info("[BLE] host disconnected (reason=%d)", reason);
        // Re-arm advertising so a fresh host can grab the link.
        if (ble_nus_internal::advertisingWanted()) {
            NimBLEDevice::getAdvertising()->start();
            ble_nus_internal::setState(BleNus::State::Advertising);
        } else {
            ble_nus_internal::setState(BleNus::State::Off);
        }
    }
};

// ---------------------------------------------------------------------------
// RX callbacks — host wrote bytes into the RX characteristic.
// ---------------------------------------------------------------------------
class RxCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* pCharacteristic,
                 NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        std::string value = pCharacteristic->getValue();
        size_t len = value.size();
        size_t stored = 0;
        for (size_t i = 0; i < len; ++i) {
            if (!ble_nus_internal::pushRxByte((uint8_t)value[i])) break;
            ++stored;
        }
        if (len > 0) {
            Log::info("[BLE] RX %u/%u bytes", (unsigned)stored, (unsigned)len);
        }
    }
};

// ---------------------------------------------------------------------------
// TX callbacks — host subscribed/unsubscribed to TX notifications.
// ---------------------------------------------------------------------------
class TxCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onSubscribe(NimBLECharacteristic* pCharacteristic,
                     NimBLEConnInfo& connInfo,
                     uint16_t subValue) override {
        (void)pCharacteristic;
        (void)connInfo;
        // subValue: bit 0 = notify enabled, bit 1 = indicate enabled.
        _txSubscribed = (subValue & 0x01) != 0;
        Log::info("[BLE] TX subscribe: notify=%u", (unsigned)_txSubscribed);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Internal helpers — exposed via the friend declaration in ble_nus.h
// ---------------------------------------------------------------------------
namespace ble_nus_internal {
    bool isInitialised()     { return _initialised; }
    bool advertisingWanted() { return _advertisingWanted; }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool BleNus::begin(const char* deviceName) {
    if (_initialised) return true;

    if (deviceName) {
        strncpy(_deviceName, deviceName, sizeof(_deviceName) - 1);
        _deviceName[sizeof(_deviceName) - 1] = '\0';
    }

    Log::info("[BLE] begin: name='%s'", _deviceName);

    // NimBLEDevice::init() handles nvs_flash_init + btStart + NimBLE
    // host-task bring-up correctly on ESP32-S3 + arduino-esp32 3.x.
    // It also wires up the framework's NimBLE HCI callbacks, so the
    // framework's BLEDevice::init() chain (which would conflict on
    // this combo) never runs.
    //
    // init() returns false when the BT controller or NimBLE host fails
    // to come up. On this no-PSRAM board the usual failure is the host
    // mbuf pools failing to allocate (esp_nimble_hci_init ->
    // ESP_ERR_NO_MEM) because internal SRAM is exhausted. We MUST check
    // the return: NimBLE-Arduino logs the error but still lets us call
    // createServer()/createService() on a dead stack, which then only
    // fails much later at "Host not synced" — masking the real cause.
    if (!NimBLEDevice::init(_deviceName)) {
        Log::error("[BLE] NimBLEDevice::init failed — largest free "
                   "internal block=%u B, free internal=%u B",
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        // Release whatever the controller did bring up so a later retry
        // starts from a clean slate rather than ESP_ERR_INVALID_STATE.
        NimBLEDevice::deinit(true);
        ble_nus_internal::setState(State::Error);
        return false;
    }
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // +9 dBm — Cardputer ADV antenna is fine
    Log::info("[BLE] NimBLEDevice::init OK (free internal=%u B)",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // No address manipulation: NimBLE uses the controller's default
    // (public, derived from the eFuse MAC) which is stable across boots
    // and is the canonical setup. Earlier iterations set a static-random
    // address to try to make the device appear in macOS System Settings —
    // that was chasing a non-problem: macOS System Settings does NOT show
    // custom-service BLE peripherals (NUS) at all, by design. A Mac must
    // use a scanner app (LightBlue / nRF Connect / Bluetooth Explorer) to
    // see and connect to this device. See docs/ble_error.md § 8e.

    // Create the server and register our callbacks.
    _pServer = NimBLEDevice::createServer();
    if (!_pServer) {
        Log::error("[BLE] NimBLEDevice::createServer failed");
        ble_nus_internal::setState(State::Error);
        return false;
    }
    _serverCb = new ServerCallbacks();
    _pServer->setCallbacks(_serverCb);
    Log::info("[BLE] server created");

    // Create the NUS service and its two characteristics.
    _pService = _pServer->createService(kNusServiceUuid);
    if (!_pService) {
        Log::error("[BLE] createService(NUS) failed");
        ble_nus_internal::setState(State::Error);
        return false;
    }

    // RX: host → device, write + write-no-response. The framework's
    // BLE2902 descriptor (deprecated in NimBLE-Arduino) is auto-created
    // when NOTIFY is set on TX, so we don't add one explicitly.
    _pRxChar = _pService->createCharacteristic(
        kNusRxUuid,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    _rxCb = new RxCallbacks();
    _pRxChar->setCallbacks(_rxCb);

    // TX: device → host, notify + read. Read returns the most-recent
    // value (we always set one byte before notify), but is essentially
    // unused — NUS clients subscribe to notify and ignore reads.
    _pTxChar = _pService->createCharacteristic(
        kNusTxUuid,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );
    _txCb = new TxCallbacks();
    _pTxChar->setCallbacks(_txCb);

    // NimBLE-Arduino 2.x: services are started automatically by the
    // server (NimBLEService::start() is now a deprecated no-op). The
    // service becomes visible to scanners once startAdvertising() runs.
    Log::info("[BLE] NUS service registered");

    // Build advertising with explicit packet construction.
    //
    // NimBLE-Arduino 2.5.0's high-level helpers (setName / addServiceUUID)
    // have their own routing logic that ignores call order and always puts
    // 128-bit UUIDs in the primary packet and routes the name to the scan
    // response when m_scanResp=true. That produces:
    //   primary: flags + UUID   scan response: name
    // …which causes "Unknown Device" on Windows (it reads the name from
    // the primary packet before issuing SCAN_REQ) and invisibility on macOS
    // (which needs the name in the primary packet for the Settings pane).
    //
    // We bypass the helpers and build the raw AD payloads directly via
    // NimBLEAdvertisementData so the layout is exactly:
    //   Primary ADV_IND : Flags (3 B) + Complete Name (9 B) = 12 B
    //   Scan response   : 128-bit NUS UUID (18 B)           = 18 B
    //
    // Verified packet bytes from serial log after this fix:
    //   primary:       02 01 06  08 09 41 31 4b 65 79 65 72
    //                  ^^^^^^^^  ^^ ^^ ^^^^^^^^^^^^^^^^^^^
    //                  flags=GD  len type  "A1Keyer"
    //   scan response: 11 07 9e ca dc 24 0e e5 a9 e0 93 f3 a3 b5 01 00 40 6e
    //                  ^^ ^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    //                  len type  NUS UUID (little-endian)
    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.setName(_deviceName);           // AD type 0x09, primary packet

    NimBLEAdvertisementData scanData;
    scanData.addServiceUUID(kNusServiceUuid); // AD type 0x07, scan response

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setAdvertisementData(advData);
    adv->setScanResponseData(scanData);
    // 160 × 0.625 ms = 100 ms advertising interval. The NimBLE default
    // (itvl=0 → controller-chosen, often 1.28 s on ESP-IDF 5.x) is too
    // slow for macOS's BLE scanner and can cause the device to be missed.
    adv->setAdvertisingInterval(160);

    _initialised = true;
    ble_nus_internal::setState(State::Off);
    Log::info("[BLE] initialised as '%s'", _deviceName);
    return true;
}

void BleNus::startAdvertising() {
    Log::info("[BLE] startAdvertising: initialised=%d state=%d",
              (int)_initialised, (int)state());
    if (!_initialised) {
        if (!begin(nullptr)) {
            Log::error("[BLE] begin() returned false during startAdvertising");
            return;
        }
    }
    if (state() == State::Connected) {
        Log::info("[BLE] already connected — not restarting advertising");
        return;
    }
    _advertisingWanted = true;
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv->start()) {
        ble_nus_internal::setState(State::Advertising);
        Log::info("[BLE] advertising as '%s'", _deviceName);
    } else {
        Log::error("[BLE] advertising start failed");
        ble_nus_internal::setState(State::Error);
    }
}

void BleNus::stopAdvertising() {
    _advertisingWanted = false;
    if (_initialised) {
        NimBLEDevice::getAdvertising()->stop();
    }
    if (state() != State::Connected) {
        ble_nus_internal::setState(State::Off);
    }
    Log::info("[BLE] advertising stopped");
}

int BleNus::write(uint8_t byte) {
    if (!_initialised || !_connected || !_txSubscribed || !_pTxChar) {
        return 0;
    }
    _pTxChar->setValue(&byte, 1);
    _pTxChar->notify();
    return 1;
}

#else  // UNIT_TEST — host stubs so the symbol surface is linkable but inert

namespace ble_nus_internal {
    bool isInitialised() { return false; }
    bool advertisingWanted() { return false; }
}

bool BleNus::begin(const char* /*deviceName*/) { return false; }
void BleNus::startAdvertising() {}
void BleNus::stopAdvertising() {}
int BleNus::write(uint8_t /*byte*/) { return 0; }

#endif  // UNIT_TEST