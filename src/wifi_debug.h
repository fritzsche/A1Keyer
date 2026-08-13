#pragma once
/**
 * wifi_debug.h — dev-only WiFi STA lifecycle shim.
 *
 * Previously this module owned the entire WiFi lifecycle: configuring
 * STA mode, kicking off association with NVS credentials, and
 * reconnecting on link drop. Those responsibilities now live in
 * src/network_manager.cpp, which uses an event-driven design (Wi-Fi
 * events, not WiFi.status() polling) and owns the connection state
 * machine.
 *
 * WifiDebug is kept as a public-API shim so console_server.cpp and any
 * external tooling that referenced its symbols keeps linking. The
 * behavior of each function:
 *
 *   begin()       — no-op. main.cpp calls WifiMgr::begin() instead.
 *   poll()        — no-op. main.cpp calls WifiMgr::poll() instead.
 *   scanAndLog()  — no-op. Press 'C' on the device to run an async
 *                   scan and view results.
 *   isConnected() — forwards to WifiMgr::isConnected().
 *   localIP()     — forwards to WifiMgr::localIP().
 *
 * Gated by ENABLE_WIFI_DEBUG so the link is only pulled in for dev
 * builds; shipping firmware has no wifi_debug symbols at all.
 */

#include <cstdint>

#ifdef __ESP32__
#include <WiFi.h>     // IPAddress
#endif

class WifiDebug {
public:
    static void begin();
    static void poll();
    static void scanAndLog();
    static bool isConnected();
    static uint32_t localIP();
};