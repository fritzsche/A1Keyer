#include "network_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include "Log.h"

#ifndef UNIT_TEST
#include <Arduino.h>
#endif

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
constexpr uint32_t kApStartTimeoutMs = 10000;

NetHal   s_hal{};
NetState s_state       = NetState::IDLE;
uint32_t s_stateAt     = 0;
NetMode  s_mode        = NetMode::STATION;

// Flags set by the Wi-Fi task, consumed by poll() on the loop core.
std::atomic<bool>     s_evScanDone{false};
std::atomic<bool>     s_evGotIp{false};
std::atomic<uint32_t> s_evIp{0};
std::atomic<bool>     s_evDisconnected{false};
std::atomic<uint8_t>  s_evReason{0};

// AP-mode event flags. Atomic because they arrive on the Wi-Fi task.
std::atomic<bool>     s_evApStart{false};
std::atomic<bool>     s_evApStop{false};
std::atomic<bool>     s_evApJoined{false};
std::atomic<bool>     s_evApLeft{false};

NetScanEntry s_scan[WifiMgr::kMaxScanResults];
int          s_scanCount = 0;

char s_error[64] = {0};

// Credentials currently in use. Held separately from the stored config
// because a candidate is only persisted once it actually works.
char          s_ssid[kSsidBufLen] = {0};
char          s_pass[kPassBufLen] = {0};
NetCredSource s_source            = NetCredSource::NONE;
bool          s_pendingSave       = false;   ///< persist on success

char     s_connectedSsid[kSsidBufLen] = {0};
uint32_t s_ip           = 0;
uint32_t s_backoffMs    = kBackoffStartMs;
uint32_t s_retryAt      = 0;      ///< 0 = no retry scheduled

// AP-mode mirror state. The driver is the source of truth at boot
// (poll() queries `apStations()` once), but the joined/left events
// keep it in sync without polling thereafter.
uint8_t s_apClients = 0;

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

