/**
 * wifi_debug.cpp — thin compatibility shim that forwards to WifiMgr.
 *
 * Previously this module owned the entire WiFi lifecycle: configuring
 * STA mode, kicking off association with NVS credentials, and
 * reconnecting on link drop. Those responsibilities now live in
 * src/network_manager.cpp, which uses an event-driven design (Wi-Fi
 * events, not WiFi.status() polling) and owns the connection state
 * machine.
 *
 * WifiDebug is kept as a public-API shim so console_server.cpp and any
 * external tooling that referenced its symbols keeps linking.
 *
 * Gated by ENABLE_WIFI_DEBUG so the link is only pulled in for dev
 * builds; shipping firmware has no wifi_debug symbols at all.
 */
#include "wifi_debug.h"

#if ENABLE_WIFI_DEBUG && !defined(UNIT_TEST)

#include <Arduino.h>
#include "network_manager.h"
#include "Log.h"

namespace {

bool _wasConnected = false;

}  // namespace

void WifiDebug::scanAndLog() {
    // Historical one-shot pre-association scan. Removed when the WiFi
    // stack moved to WifiMgr (which scans asynchronously from the
    // device keyboard). Kept as a no-op for link compatibility.
    Log::info("[WiFi] scanAndLog: use the 'C' key on the device for an async scan");
}

void WifiDebug::begin() {
    // No-op: WifiMgr::bindPlatformHal() + WifiMgr::begin() in main.cpp
    // owns station-mode setup. Calling WiFi.mode(WIFI_STA) here would
    // race with WifiMgr and break its event handlers.
    _wasConnected = false;
}

void WifiDebug::poll() {
    // No-op: WifiMgr::poll() in main.cpp advances the state machine.
    // We still log connection transitions so a developer can see them
    // on the USB monitor without watching /state.
    const bool connected = WifiMgr::isConnected();
    if (connected && !_wasConnected) {
        _wasConnected = true;
        const uint32_t ip = WifiMgr::localIP();
        Log::info("[WiFi] connected: %u.%u.%u.%u",
                  (unsigned)(ip >> 24), (unsigned)(ip >> 16),
                  (unsigned)(ip >> 8),  (unsigned)(ip));
    } else if (!connected && _wasConnected) {
        _wasConnected = false;
        Log::warning("[WiFi] disconnected, will retry");
    }
}

bool WifiDebug::isConnected() {
    return WifiMgr::isConnected();
}

uint32_t WifiDebug::localIP() {
    return WifiMgr::localIP();
}

#else  // ENABLE_WIFI_DEBUG == 0  OR  UNIT_TEST

// Stubs so callers under `#if ENABLE_WIFI_DEBUG` always link.
void     WifiDebug::begin()       {}
void     WifiDebug::poll()        {}
void     WifiDebug::scanAndLog()  {}
bool     WifiDebug::isConnected() { return false; }
uint32_t WifiDebug::localIP()     { return 0; }

#endif