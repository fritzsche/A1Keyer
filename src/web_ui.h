#pragma once
/**
 * web_ui.h — on-device web UI for the A1Keyer.
 *
 * Registers three routes on the shared WebServer (`http_server.h`):
 *   GET  /                 — dark, mobile-responsive single-page app.
 *   POST /api/settings     — apply a partial settings patch (writes
 *                            MorseModel setters + NVS mirroring the
 *                            keyboard UI persistence in main.cpp).
 *   POST /api/memory       — update one of the 10 CW memory slots and
 *                            persist the bank to NVS.
 *
 * Mirrors `ConsoleServer`'s lifecycle exactly:
 *   setup()    → WebUI::begin(80) once.
 *   loop()     → WebUI::poll() every iteration.
 *
 * Gating: body is `#if ENABLE_WIFI_DEBUG`. When the flag is off the
 * module compiles to a pair of no-op stubs and the WebServer library
 * is not pulled in.
 */
#include <cstdint>

class WebUI {
public:
    /// Register UI routes on the shared WebServer. Idempotent. Call once
    /// from setup() after `HttpServer::begin()` and after the Wi-Fi STA
    /// link is brought up (route registration does not require the
    /// listener to be live).
    static void begin(uint16_t port = 80);

    /// Service the UI (lazy start/stop is inside `HttpServer::poll()`).
    /// Call every loop() iteration. No-op when ENABLE_WIFI_DEBUG=0.
    static void poll();
};
