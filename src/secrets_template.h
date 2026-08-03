#pragma once
/**
 * secrets_template.h — template for git-ignored src/secrets.h
 *
 * Copy this file to src/secrets.h and fill in your local values:
 *   cp src/secrets_template.h src/secrets.h
 *   $EDITOR src/secrets.h
 *
 * src/secrets.h is in .gitignore and will NOT be checked in.
 *
 * These macros are only consumed by src/wifi_debug.cpp when
 * ENABLE_WIFI_DEBUG=1 is set in platformio.ini. With the default
 * ENABLE_WIFI_DEBUG=0 the whole network subsystem is compiled out,
 * so missing macros are harmless.
 *
 * IP layout:
 *   - WIFI_STATIC_IP — your chosen device IP (must be reserved on the
 *     DHCP server so it does not conflict). Pick anything in the
 *     subnet that is not in the DHCP pool.
 *   - WIFI_GATEWAY   — your router's IP (usually .1 or .254)
 *   - WIFI_SUBNET    — typically 255.255.255.0 / 24
 *   - WIFI_DNS       — typically your gateway, or a public DNS
 *
 * Future iterations will source these from NVS (Preferences) instead,
 * with the device keyboard or a web UI entering them. For now, this
 * hardcoded form is intentional and dev-only.
 */

#define WIFI_SSID       "your-ssid-here"
#define WIFI_PASS       "your-password-here"

// IPAddress() takes 4 bytes. Use commas, NOT dots.
#define WIFI_STATIC_IP  192, 168, 1, 123
#define WIFI_GATEWAY    192, 168, 1, 1
#define WIFI_SUBNET     255, 255, 255, 0
#define WIFI_DNS        192, 168, 1, 1