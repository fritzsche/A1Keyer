/**
 * wifi_debug.cpp — dev-only WiFi STA lifecycle.
 *
 * See wifi_debug.h. Body is `#if ENABLE_WIFI_DEBUG` — when the flag is
 * off (default in platformio.ini) the build skips every line below so
 * shipping firmware has no WiFi code, no stack cost, and no dependency
 * on src/secrets.h. Host unit tests (UNIT_TEST) also skip the body —
 * WiFi.h is not available there.
 *
 * Credentials live in src/secrets.h which is git-ignored; the include
 * is here, NOT in platformio.ini's `-include`, so missing secrets.h is
 * a build error ONLY when ENABLE_WIFI_DEBUG=1 (and only on this TU).
 */
#include "wifi_debug.h"

#if ENABLE_WIFI_DEBUG && !defined(UNIT_TEST)

#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"
#include "Log.h"

namespace {

bool _wasConnected = false;

}  // namespace

void WifiDebug::begin() {
    WiFi.mode(WIFI_STA);

    // Order matters: WiFi.config() must be called BEFORE WiFi.begin()
    // or the static IP is ignored (espressif/arduino-esp32 docs).
    IPAddress ip(WIFI_STATIC_IP);
    WiFi.config(
        ip,
        IPAddress(WIFI_GATEWAY),
        IPAddress(WIFI_SUBNET),
        IPAddress(WIFI_DNS));

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    _wasConnected = false;
    Log::info("[WiFi] connecting to '%s' as %s ...",
              WIFI_SSID, ip.toString().c_str());
}

void WifiDebug::poll() {
    const bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected && !_wasConnected) {
        _wasConnected = true;
        Log::info("[WiFi] connected: %s", WiFi.localIP().toString().c_str());
    } else if (!connected && _wasConnected) {
        _wasConnected = false;
        Log::warning("[WiFi] disconnected, will retry");
    } else if (!connected) {
        // Still associating. WiFi.begin() retries on its own; nothing to do.
    }
}

bool WifiDebug::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

uint32_t WifiDebug::localIP() {
    return WiFi.localIP();
}

#else  // ENABLE_WIFI_DEBUG == 0  OR  UNIT_TEST

// Stubs so callers under `#if ENABLE_WIFI_DEBUG` always link.
void     WifiDebug::begin()       {}
void     WifiDebug::poll()        {}
bool     WifiDebug::isConnected() { return false; }
uint32_t WifiDebug::localIP()     { return 0; }

#endif