void clearApEvents() {
    s_evApStart.store(false);
    s_evApStop.store(false);
    s_evApJoined.store(false);
    s_evApLeft.store(false);
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

void begin() {
    NetConfig cfg;
    netConfigLoad(cfg);
    s_mode = cfg.mode;

    if (s_mode == NetMode::ACCESS_POINT) {
        // AP path: configure the soft-AP interface with the locked
        // 192.168.73.0/24 block and load the stored passphrase. We do
        // NOT call esp_wifi_set_storage(WIFI_STORAGE_RAM) here — that
        // flag is meaningful only in STA mode and would interact
        // oddly with AP state.
        if (s_hal.initAp) {
            s_hal.initAp(WifiMgr::kApIpAddr, WifiMgr::kApGwAddr, WifiMgr::kApNetmask);
        }
        netCopyStr(s_pass, sizeof(s_pass), cfg.apPass);
        s_ssid[0]       = '\0';   // unused in AP mode
        s_source        = NetCredSource::NVS;
        setState(NetState::IDLE);
        return;
    }

    // STA path: existing behaviour. initSta installs the
    // WIFI_STORAGE_RAM guard that makes "forget network" actually
    // forget (see docs/network.md §4.2).
    if (s_hal.initSta) s_hal.initSta();

    if (cfg.hasCredentials()) {
        netCopyStr(s_ssid, sizeof(s_ssid), cfg.nets[0].ssid);
        netCopyStr(s_pass, sizeof(s_pass), cfg.nets[0].pass);
        s_source = NetCredSource::NVS;
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

    // Persist IMMEDIATELY when the user commits the password — not on
    // GOT_IP. The original "save only after success" design was meant
    // to keep typos out of NVS, but in practice it stranded the user:
    // if the first association failed (wrong password, weak signal, an
    // AP that simply never responded), the device silently bailed into
    // CONNECT_FAILED with the typed passphrase only held in RAM, and
    // the next power cycle wiped it. The user had no way to retry
    // without retyping the entire passphrase. Saving here means the
    // typed credentials survive any number of failed attempts, and
    // "forget" (the X key on the network-info screen) remains the
    // single, explicit way to remove them.
    if (!netConfigSaveSingle(s_ssid, s_pass)) {
        Log::warning("[NET] connect: save NVS failed; creds will not "
                     "survive a power cycle");
    }
    s_source      = NetCredSource::NVS;
    s_pendingSave = false;

    s_backoffMs = kBackoffStartMs;
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
    if (s_mode == NetMode::ACCESS_POINT) {
        // Forgetting means dropping the link in AP mode. STA creds
        // are untouched (they were never present on this path), and
        // AP mode + AP password are preserved by design — see the
        // discardAndStopAp variant below.
        if (s_hal.stopAp) s_hal.stopAp();
        s_apClients = 0;
        clearApEvents();
        setError("");
        setState(NetState::IDLE);
        return;
    }

    if (s_hal.disconnect) s_hal.disconnect();
    netConfigClear();

    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    s_connectedSsid[0] = '\0';
    s_ip = 0;
    s_source = NetCredSource::NONE;
    s_pendingSave = false;
    s_backoffMs = kBackoffStartMs;

    cancelRetry();
    clearEvents();
    setError("");
    setState(NetState::IDLE);
}

void disconnectCurrent() {
    // Drops whatever link is up. NVS is NEVER touched here — that is
    // the whole point of this entry point vs. disconnectAndForget().
    switch (s_state) {
    case NetState::CONNECTING:
    case NetState::CONNECTED:
    case NetState::CONNECT_FAILED:
    case NetState::DISCONNECTED:
        if (s_hal.disconnect) s_hal.disconnect();
        s_ip = 0;
        s_connectedSsid[0] = '\0';
        cancelRetry();
        clearEvents();
        setError("");
        setState(NetState::IDLE);
        break;
    case NetState::AP_STARTING:
    case NetState::AP_UP:
        if (s_hal.stopAp) s_hal.stopAp();
        s_apClients = 0;
        clearApEvents();
        setError("");
        setState(NetState::IDLE);
        break;
    case NetState::SCANNING:
        if (s_hal.disconnect) s_hal.disconnect();
        if (s_hal.scanDelete) s_hal.scanDelete();
        clearEvents();
        setError("");
        setState(NetState::IDLE);
        break;
    case NetState::SCAN_DONE:
    case NetState::SCAN_FAILED:
    case NetState::AP_FAILED:
    case NetState::IDLE:
        // Nothing to drop.
        setError("");
        setState(NetState::IDLE);
        break;
    }
}

void retry() {
    if (s_ssid[0] == '\0') return;
    s_backoffMs = kBackoffStartMs;
    cancelRetry();
    beginAssociation();
}

void startAp(const char* pass) {
    // Idempotent — silently no-op when an AP is already up.
    if (s_state == NetState::AP_UP || s_state == NetState::AP_STARTING) {
        return;
    }

    // Persist BEFORE bringing the interface up. Same rationale as the
    // STA-mode connect(): a power-cycle between the user committing
    // the password and the AP actually starting would otherwise wipe
    // the just-typed passphrase.
    netCopyStr(s_pass, sizeof(s_pass), pass);
    if (!netConfigSaveAp(s_pass)) {
        Log::warning("[NET] startAp: save NVS failed; passphrase will "
                     "not survive a power cycle");
    }

    clearApEvents();
    s_apClients = 0;

    if (!s_hal.startAp || !s_hal.startAp(kApSsid, s_pass)) {
        setError("could not start AP");
        setState(NetState::AP_FAILED);
        return;
    }

    s_mode  = NetMode::ACCESS_POINT;
    s_source = NetCredSource::NVS;

    // Trust the softAP return value. By the time it returns true the
    // AP is configured and broadcasting — the laptop can connect and
    // pull DHCP immediately. We deliberately skip AP_STARTING and go
    // straight to AP_UP rather than waiting on ARDUINO_EVENT_WIFI_AP_START,
    // because that event's dispatch goes through APClass::_onApEvent
    // which posts via Network — and after our WiFi.mode(WIFI_OFF)
    // → WiFi.mode(WIFI_AP) cycle the registration chain is unreliable
    // enough on Arduino-ESP32 3.3.11 that the event frequently never
    // reaches our sink. Station join/leave still flow through the
    // same dispatch and DO work; we just don't gate the state on the
    // start event itself.
    if (s_hal.apStations) s_apClients = s_hal.apStations();

    setError("");
    setState(NetState::AP_UP);
}

void stopAp() {
    if (s_state != NetState::AP_STARTING &&
        s_state != NetState::AP_UP) {
        return;
    }
    if (s_hal.stopAp) s_hal.stopAp();
    s_apClients = 0;
    clearApEvents();
    setError("");
    setState(NetState::IDLE);
}

// ─── the state machine ──────────────────────────────────────────────────────

void poll() {
    const uint32_t t = now();

    // GOT_IP must be checked BEFORE the state-specific branches. If
    // WiFi.begin() is called while the previous session is still up,
    // Arduino-ESP32 tears it down synchronously and fires a
    // DISCONNECTED event (reason 8, ASSOC_LEAVE) before the new
    // association completes. Our CONNECTING branch then transitions
    // to CONNECT_FAILED on that spurious disconnect, and the
    // subsequent GOT_IP — which lives only inside the CONNECTING
    // branch — is silently dropped, leaving the device stuck in
    // CONNECT_FAILED with a perfectly good IP it never advertises.
    //
    // Handling GOT_IP first lets a real DHCP success recover from the
    // race: the IP is committed, state goes to CONNECTED, and the
    // queued DISCONNECTED event is consumed by the CONNECTED branch's
    // normal "link dropped" handling.
    //
    // Guard on the connection-state subspace so a late IP arriving
    // after cancel() (which leaves the manager in IDLE) does not
    // silently re-attach.
    if (s_evGotIp.exchange(false) &&
        (s_state == NetState::CONNECTING ||
         s_state == NetState::CONNECT_FAILED ||
         s_state == NetState::DISCONNECTED)) {
        s_ip = s_evIp.load();
        netCopyStr(s_connectedSsid, sizeof(s_connectedSsid), s_ssid);
        // Discard any pending DISCONNECTED that arrived in the same
        // window as this IP. It was the spurious ASSOC_LEAVE from the
        // previous WiFi.begin() tearing down the old session; the new
        // session is alive and addressed. Without this clear the
        // CONNECTED branch would immediately drop us back to
        // DISCONNECTED on the very next poll tick.
        s_evDisconnected.store(false);

        // s_pendingSave is set to false in connect() once NVS has been
        // written, so by the time we reach GOT_IP the credentials are
        // already durable. Nothing to do here.
        s_backoffMs = kBackoffStartMs;
        cancelRetry();
        setError("");
        setState(NetState::CONNECTED);
        return;
    }

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
        // GOT_IP is handled at the top of poll() so a successful DHCP
        // reply can recover from the ASSOC_LEAVE race that happens when
        // WiFi.begin() tears down the previous session synchronously.
        // The CONNECTING branch only needs to watch for DISCONNECTED
        // and the 15 s timeout.

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
    case NetState::AP_FAILED:
    default:
        break;
    }

    // AP state machine. Kept OUT of the switch above on purpose:
    // the AP branches are short and event-driven, and splitting them
    // out keeps the original STA diagram legible.
    switch (s_state) {
    case NetState::AP_STARTING: {
        if (s_evApStart.exchange(false)) {
            // Sync the count from the driver the first time we land in
            // AP_UP, in case stations were already associated before
            // the event fired (rare, but happens on a fast retry).
            if (s_hal.apStations) s_apClients = s_hal.apStations();
            setError("");
            setState(NetState::AP_UP);
            break;
        }
        if (t - s_stateAt >= kApStartTimeoutMs) {
            if (s_hal.stopAp) s_hal.stopAp();
            s_apClients = 0;
            clearApEvents();
            setError("ap timeout");
            setState(NetState::AP_FAILED);
        }
        break;
    }

    case NetState::AP_UP: {
        // Drain station join/leave events. Both can stack between
        // polls, so we loop until the flag clears.
        bool any = false;
        if (s_evApJoined.exchange(false)) {
            if (s_apClients < kApMaxStations) s_apClients++;
            any = true;
        }
        if (s_evApLeft.exchange(false)) {
            if (s_apClients > 0) s_apClients--;
            any = true;
        }
        // Re-sync with the driver every 2 s as a safety net. The
        // AP_STACONNECTED / AP_STADISCONNECTED events go through the
        // same APClass::_onApEvent → Network.postEvent dispatch as the
        // AP_START event, and after the WiFi.mode(WIFI_OFF) cycle a
        // missed join/leave is plausible. esp_wifi_ap_get_sta_list is
        // cheap; the trade is worth a guaranteed-correct count.
        static uint32_t s_lastApSync = 0;
        if (s_hal.apStations && (any || t - s_lastApSync >= 2000)) {
            const uint8_t live = s_hal.apStations();
            if (live != s_apClients) {
                if (live <= kApMaxStations) s_apClients = live;
                else                       s_apClients = kApMaxStations;
            }
            s_lastApSync = t;
        }

        if (s_evApStop.exchange(false)) {
            s_apClients = 0;
            setError("");
            setState(NetState::IDLE);
        }
        break;
    }

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

void notifyApStart()        { s_evApStart.store(true); }
void notifyApStop()         { s_evApStop.store(true); }
void notifyApStationJoined(){ s_evApJoined.store(true); }
void notifyApStationLeft()  { s_evApLeft.store(true); }

// ─── observation ────────────────────────────────────────────────────────────

NetState      state()            { return s_state; }
bool          isConnected()      { return s_state == NetState::CONNECTED ||
                                        s_state == NetState::AP_UP; }
uint32_t      localIP()          {
    // In STA mode this is the DHCP lease (set by the GOT_IP event).
    // In AP mode the device has no DHCP lease of its own — it IS the
    // DHCP server — so we report the locked AP-side address instead.
    // Without this the web UI shows "Offline" even though the device
    // is fully reachable on 192.168.73.1.
    if (s_mode == NetMode::ACCESS_POINT) return kApIpAddr;
    return s_ip;
}
const char*   connectedSSID()    { return s_connectedSsid; }
NetCredSource credentialSource() { return s_source; }
NetMode       mode()             { return s_mode; }
const char*   apSsid()           { return kApSsid; }
uint8_t       apStations()       { return s_apClients; }
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
    s_mode        = NetMode::STATION;
    s_scanCount   = 0;
    s_error[0]    = '\0';
    s_ssid[0]     = '\0';
    s_pass[0]     = '\0';
    s_source      = NetCredSource::NONE;
    s_pendingSave = false;
    s_connectedSsid[0] = '\0';
    s_ip          = 0;
    s_backoffMs   = kBackoffStartMs;
    s_retryAt     = 0;
    s_apClients   = 0;
    clearEvents();
    clearApEvents();
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
        // esp_netif stores ip_info.ip.addr in network byte order on big-
        // endian systems but — empirically on this little-endian ESP32-S3
        // — the value comes through as the host-byte-order layout of the
        // same four octets. The project's localIP() contract is "value
        // such that >>24 yields the first dotted-decimal octet" (see the
        // host-side test_notify_got_ip assertion), which is big-endian /
        // network byte order. htonl() converts from host to that layout.
        WifiMgr::notifyGotIp(htonl((uint32_t)info.got_ip.ip_info.ip.addr));
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

void halInitAp(uint32_t ip, uint32_t gw, uint32_t mask) {
    WiFi.mode(WIFI_AP);

    // Apply the locked 192.168.73.0/24 layout. softAPConfig must be
    // called BEFORE softAP, otherwise the default 192.168.4.0/24 sticks.
    if (ip) {
        WiFi.softAPConfig(
            IPAddress((uint8_t)(ip >> 24), (uint8_t)(ip >> 16),
                      (uint8_t)(ip >>  8), (uint8_t)(ip)),
            IPAddress((uint8_t)(gw >> 24), (uint8_t)(gw >> 16),
                      (uint8_t)(gw >>  8), (uint8_t)(gw)),
            IPAddress((uint8_t)(mask >> 24), (uint8_t)(mask >> 16),
                      (uint8_t)(mask >>  8), (uint8_t)(mask)));
    }
}

bool halStartAp(const char* ssid, const char* pass) {
    // The first softAP() call from a device that booted in STA mode
    // fails with no useful error if we only call WiFi.mode(WIFI_AP):
    // esp_wifi_set_mode() returns ESP_ERR_WIFI_STATE while the STA
    // interface is still tearing down (WiFi.disconnect() does not
    // block for the disconnect to complete). On Arduino-ESP32 3.3.11
    // the only reliable path is a full radio cycle: WIFI_OFF powers
    // the driver down synchronously, WIFI_AP re-inits cleanly into
    // AP-only mode.
    if (WiFi.getMode() != WIFI_AP) {
        WiFi.mode(WIFI_OFF);
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(
            IPAddress((uint8_t)(WifiMgr::kApIpAddr >> 24),
                      (uint8_t)(WifiMgr::kApIpAddr >> 16),
                      (uint8_t)(WifiMgr::kApIpAddr >>  8),
                      (uint8_t)(WifiMgr::kApIpAddr)),
            IPAddress((uint8_t)(WifiMgr::kApGwAddr >> 24),
                      (uint8_t)(WifiMgr::kApGwAddr >> 16),
                      (uint8_t)(WifiMgr::kApGwAddr >>  8),
                      (uint8_t)(WifiMgr::kApGwAddr)),
            IPAddress((uint8_t)(WifiMgr::kApNetmask >> 24),
                      (uint8_t)(WifiMgr::kApNetmask >> 16),
                      (uint8_t)(WifiMgr::kApNetmask >>  8),
                      (uint8_t)(WifiMgr::kApNetmask)));
    }

    // Channel 1, hidden=0, max=kApMaxStations. Empty passphrase
    // opens the AP (softAP accepts nullptr for that case).
    return WiFi.softAP(ssid,
                       (pass && pass[0]) ? pass : nullptr,
                       /*channel=*/1,
                       /*ssid_hidden=*/0,
                       /*max_connection=*/WifiMgr::kApMaxStations) != 0;
}

void halStopAp() {
    // wifioff=false — keep the radio subsystem alive so a subsequent
    // startAp() does not have to re-initialise the Wi-Fi driver.
    WiFi.softAPdisconnect(/*wifioff=*/false);
}

uint8_t halApStations() {
    return WiFi.softAPgetStationNum();
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
    hal.initAp       = halInitAp;
    hal.startAp      = halStartAp;
    hal.stopAp       = halStopAp;
    hal.apStations   = halApStations;
    setHalForTest(hal);   // same setter; resets state, does no radio work

    // Register AP event handlers. STA handlers were already installed
    // by halInitSta() on the first call from begin(), but begin()
    // might not have run yet at this point — and in any case, the
    // handlers are idempotent (they just record flags).
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
        WifiMgr::notifyApStart();
    }, ARDUINO_EVENT_WIFI_AP_START);
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
        WifiMgr::notifyApStop();
    }, ARDUINO_EVENT_WIFI_AP_STOP);
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
        WifiMgr::notifyApStationJoined();
    }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
        WifiMgr::notifyApStationLeft();
    }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
}

}  // namespace WifiMgr

#endif  // !UNIT_TEST
