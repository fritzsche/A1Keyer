#include "network_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>

// ─── reason-code mapping ────────────────────────────────────────────────────
//
// Values are esp_wifi's wifi_err_reason_t. Only the ones a user can act on
// get a bespoke message; everything else falls through to a numbered
// message so a bug report still carries the code. See docs/network.md §7.

namespace {
constexpr uint8_t kReasonAuthExpire        = 2;
constexpr uint8_t kReasonAuthLeave         = 3;
constexpr uint8_t kReasonAssocLeave        = 8;
constexpr uint8_t kReason4WayTimeout       = 15;
constexpr uint8_t kReasonHandshakeTimeout  = 204;
constexpr uint8_t kReasonBeaconTimeout     = 200;
constexpr uint8_t kReasonNoApFound         = 201;
constexpr uint8_t kReasonAuthFail          = 202;
constexpr uint8_t kReasonAssocFail         = 203;

char s_reasonBuf[48];
}  // namespace

const char* netReasonToMessage(uint8_t reason) {
    switch (reason) {
        case kReasonAuthFail:
        case kReason4WayTimeout:
        case kReasonHandshakeTimeout:
            return "wrong password";

        case kReasonNoApFound:
            return "network not found (5GHz-only?)";

        case kReasonBeaconTimeout:
        case kReasonAssocLeave:
        case kReasonAuthExpire:
        case kReasonAuthLeave:
            return "signal lost, reconnecting";

        case kReasonAssocFail:
            return "access point refused connection";

        default:
            std::snprintf(s_reasonBuf, sizeof(s_reasonBuf),
                          "connect failed (reason %u)", (unsigned)reason);
            return s_reasonBuf;
    }
}

/// A wrong passphrase will never fix itself, so retrying is pointless and
/// merely hammers the access point. Every other failure is potentially
/// transient and is retried with backoff.
static bool reasonIsTerminal(uint8_t reason) {
    return reason == kReasonAuthFail ||
           reason == kReason4WayTimeout ||
           reason == kReasonHandshakeTimeout;
}

// ─── module state ───────────────────────────────────────────────────────────

namespace {

constexpr uint32_t kScanTimeoutMs    = 10000;
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kBackoffStartMs   = 1000;
constexpr uint32_t kBackoffMaxMs     = 300000;   // five minutes

NetHal   s_hal{};
NetState s_state       = NetState::IDLE;
uint32_t s_stateAt     = 0;

// Flags set by the Wi-Fi task, consumed by poll() on the loop core.
std::atomic<bool>     s_evScanDone{false};
std::atomic<bool>     s_evGotIp{false};
std::atomic<uint32_t> s_evIp{0};
std::atomic<bool>     s_evDisconnected{false};
std::atomic<uint8_t>  s_evReason{0};

NetScanEntry s_scan[WifiMgr::kMaxScanResults];
int          s_scanCount = 0;

char s_error[64] = {0};

// Credentials currently in use. Held separately from the stored config
// because a candidate is only persisted once it actually works.
char          s_ssid[kSsidBufLen] = {0};
char          s_pass[kPassBufLen] = {0};
NetCredSource s_source            = NetCredSource::NONE;
bool          s_pendingSave       = false;   ///< persist on success

// Compiled-in fallback (src/secrets.h development path).
char s_fbSsid[kSsidBufLen] = {0};
char s_fbPass[kPassBufLen] = {0};
bool s_hasFallback         = false;

char     s_connectedSsid[kSsidBufLen] = {0};
uint32_t s_ip           = 0;
uint32_t s_backoffMs    = kBackoffStartMs;
uint32_t s_retryAt      = 0;      ///< 0 = no retry scheduled

uint32_t now() {
    return s_hal.millisFn ? s_hal.millisFn() : 0;
}

void setError(const char* msg) {
    netCopyStr(s_error, sizeof(s_error), msg ? msg : "");
}

void setState(NetState st) {
    if (s_state == st) return;
    s_state   = st;
    s_stateAt = now();
}

void clearEvents() {
    s_evScanDone.store(false);
    s_evGotIp.store(false);
    s_evDisconnected.store(false);
}

/// Schedule the next automatic attempt and grow the backoff.
void scheduleRetry() {
    s_retryAt   = now() + s_backoffMs;
    if (s_backoffMs < kBackoffMaxMs) {
        s_backoffMs *= 2;
        if (s_backoffMs > kBackoffMaxMs) s_backoffMs = kBackoffMaxMs;
    }
}

void cancelRetry() {
    s_retryAt = 0;
}

/// Issue the association. Always called from poll() or from a user
/// action on the loop core — never from an event callback, which would
/// re-enter the Wi-Fi stack from its own task.
void beginAssociation() {
    clearEvents();
    if (!s_hal.beginSta || !s_hal.beginSta(s_ssid, s_pass)) {
        setError("could not start connection");
        setState(NetState::CONNECT_FAILED);
        return;
    }
    setError("");
    setState(NetState::CONNECTING);
}

void harvestScanResults(int n) {
    if (n > WifiMgr::kMaxScanResults) n = WifiMgr::kMaxScanResults;
    s_scanCount = 0;
    for (int i = 0; i < n; ++i) {
        NetScanEntry e;
        if (s_hal.scanEntry && s_hal.scanEntry(i, e)) {
            // An access point may advertise an empty SSID (hidden). It
            // cannot be selected from a list, so it is not shown.
            if (e.ssid[0] != '\0') s_scan[s_scanCount++] = e;
        }
    }
    if (s_hal.scanDelete) s_hal.scanDelete();
}

}  // namespace

