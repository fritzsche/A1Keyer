#pragma once
/**
 * wifi_debug.h — dev-only WiFi STA lifecycle.
 *
 * Brings up the ESP32-S3 WiFi radio as a station, joins the AP whose
 * credentials live in src/secrets.h (git-ignored), and reconnects if
 * the link drops. The actual IP / DNS / credentials live in secrets.h
 * so they never reach the repo.
 *
 * This module is gated by ENABLE_WIFI_DEBUG. When the flag is 0 the
 * .cpp body is `#ifdef UNIT_TEST` or `#if ENABLE_WIFI_DEBUG` away, so
 * shipping releases are unaffected. Default in platformio.ini is 0;
 * flip to 1 for development builds.
 *
 * Future iteration: change begin() to accept credentials as parameters
 * (sourced from NVS) and skip WiFi.config() when static IP is disabled
 * (DHCP). The current shape already isolates the configuration source
 * from the WiFi subsystem so that swap is mechanical.
 */

#include <cstdint>

#ifdef __ESP32__
#include <WiFi.h>     // IPAddress
#endif

class WifiDebug {
public:
    /// Configure STA mode with static IP and start association. Pulls
    /// SSID/pass/IP/gateway/subnet/DNS from src/secrets.h. Non-blocking
    /// — the link comes up over the next few seconds; poll() drives it.
    /// No-op when ENABLE_WIFI_DEBUG=0.
    static void begin();

    /// Service WiFi reconnect + log transitions. Call from loop().
    /// No-op when ENABLE_WIFI_DEBUG=0.
    static void poll();

    /// True once WiFi.status() == WL_CONNECTED.
    static bool isConnected();

    /// Current local IPv4 (0.0.0.0 when not connected).
    static uint32_t localIP();
};