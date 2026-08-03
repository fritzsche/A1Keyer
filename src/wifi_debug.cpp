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

// One-shot boot scan. Off by default because it adds ~1–3 s to boot.
// Flip to 1 when debugging "can't find my AP" — see scanAndLog() below
// for the output format and what to look for.
#ifndef WIFI_DEBUG_SCAN_AT_BOOT
#define WIFI_DEBUG_SCAN_AT_BOOT 0
#endif

namespace {

bool _wasConnected = false;

// authName — short string for the scan dump.
const char* authName(wifi_auth_mode_t a) {
    switch (a) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
        default:                        return "?";
    }
}

}  // namespace

void WifiDebug::scanAndLog() {
    // One-shot pre-association scan. Blocking (~1–3 s). Output goes
    // through Log::* so it lands in both the USB monitor and the
    // LogRing HTTP tail — useful when the SSID isn't visible and the
    // user is debugging remotely.
    //
    // The target SSID (from src/secrets.h) is prefixed with '*' so it
    // can be picked out at a glance. If you don't see '*' next to your
    // AP, the device can't see it: wrong band (5 GHz), hidden SSID,
    // wrong region, or out of range.
    Log::info("[WiFi] scanning…");
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
    if (n <= 0) {
        Log::warning("[WiFi] scan found 0 networks");
    } else {
        Log::info("[WiFi] scan found %d network%s:", n, n == 1 ? "" : "s");
        for (int i = 0; i < n; ++i) {
            const bool isTarget = (WiFi.SSID(i) == WIFI_SSID);
            Log::info("  %s [%2d] %-24s ch=%2d rssi=%4d %s",
                      isTarget ? "*" : " ",
                      i,
                      WiFi.SSID(i).c_str(),
                      WiFi.channel(i),
                      WiFi.RSSI(i),
                      authName(WiFi.encryptionType(i)));
        }
    }
    WiFi.scanDelete();
}

void WifiDebug::begin() {
    WiFi.mode(WIFI_STA);

#if WIFI_DEBUG_SCAN_AT_BOOT
    scanAndLog();
#endif

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
void     WifiDebug::scanAndLog()  {}
bool     WifiDebug::isConnected() { return false; }
uint32_t WifiDebug::localIP()     { return 0; }

#endif