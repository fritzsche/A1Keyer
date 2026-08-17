/**
 * http_server.cpp — single shared WebServer for the device web UI.
 *
 * Owns the `WebServer _server(80)` that backs every HTTP route on the
 * device (dev console + the new on-device web UI from `web_ui.cpp`).
 * Both modules call `HttpServer::addRoute()` once from their `begin()`
 * and share this module's `poll()`. A second `WebServer(80)` would
 * fail to bind — only one listener can live on port 80.
 *
 * Lazy start/stop mirrors the original `console_server.cpp` policy:
 * the listener comes up the first tick after `WifiMgr::isConnected()`
 * flips true, and goes back down on disconnect so we never advertise
 * a stale IP.
 *
 * Body fenced `#if ENABLE_WIFI_DEBUG && !defined(UNIT_TEST)`. Unit
 * tests get a no-op stub so they don't need their own `#if`.
 */
#include "http_server.h"

#if ENABLE_WIFI_DEBUG && !defined(UNIT_TEST)

#include <Arduino.h>
#include <WebServer.h>

#include "network_manager.h"

namespace {

WebServer _server(80);
bool     _listening = false;

}  // namespace

namespace HttpServer {

void addRoute(uint16_t code, const char* uri, RouteHandler handler) {
    // The cast is safe: HTTPMethod is an enum of small-int values
    // (HTTP_ANY=0, HTTP_GET=1, HTTP_POST=2, ...) that fits in uint16_t.
    _server.on(uri, (HTTPMethod)code, handler);
}

WebServer& server() {
    return _server;
}

void begin() {
    // Routes are bound by addRoute() at registration time (callers
    // invoke addRoute() before this), so begin() is a hook for
    // symmetry with ConsoleServer::begin/WebUI::begin and a place for
    // any one-shot initialization that may show up later.
}

void poll() {
    // Lazy start on first connected tick; stop on disconnect. Mirrors
    // the pre-refactor console_server.cpp behavior so the only change
    // is that multiple modules now share the listener.
    if (!_listening && WifiMgr::isConnected()) {
        _server.begin();
        _listening = true;
    } else if (_listening && !WifiMgr::isConnected()) {
        _server.stop();
        _listening = false;
    }
    if (_listening) {
        _server.handleClient();
    }
}

}  // namespace HttpServer

#else  // ENABLE_WIFI_DEBUG == 0  OR  UNIT_TEST

namespace HttpServer {

void addRoute(uint16_t, const char*, RouteHandler) {}
void begin() {}
void poll()   {}

}  // namespace HttpServer

#endif
