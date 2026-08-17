#pragma once
/**
 * http_server.h — shared HTTP listener for the on-device web UI.
 *
 * A single Arduino `WebServer` instance on port 80 backs both the dev
 * console (`console_server.cpp`) and the on-device web UI
 * (`web_ui.cpp`). Two modules cannot each own their own `WebServer(80)`
 * — the second `bind()` would fail — so all routes register through
 * `HttpServer::addRoute` and share one `poll()` that lazy-starts the
 * listener once the STA link comes up and tears it down on disconnect.
 *
 * Lifecycle:
 *   setup()    → `HttpServer::begin()` after callers have registered
 *                their routes via `addRoute`.
 *   loop()     → `HttpServer::poll()` does `handleClient()` and the
 *                lazy start/stop driven by `WifiMgr::isConnected()`.
 *
 * Gating: `<WebServer.h>` is only available when the network stack is
 * enabled. The `server()` accessor (which returns a `WebServer&`) is
 * therefore gated on `ENABLE_WIFI_DEBUG` so the header compiles
 * unconditionally and unit tests don't need to mock the entire
 * networking stack. Both callers (`console_server.cpp`, `web_ui.cpp`)
 * already gate their bodies on the same flag, so calls to `server()`
 * only ever appear inside gated translation units.
 */
#include <cstdint>

// Forward declaration so the header is self-contained when the
// network stack isn't pulled in. `server()` is only available when
// ENABLE_WIFI_DEBUG is set.
class WebServer;

namespace HttpServer {

/// Handler is a plain C function pointer — matches the Arduino
/// `WebServer` library convention. Handlers reference the singleton
/// via `HttpServer::server()` to read request args and send replies.
using RouteHandler = void (*)();

/// Register a route. `code` is the HTTPMethod enum value cast to
/// uint16_t (HTTP_GET=1, HTTP_POST=2, …). Idempotent for the same
/// (code, uri) pair — last call wins. Safe to call from setup() before
/// the listener is up.
void addRoute(uint16_t code, const char* uri, RouteHandler handler);

/// Bind all registered routes. Idempotent. Safe to call once from
/// setup(). The HTTP listener itself starts lazily from poll().
void begin();

/// Drive the lazy start/stop gate and call `server().handleClient()`.
/// Safe to call every loop() iteration. No-op when ENABLE_WIFI_DEBUG=0.
void poll();

#if ENABLE_WIFI_DEBUG
/// Underlying WebServer reference — only callable from inside gated
/// translation units. Handlers use this to read request state and
/// write responses (`server().arg(...)`, `server().send(...)`, etc.).
WebServer& server();
#endif

}  // namespace HttpServer
