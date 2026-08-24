/**
 * console_server.cpp — JSON snapshot + log tail endpoints.
 *
 * Routes are registered on the shared WebServer (`http_server.h`),
 * which also serves the on-device web UI from `web_ui.cpp`. Body is
 * `#if ENABLE_WIFI_DEBUG`; when the flag is off the WebServer library
 * is not pulled in and shipping firmware has no networking stack.
 *
 * JSON is built with snprintf into Arduino String — keeps the
 * dependency footprint small (no ArduinoJson). Output sizes are
 * bounded by the number of decoded-text chars (≤200) and a fixed set
 * of numeric fields; String reallocations are rare and cheap on
 * ESP32-S3.
 *
 * Lazy start/stop is driven by `HttpServer::poll()`, called from the
 * loop. `ConsoleServer::poll()` is a no-op hook kept for symmetry
 * with `WebUI::poll()` and to leave the call site in main.cpp
 * unchanged in shape.
 */
#include "console_server.h"

#if ENABLE_WIFI_DEBUG && !defined(UNIT_TEST)

#include <Arduino.h>
#include <WebServer.h>
#include "network_manager.h"
#include "http_server.h"
#include "console_io.h"
#include "display_model.h"
#include "winkey.h"
#include "winkey_bridge.h"
#include "winkey_buffer.h"
#include "radio_keyer.h"
#include "audio_engine.h"
#include "key_event_bus.h"
#include "log_ring.h"
#include "memory_store.h"

namespace {

// jsonEscape — append src to dst with JSON-string escaping.
// Avoids std::string to keep the dependency footprint minimal.
void jsonEscape(String& dst, const char* src) {
    if (!src) return;
    while (*src) {
        char c = *src++;
        switch (c) {
            case '"':  dst += "\\\""; break;
            case '\\': dst += "\\\\"; break;
            case '\n': dst += "\\n";  break;
            case '\r': dst += "\\r";  break;
            case '\t': dst += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    dst += buf;
                } else {
                    dst += c;
                }
        }
    }
}

// ─── GET /state — JSON snapshot of every observable piece of state ───
//
// Schema is additive: existing fields stay; the decoded-text tail was
// bumped from 50 to TEXT_BUF_SIZE (=200) and the full 10-slot memory
// bank was added so the web UI can render without an extra round-trip.
void handleState() {
    const auto& m  = MorseModel::instance();
    const WinkeyBridge* wk = Winkey::bridge();

    // Decoded text: full ring buffer, oldest first.
    String decoded;
    decoded.reserve(MorseModel::TEXT_BUF_SIZE + 16);
    const size_t tl = m.decodedTextLen();
    const size_t want = MorseModel::TEXT_BUF_SIZE;
    const size_t start = (tl > want) ? (tl - want) : 0;
    const size_t tail  = m.textTail();
    for (size_t i = start; i < tl; ++i) {
        size_t idx = (tail + i) % MorseModel::TEXT_BUF_SIZE;
        char c = m.textAt(idx);
        if (c) decoded += c;
    }

    auto  ip    = IPAddress(WifiMgr::localIP());
    char  ipStr[20];
    snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u",
             ip[0], ip[1], ip[2], ip[3]);

    String body;
    body.reserve(2048);
    body += '{';

    body += "\"wpm\":";              body += m.wpm();
    body += ",\"frequency\":";       body += (int)m.frequency();
    body += ",\"volume\":";          body += m.volume();
    body += ",\"keyerType\":\"";     body += (m.keyerType() == KeyerType::PADDLE) ? "paddle" : "straight"; body += '"';
    body += ",\"mode\":\"";          body += (m.mode() == KeyerMode::ENCODER) ? "encoder" : "keyer";     body += '"';
    body += ",\"radioKeyingEnabled\":"; body += m.radioKeyingEnabled() ? "true" : "false";
    body += ",\"winkeyMode\":";      body += m.winkeyMode() ? "true" : "false";
    body += ",\"radioKeyed\":";      body += RadioKeyer::isKeyed() ? "true" : "false";
    body += ",\"radioDownCount\":";  body += RadioKeyer::downCount();
    body += ",\"radioUpCount\":";    body += RadioKeyer::upCount();
    body += ",\"keyerDemand\":";     body += KeyEventBus::demand();
    body += ",\"keyerPatternPercent\":"; body += m.keyerPatternPercent();
    body += ",\"encoderChar\":\"";   if (m.encoderChar()) body += m.encoderChar(); body += '"';
    body += ",\"wifiIP\":\"";        body += ipStr; body += '"';
    body += ",\"wifiMode\":\"";      body += (WifiMgr::mode() == NetMode::ACCESS_POINT) ? "ap" : "sta"; body += '"';

    if (wk) {
        body += ",\"winkeyOpen\":";       body += wk->isOpen() ? "true" : "false";
        body += ",\"winkeyWpm\":";        body += wk->wpm();
        body += ",\"winkeySidetoneHz\":"; body += wk->sidetoneHz();
        body += ",\"winkeyKeyerMode\":";  body += (unsigned)wk->keyerMode();
    }

    body += ",\"decoded\":\"";       jsonEscape(body, decoded.c_str()); body += '"';

    // Memory bank: all 10 slots, JSON-escaped, oldest to newest.
    body += ",\"memory\":[";
    for (uint8_t i = 0; i < kMemSlots; ++i) {
        if (i > 0) body += ',';
        body += '"';
        jsonEscape(body, m.getMemory(i));
        body += '"';
    }
    body += ']';

    body += '}';

    HttpServer::server().send(200, "application/json", body);
}

// ─── GET /log?n=N — JSON array of recent log lines ────────────────────
void handleLog() {
    auto& server = HttpServer::server();
    if (!server.hasArg("n")) {
        server.send(400, "application/json", "{\"error\":\"missing n\"}");
        return;
    }
    long n = server.arg("n").toInt();
    if (n < 1)   n = 1;
    if (n > 200) n = 200;

    std::string json = LogRing::instance().snapshotLines((size_t)n);
    server.send(200, "application/json", json.c_str());
}

}  // namespace

void ConsoleServer::begin(uint16_t port) {
    // Routes are registered on the shared WebServer. `HttpServer::begin()`
    // does the lazy listener start; this method just binds the dev
    // console endpoints that the on-device web UI also depends on
    // (the /state JSON snapshot is what populates the settings card).
    (void)port;
    using namespace HttpServer;
    addRoute((uint16_t)HTTP_GET, "/state", handleState);
    addRoute((uint16_t)HTTP_GET, "/log",   handleLog);
}

void ConsoleServer::poll() {
    // No per-tick work — listener start/stop and handleClient() now
    // live in `HttpServer::poll()`. Kept as a hook so the call site
    // in main.cpp doesn't need to change shape.
}

#else  // ENABLE_WIFI_DEBUG == 0  OR  UNIT_TEST

void ConsoleServer::begin(uint16_t) {}
void ConsoleServer::poll() {}

#endif