// ─── lifecycle ──────────────────────────────────────────────────────────────

namespace WifiMgr {

void setFallbackCredentials(const char* ssid, const char* pass) {
    netCopyStr(s_fbSsid, sizeof(s_fbSsid), ssid);
    netCopyStr(s_fbPass, sizeof(s_fbPass), pass);
    s_hasFallback = s_fbSsid[0] != '\0';
}

void begin() {
    if (s_hal.initSta) s_hal.initSta();

    NetConfig cfg;
    netConfigLoad(cfg);

    if (cfg.hasCredentials()) {
        netCopyStr(s_ssid, sizeof(s_ssid), cfg.nets[0].ssid);
        netCopyStr(s_pass, sizeof(s_pass), cfg.nets[0].pass);
        s_source = NetCredSource::NVS;
    } else if (s_hasFallback) {
        // Development path: src/secrets.h is consulted only when nothing
        // has been configured on the device. Stored credentials always win.
        netCopyStr(s_ssid, sizeof(s_ssid), s_fbSsid);
        netCopyStr(s_pass, sizeof(s_pass), s_fbPass);
        s_source = NetCredSource::SECRETS_H;
    } else {
        s_ssid[0] = '\0';
        s_pass[0] = '\0';
        s_source  = NetCredSource::NONE;
    }

    setState(NetState::IDLE);
}

// ─── user actions ───────────────────────────────────────────────────────────

void startScan() {
    if (s_state == NetState::SCANNING) return;   // do not stack scans

    cancelRetry();
    clearEvents();
    s_scanCount = 0;

    if (!s_hal.startScan || !s_hal.startScan()) {
        setError("could not start scan");
        setState(NetState::SCAN_FAILED);
        return;
    }
    setError("");
    setState(NetState::SCANNING);
}

void cancel() {
    // Disassociating here is what stops a half-finished attempt from
    // completing after the user has already backed out of the screen.
    if (s_state == NetState::CONNECTING || s_state == NetState::SCANNING) {
        if (s_hal.disconnect) s_hal.disconnect();
    }
    if (s_hal.scanDelete) s_hal.scanDelete();
    cancelRetry();
    clearEvents();
    s_pendingSave = false;
    setError("");
    setState(NetState::IDLE);
}

void connect(int scanIndex, const char* pass) {
    if (scanIndex < 0 || scanIndex >= s_scanCount) {
        setError("no network selected");
        setState(NetState::CONNECT_FAILED);
        return;
    }
    netCopyStr(s_ssid, sizeof(s_ssid), s_scan[scanIndex].ssid);
    netCopyStr(s_pass, sizeof(s_pass), pass);

    // Persist only after the association succeeds, so a typo never
    // becomes the stored credential.
    s_pendingSave = true;
    s_backoffMs   = kBackoffStartMs;
    cancelRetry();
    beginAssociation();
}

void connectWithSaved() {
    if (s_ssid[0] == '\0') return;
    s_pendingSave = false;
    s_backoffMs   = kBackoffStartMs;
    cancelRetry();
    beginAssociation();
}

void disconnectAndForget() {
    if (s_hal.disconnect) s_hal.disconnect();
    netConfigClear();

    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    s_connectedSsid[0] = '\0';
    s_ip = 0;
    s_source = NetCredSource::NONE;
    s_pendingSave = false;
    s_backoffMs = kBackoffStartMs;

    // The compiled-in fallback is deliberately NOT reapplied here.
    // "Forget" must mean the device stays offline until told otherwise,
    // otherwise a development build would silently rejoin.
    s_hasFallback = false;

    cancelRetry();
    clearEvents();
    setError("");
    setState(NetState::IDLE);
}

void retry() {
    if (s_ssid[0] == '\0') return;
    s_backoffMs = kBackoffStartMs;
    cancelRetry();
    beginAssociation();
}

// ─── the state machine ──────────────────────────────────────────────────────

void poll() {
    const uint32_t t = now();

    switch (s_state) {

    case NetState::SCANNING: {
        const bool  evented = s_evScanDone.exchange(false);
        const int   n       = s_hal.scanComplete ? s_hal.scanComplete() : -1;

        if (evented || n >= 0) {
            if (n < 0) {
                // The event fired but results are not readable. Treat as
                // an empty scan rather than hanging in SCANNING.
                s_scanCount = 0;
                setState(NetState::SCAN_DONE);
                break;
            }
            harvestScanResults(n);
            setError(s_scanCount == 0 ? "no networks found" : "");
            setState(NetState::SCAN_DONE);
            break;
        }

        if (t - s_stateAt >= kScanTimeoutMs) {
            if (s_hal.scanDelete) s_hal.scanDelete();
            setError("scan timeout");
            setState(NetState::SCAN_FAILED);
        }
        break;
    }

    case NetState::CONNECTING: {
        if (s_evGotIp.exchange(false)) {
            s_ip = s_evIp.load();
            netCopyStr(s_connectedSsid, sizeof(s_connectedSsid), s_ssid);
            s_evDisconnected.store(false);

            if (s_pendingSave) {
                netConfigSaveSingle(s_ssid, s_pass);
                s_source      = NetCredSource::NVS;
                s_pendingSave = false;
            }
            s_backoffMs = kBackoffStartMs;   // a success resets the schedule
            cancelRetry();
            setError("");
            setState(NetState::CONNECTED);
            break;
        }

        if (s_evDisconnected.exchange(false)) {
            const uint8_t reason = s_evReason.load();
            setError(netReasonToMessage(reason));
            setState(NetState::CONNECT_FAILED);
            if (reasonIsTerminal(reason)) {
                // A wrong passphrase will not fix itself; stop here and
                // leave it to the user.
                s_pendingSave = false;
                cancelRetry();
            } else {
                scheduleRetry();
            }
            break;
        }

        if (t - s_stateAt >= kConnectTimeoutMs) {
            if (s_hal.disconnect) s_hal.disconnect();
            setError("connect timeout");
            setState(NetState::CONNECT_FAILED);
            scheduleRetry();
        }
        break;
    }

    case NetState::CONNECTED: {
        if (s_evDisconnected.exchange(false)) {
            const uint8_t reason = s_evReason.load();
            s_ip = 0;
            s_connectedSsid[0] = '\0';
            setError(netReasonToMessage(reason));
            setState(NetState::DISCONNECTED);
            scheduleRetry();
        }
        break;
    }

    case NetState::CONNECT_FAILED:
    case NetState::DISCONNECTED: {
        // Automatic retry, if one is due. Issuing WiFi.begin() from here
        // rather than from the disconnect callback is deliberate: the
        // callback runs on the Wi-Fi task and re-entering the stack from
        // it is a recursion hazard.
        if (s_retryAt != 0 && (int32_t)(t - s_retryAt) >= 0) {
            cancelRetry();
            beginAssociation();
        }
        break;
    }

    case NetState::IDLE:
    case NetState::SCAN_DONE:
    case NetState::SCAN_FAILED:
    default:
        break;
    }
}

// ─── event sinks ────────────────────────────────────────────────────────────
//
// These run on the Wi-Fi task. They record and return; all work happens
// in poll() on the loop core.

void notifyScanDone() {
    s_evScanDone.store(true);
}

void notifyGotIp(uint32_t ip) {
    s_evIp.store(ip);
    s_evGotIp.store(true);
}

void notifyDisconnected(uint8_t reason) {
    s_evReason.store(reason);
    s_evDisconnected.store(true);
}

// ─── observation ────────────────────────────────────────────────────────────

NetState      state()            { return s_state; }
bool          isConnected()      { return s_state == NetState::CONNECTED; }
uint32_t      localIP()          { return s_ip; }
const char*   connectedSSID()    { return s_connectedSsid; }
NetCredSource credentialSource() { return s_source; }
const char*   lastErrorMessage() { return s_error; }
uint32_t      stateChangedAt()   { return s_stateAt; }
int           scanCount()        { return s_scanCount; }

bool hasSavedCredentials() { return s_ssid[0] != '\0'; }

const NetScanEntry* scanEntry(int i) {
    if (i < 0 || i >= s_scanCount) return nullptr;
    return &s_scan[i];
}

uint32_t secondsUntilRetry() {
    if (s_retryAt == 0) return 0;
    const uint32_t t = now();
    if ((int32_t)(t - s_retryAt) >= 0) return 0;
    return (s_retryAt - t + 999) / 1000;
}

// ─── test seam ──────────────────────────────────────────────────────────────

void resetForTest() {
    s_state       = NetState::IDLE;
    s_stateAt     = 0;
    s_scanCount   = 0;
    s_error[0]    = '\0';
    s_ssid[0]     = '\0';
    s_pass[0]     = '\0';
    s_fbSsid[0]   = '\0';
    s_fbPass[0]   = '\0';
    s_hasFallback = false;
    s_source      = NetCredSource::NONE;
    s_pendingSave = false;
    s_connectedSsid[0] = '\0';
    s_ip          = 0;
    s_backoffMs   = kBackoffStartMs;
    s_retryAt     = 0;
    clearEvents();
    s_evIp.store(0);
    s_evReason.store(0);
}

void setHalForTest(const NetHal& hal) {
    s_hal = hal;
    resetForTest();
}

}  // namespace WifiMgr

