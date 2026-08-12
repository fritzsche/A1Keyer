#pragma once
/**
 * console_server.h — dev-only HTTP console for state queries + log tail.
 *
 * Three endpoints:
 *   GET /             — minimal HTML status page (placeholder for the
 *                       future on-device web UI).
 *   GET /state        — JSON snapshot of every observable piece of
 *                       keyer state (MorseModel, WinkeyBridge,
 *                       RadioKeyer, KeyEventBus, AudioEngine, WiFi).
 *   GET /log?n=N      — JSON array of the last N complete lines from
 *                       the passive LogRing (1..200, default 20).
 *
 * Body is `#if ENABLE_WIFI_DEBUG` — when the flag is off (default in
 * platformio.ini) the build skips every line and the WebServer library
 * is never pulled in. Shipping releases are unaffected.
 *
 * Future iteration: add POST /cmd for command execution, swap the
 * Arduino WebServer for an AsyncWebServer if WebSocket streaming is
 * needed, and serve the web UI assets from LittleFS.
 */

#include <cstdint>

class ConsoleServer {
public:
    /// Start the HTTP server on the given port. No-op when
    /// ENABLE_WIFI_DEBUG=0. Call after WifiMgr::isConnected().
    static void begin(uint16_t port = 80);

    /// Service pending HTTP clients. Call from loop().
    /// No-op when ENABLE_WIFI_DEBUG=0.
    static void poll();
};