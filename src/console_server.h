#pragma once
/**
 * console_server.h — JSON snapshot + log tail endpoints.
 *
 * Routes are registered on the shared WebServer (`http_server.h`),
 * which also serves the on-device web UI from `web_ui.cpp`. Body is
 * `#if ENABLE_WIFI_DEBUG`; when the flag is off (default in shipping
 * builds) the WebServer library is not pulled in.
 *
 * Two endpoints registered here:
 *   GET /state        — JSON snapshot of every observable piece of
 *                       keyer state (MorseModel, WinkeyBridge,
 *                       RadioKeyer, KeyEventBus, AudioEngine, WiFi,
 *                       plus the full 10-slot memory bank).
 *   GET /log?n=N      — JSON array of the last N complete lines from
 *                       the passive LogRing (1..200, default 20).
 *
 * The on-device web UI lives in `web_ui.cpp` (GET /, POST
 * /api/settings, POST /api/memory). Both modules share the same
 * listener via `http_server.h`.
 *
 * Future iteration: swap the Arduino WebServer for an AsyncWebServer
 * if WebSocket streaming is needed, or move HTML/JS assets to
 * LittleFS if the inline asset grows past ~16 KB.
 */

#include <cstdint>

class ConsoleServer {
public:
    /// Register the dev console's two routes on the shared WebServer.
    /// Idempotent. Safe to call once from setup().
    /// No-op when ENABLE_WIFI_DEBUG=0.
    static void begin(uint16_t port = 80);

    /// Per-loop hook for symmetry with `WebUI::poll()`. Listener
    /// start/stop lives in `HttpServer::poll()`. No-op today.
    static void poll();
};