// ─── device platform layer ──────────────────────────────────────────────────

#ifndef UNIT_TEST

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace {

void halInitSta() {
    WiFi.mode(WIFI_STA);

    // The single most important line in this file. By default esp_wifi
    // keeps its own copy of the SSID and passphrase in NVS
    // (WIFI_STORAGE_FLASH), invisible to us, and will happily reassociate
    // from it after the user has asked the device to forget the network.
    // WiFi.persistent(false) does NOT prevent this on Arduino-ESP32 3.x.
    // See docs/network.md §4.2.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    // We run our own backoff schedule so the UI can report it; the
    // built-in one retries forever on a fixed short interval.
    WiFi.setAutoReconnect(false);

    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
        WifiMgr::notifyScanDone();
    }, ARDUINO_EVENT_WIFI_SCAN_DONE);

    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
        WifiMgr::notifyGotIp((uint32_t)info.got_ip.ip_info.ip.addr);
    }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
        WifiMgr::notifyDisconnected(info.wifi_sta_disconnected.reason);
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

bool halStartScan() {
    // true = asynchronous. A synchronous scan blocks for one to three
    // seconds, which would stall the keyboard and the display.
    return WiFi.scanNetworks(/*async=*/true) != WIFI_SCAN_FAILED;
}

int halScanComplete() {
    return WiFi.scanComplete();
}

bool halScanEntry(int i, NetScanEntry& out) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) return false;
    netCopyStr(out.ssid, kSsidBufLen, ssid.c_str());
    out.rssi    = (int8_t)WiFi.RSSI(i);
    out.channel = (uint8_t)WiFi.channel(i);
    out.open    = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    return true;
}

void halScanDelete() {
    WiFi.scanDelete();
}

bool halBeginSta(const char* ssid, const char* pass) {
    return WiFi.begin(ssid, (pass && pass[0]) ? pass : nullptr) != WL_CONNECT_FAILED;
}

void halDisconnect() {
    WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/true);
}

uint32_t halMillis() {
    return millis();
}

}  // namespace

namespace WifiMgr {

void bindPlatformHal() {
    NetHal hal;
    hal.millisFn     = halMillis;
    hal.initSta      = halInitSta;
    hal.startScan    = halStartScan;
    hal.scanComplete = halScanComplete;
    hal.scanEntry    = halScanEntry;
    hal.scanDelete   = halScanDelete;
    hal.beginSta     = halBeginSta;
    hal.disconnect   = halDisconnect;
    setHalForTest(hal);   // same setter; resets state, does no radio work
}

}  // namespace WifiMgr

#endif  // !UNIT_TEST
