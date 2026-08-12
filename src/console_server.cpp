/**
 * console_server.cpp — dev-only HTTP console for state + log tail.
 *
 * See console_server.h. Body is `#if ENABLE_WIFI_DEBUG`; when the flag
 * is off the WebServer library is not pulled in and shipping firmware
 * has no networking stack beyond what WiFiDebug brings (and WiFiDebug
 * is also gated, so a default build has nothing at all).
 *
 * JSON is built with snprintf into Arduino String — keeps the dep
 * footprint small (no ArduinoJson). Output sizes are bounded by the
 * number of decoded-text chars (≤200) and a fixed set of numeric
 * fields; String reallocations are rare and cheap on ESP32-S3.
 */
#include "console_server.h"

#if ENABLE_WIFI_DEBUG && !defined(UNIT_TEST)

#include <Arduino.h>
#include <WebServer.h>
#include "network_manager.h"
#include "console_io.h"
#include "display_model.h"
#include "winkey.h"
#include "winkey_bridge.h"
#include "winkey_buffer.h"
#include "radio_keyer.h"
#include "audio_engine.h"
#include "key_event_bus.h"
#include "log_ring.h"

namespace {

WebServer _server(80);

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

// ─── GET / — minimal HTML status page ─────────────────────────────────
void handleRoot() {
    const auto& m  = MorseModel::instance();
    const uint32_t ip = WifiMgr::localIP();
    char  ipStr[20];
    snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u",
             (unsigned)(ip >> 24), (unsigned)(ip >> 16),
             (unsigned)(ip >> 8),  (unsigned)(ip));

    String body;
    body.reserve(1024);
    body += F("<!doctype html><html><head><meta charset=utf-8>"
              "<title>A1Keyer</title>"
              "<style>body{font-family:monospace;margin:1em}"
              "pre{background:#f4f4f4;padding:0.5em}"
              "h1{font-size:1.2em}</style></head><body>"
              "<h1>A1Keyer — dev console</h1>"
              "<p>State and log endpoints live at "
              "<code>/state</code> and <code>/log?n=N</code>.</p>"
              "<pre id=s>");

    char line[160];
    const char* modeStr = (m.mode() == KeyerMode::ENCODER) ? "ENCODER" : "KEYER";
    const char* keyerStr = (m.keyerType() == KeyerType::PADDLE) ? "paddle" : "straight";
    snprintf(line, sizeof(line),
             "WiFi IP    : %s\n"
             "WPM        : %d\n"
             "Frequency  : %.0f Hz\n"
             "Volume     : %d%%\n"
             "Keyer mode : %s\n"
             "Keyer type : %s\n"
             "Radio key  : %s (keyed=%s)\n"
             "WinKey mode: %s\n"
             "Encoder ch : '%c'\n",
             ipStr,
             m.wpm(),
             m.frequency(),
             m.volume(),
             modeStr,
             keyerStr,
             m.radioKeyingEnabled() ? "ON" : "off",
             RadioKeyer::isKeyed()   ? "YES" : "no",
             m.winkeyMode()          ? "WK2" : "Console",
             m.encoderChar() ? m.encoderChar() : ' ');
    body += line;

    if (const WinkeyBridge* wk = Winkey::bridge()) {
        snprintf(line, sizeof(line),
                 "WK host open: %s\n"
                 "WK WPM      : %d\n"
                 "WK sidetone : %d Hz\n"
                 "WK keyerMode: %u\n",
                 wk->isOpen()       ? "yes" : "no",
                 wk->wpm(),
                 wk->sidetoneHz(),
                 (unsigned)wk->keyerMode());
        body += line;
    }

    body += F("</pre><p><a href='/state'>/state</a> &middot; "
              "<a href='/log?n=20'>/log?n=20</a></p>"
              "</body></html>");
    _server.send(200, "text/html", body);
}

// ─── GET /state — JSON snapshot of every observable piece of state ──
void handleState() {
    const auto& m  = MorseModel::instance();
    const WinkeyBridge* wk = Winkey::bridge();

    // Decoded text: last min(textLen, 50) characters, oldest first.
    String decoded;
    decoded.reserve(64);
    const size_t tl = m.decodedTextLen();
    const size_t want = 50;
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
    body.reserve(1024);
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

    if (wk) {
        body += ",\"winkeyOpen\":";       body += wk->isOpen() ? "true" : "false";
        body += ",\"winkeyWpm\":";        body += wk->wpm();
        body += ",\"winkeySidetoneHz\":"; body += wk->sidetoneHz();
        body += ",\"winkeyKeyerMode\":";  body += (unsigned)wk->keyerMode();
    }

    body += ",\"decoded\":\"";       jsonEscape(body, decoded.c_str()); body += '"';
    body += '}';

    _server.send(200, "application/json", body);
}

// ─── GET /log?n=N — JSON array of recent log lines ────────────────────
void handleLog() {
    if (!_server.hasArg("n")) {
        _server.send(400, "application/json", "{\"error\":\"missing n\"}");
        return;
    }
    long n = _server.arg("n").toInt();
    if (n < 1)   n = 1;
    if (n > 200) n = 200;

    std::string json = LogRing::instance().snapshotLines((size_t)n);
    _server.send(200, "application/json", json.c_str());
}

}  // namespace

void ConsoleServer::begin(uint16_t port) {
    // The dev HTTP console is bound to port 80 at _server construction
    // above (the parameter is currently unused). WebServer is not
    // copy-assignable because it owns unique_ptr<HTTPUpload> etc., so
    // we can't rebind to a different port here. If we ever need a
    // configurable port, switch _server to a std::unique_ptr<WebServer>
    // and re-allocate in begin(). For now, port 80 is fine.
    (void)port;
    _server.on("/",            HTTP_GET, handleRoot);
    _server.on("/state",       HTTP_GET, handleState);
    _server.on("/log",         HTTP_GET, handleLog);
    _server.begin();
}

void ConsoleServer::poll() {
    _server.handleClient();
}

#else  // ENABLE_WIFI_DEBUG == 0  OR  UNIT_TEST

void ConsoleServer::begin(uint16_t) {}
void ConsoleServer::poll() {}

#endif