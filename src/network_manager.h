#pragma once
/**
 * network_manager.h — Wi-Fi station connectivity state machine.
 *
 * Owns everything about being on a network: scanning, associating,
 * reporting why an attempt failed, reconnecting with backoff, and
 * forgetting credentials. See docs/network.md for the design and for the
 * reasoning behind the two decisions that most shape this file:
 *
 *  1. Transitions are driven by Wi-Fi EVENTS, not by polling
 *     WiFi.status(). Only the disconnect event carries a `reason` code,
 *     and that code is the sole way to distinguish a wrong password from
 *     a missing network from a faded signal. Polling collapses all three
 *     into one useless "disconnected".
 *
 *  2. The Wi-Fi driver's own NVS copy of the credentials is disabled with
 *     esp_wifi_set_storage(WIFI_STORAGE_RAM). Without it, "forget
 *     network" does not actually forget. (WiFi.persistent(false), the
 *     usual advice, does not do this on Arduino-ESP32 3.x — it only
 *     suppresses persisting the mode.)
 *
 * Threading: event callbacks arrive on the Wi-Fi task and do nothing but
 * record a reason code and set a flag. Every state transition, and every
 * outgoing Wi-Fi call, happens in poll() on the loop core. The
 * Arduino-ESP32 Wi-Fi API is not thread-safe, so this separation is a
 * requirement, not a style choice.
 *
 * Real-time: nothing here blocks. The scan is asynchronous and
 * association is event-driven, so the keyer stays responsive throughout.
 *
 * Hardware access goes through NetHal, a table of function pointers. On
 * device it is bound to the Arduino Wi-Fi API; in tests it is replaced
 * wholesale, which is what lets the entire machine — including every
 * error path and the backoff schedule — run on the host.
 */
#include <cstddef>
#include <cstdint>

#include "net_config.h"

/// Why this lives in `namespace WifiMgr` rather than `NetworkManager`:
/// Arduino-ESP32's <WiFi.h> pulls in NetworkManager.h which declares a
/// `class NetworkManager` in the global namespace. A `namespace
/// NetworkManager` would collide with that class. `WifiMgr` is short,
/// descriptive, and unambiguous.

enum class NetState : uint8_t {
    IDLE,            ///< radio idle, nothing requested
    SCANNING,        ///< asynchronous scan in flight
    SCAN_DONE,       ///< results available, awaiting a choice
    SCAN_FAILED,     ///< scan produced an error or timed out
    CONNECTING,      ///< association in flight
    CONNECTED,       ///< associated and addressed
    CONNECT_FAILED,  ///< association failed; see lastErrorMessage()
    DISCONNECTED,    ///< was connected, link dropped, retry pending
};

/// Where the active credentials came from, for the status line.
enum class NetCredSource : uint8_t {
    NONE,
    NVS,        ///< configured on the device
};

/// One access point from a scan.
struct NetScanEntry {
    char    ssid[kSsidBufLen] = {0};
    int8_t  rssi              = 0;
    uint8_t channel           = 0;
    bool    open              = false;   ///< no encryption
};

/**
 * Platform access. Every call the manager makes to the radio goes
 * through here so the machine can be driven from a test.
 *
 * All functions are non-blocking.
 */
struct NetHal {
    uint32_t (*millisFn)()                     = nullptr;
    /// Put the radio in station mode and disable driver-side credential
    /// persistence. Called once from begin().
    void     (*initSta)()                      = nullptr;
    /// Start an asynchronous scan. Returns false if one could not start.
    bool     (*startScan)()                    = nullptr;
    /// Number of results, or a negative value while still running.
    int      (*scanComplete)()                 = nullptr;
    /// Fill one result. Returns false if the index is invalid.
    bool     (*scanEntry)(int i, NetScanEntry& out) = nullptr;
    /// Release scan result memory.
    void     (*scanDelete)()                   = nullptr;
    /// Begin association. Returns false if the call itself failed.
    bool     (*beginSta)(const char* ssid, const char* pass) = nullptr;
    /// Disassociate and stop retrying.
    void     (*disconnect)()                   = nullptr;
};

namespace WifiMgr {

/// Upper bound on retained scan results. More than this on a crowded
/// band is not useful on a four-row display, and the cap keeps the whole
/// manager free of dynamic allocation.
inline constexpr int kMaxScanResults = 24;

/// Rows of the scan list shown at once on the 240x135 display.
/// Reduced from 6 → 4 to allow the larger 20 px font used by
/// CardputerDisplay::showWifiScanList. See docs/network.md §3.1.
inline constexpr int kPageSize = 4;

// ── lifecycle ───────────────────────────────────────────────────────────

/// Load stored credentials and prepare the radio. Does not associate.
/// Never blocks. Safe to call once from setup().
void begin();

/// Advance the state machine. Call every loop() iteration.
void poll();

// ── user actions ────────────────────────────────────────────────────────

/// Start an asynchronous scan. No-op while a scan is already running, so
/// repeated keypresses cannot stack scans.
void startScan();

/// Abandon whatever is in flight and return to IDLE, disassociating if
/// necessary so no orphaned attempt is left running.
void cancel();

/// Associate with a scan result, using `pass` (may be empty for an open
/// network). The credentials are persisted only once the association
/// succeeds, so a mistyped password does not become the stored one.
void connect(int scanIndex, const char* pass);

/// Associate using the stored credentials. No-op when none exist.
void connectWithSaved();

/// Disassociate and erase the stored credentials.
void disconnectAndForget();

/// Re-attempt the last connection immediately, ignoring any pending
/// backoff.
void retry();

// ── observation ─────────────────────────────────────────────────────────

NetState      state();
bool          isConnected();
uint32_t      localIP();          ///< 0 when not connected
const char*   connectedSSID();    ///< "" when not connected
NetCredSource credentialSource();
bool          hasSavedCredentials();

/// Short, user-facing explanation of the most recent failure. Never
/// null; empty when nothing has failed.
const char* lastErrorMessage();

/// millis() of the most recent state change, for UI animation and for
/// auto-dismiss timing.
uint32_t stateChangedAt();

int                 scanCount();
const NetScanEntry* scanEntry(int i);   ///< nullptr when out of range

/// Seconds until the next automatic retry, or 0 when none is pending.
uint32_t secondsUntilRetry();

// ── event sinks (called from the Wi-Fi task; do not block) ──────────────

void notifyScanDone();
void notifyGotIp(uint32_t ip);
void notifyDisconnected(uint8_t reason);

// ── test seam ───────────────────────────────────────────────────────────

/// Replace the platform layer and reset all internal state. Tests call
/// this instead of begin() doing real radio work.
void setHalForTest(const NetHal& hal);

/// Restore the manager to its just-constructed state without touching
/// the HAL. Used between test cases.
void resetForTest();

#ifndef UNIT_TEST
/// Bind the manager to the real Wi-Fi API. Must be called once, before
/// begin(). Defined only in firmware builds.
void bindPlatformHal();
#endif

}  // namespace NetworkManager

/// Map an 802.11 / ESP disconnect reason code to the message shown to the
/// user. Exposed for testing; see docs/network.md §7.
const char* netReasonToMessage(uint8_t reason);
