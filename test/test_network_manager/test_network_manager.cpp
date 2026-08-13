#include "test_framework.h"

#include "Preferences.h"   // mock, for the netConfig* calls the manager makes
#include "network_manager.h"

#include <string>
#include <vector>

// ─── fake platform ──────────────────────────────────────────────────────────
//
// A controllable clock and a scriptable radio. Everything the manager can
// observe or do goes through here, so each test drives the machine
// deterministically without any timing races.

namespace fake {

uint32_t clockMs = 0;

int  startScanCalls  = 0;
int  beginStaCalls   = 0;
int  disconnectCalls = 0;
int  scanDeleteCalls = 0;
int  initStaCalls    = 0;

bool startScanSucceeds = true;
bool beginStaSucceeds  = true;

/// Result count reported by scanComplete(); negative means "still running".
int scanResult = -1;

std::vector<NetScanEntry> scanTable;

/// Every (ssid, pass) the manager tried to associate with, in order.
std::vector<std::pair<std::string, std::string>> attempts;

void reset() {
    clockMs = 1000;
    startScanCalls = beginStaCalls = disconnectCalls = 0;
    scanDeleteCalls = initStaCalls = 0;
    startScanSucceeds = beginStaSucceeds = true;
    scanResult = -1;
    scanTable.clear();
    attempts.clear();
}

uint32_t millisFn() { return clockMs; }

void initSta() { ++initStaCalls; }

bool startScan() {
    ++startScanCalls;
    scanResult = -1;   // in flight
    return startScanSucceeds;
}

int scanComplete() { return scanResult; }

bool scanEntry(int i, NetScanEntry& out) {
    if (i < 0 || i >= (int)scanTable.size()) return false;
    out = scanTable[i];
    return true;
}

void scanDelete() { ++scanDeleteCalls; }

bool beginSta(const char* ssid, const char* pass) {
    ++beginStaCalls;
    attempts.emplace_back(ssid ? ssid : "", pass ? pass : "");
    return beginStaSucceeds;
}

void disconnect() { ++disconnectCalls; }

NetHal hal() {
    NetHal h;
    h.millisFn     = millisFn;
    h.initSta      = initSta;
    h.startScan    = startScan;
    h.scanComplete = scanComplete;
    h.scanEntry    = scanEntry;
    h.scanDelete   = scanDelete;
    h.beginSta     = beginSta;
    h.disconnect   = disconnect;
    return h;
}

/// Publish a scan table and report it as complete, the way the driver
/// would once the radio finishes sweeping.
void finishScanWith(std::vector<NetScanEntry> entries) {
    scanTable  = std::move(entries);
    scanResult = (int)scanTable.size();
    WifiMgr::notifyScanDone();
}

NetScanEntry ap(const char* ssid, int8_t rssi, bool open = false,
                uint8_t channel = 6) {
    NetScanEntry e;
    netCopyStr(e.ssid, kSsidBufLen, ssid);
    e.rssi    = rssi;
    e.open    = open;
    e.channel = channel;
    return e;
}

/// Advance the clock and pump the machine, as loop() would.
void advance(uint32_t ms, int ticks = 2) {
    clockMs += ms;
    for (int i = 0; i < ticks; ++i) WifiMgr::poll();
}

}  // namespace fake

/// Fresh manager, fresh NVS, fresh fake radio.
static void setup() {
    prefsMockReset();
    fake::reset();
    WifiMgr::setHalForTest(fake::hal());
}

// ─── initial state ──────────────────────────────────────────────────────────

static void test_starts_idle() {
    setup();
    CHECK(WifiMgr::state() == NetState::IDLE);
    CHECK(!WifiMgr::isConnected());
    CHECK_EQ(0u, WifiMgr::localIP());
    CHECK_STR_EQ("", WifiMgr::lastErrorMessage());
    CHECK_EQ(0, WifiMgr::scanCount());
}

// An unprovisioned device must not bring the radio up at all.
static void test_begin_without_credentials_stays_idle() {
    setup();
    WifiMgr::begin();
    CHECK(WifiMgr::state() == NetState::IDLE);
    CHECK(!WifiMgr::hasSavedCredentials());
    CHECK(WifiMgr::credentialSource() == NetCredSource::NONE);

    WifiMgr::connectWithSaved();
    CHECK_EQ(0, fake::beginStaCalls);
    CHECK(WifiMgr::state() == NetState::IDLE);
}

static void test_begin_loads_stored_credentials() {
    setup();
    netConfigSaveSingle("StoredAP", "storedpw");
    WifiMgr::begin();
    CHECK(WifiMgr::hasSavedCredentials());
    CHECK(WifiMgr::credentialSource() == NetCredSource::NVS);
}

// ─── scanning ───────────────────────────────────────────────────────────────

static void test_scan_success() {
    setup();
    WifiMgr::startScan();
    CHECK(WifiMgr::state() == NetState::SCANNING);
    CHECK_EQ(1, fake::startScanCalls);

    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::SCANNING);   // still running

    fake::finishScanWith({fake::ap("Home", -42), fake::ap("Cafe", -80, true)});
    WifiMgr::poll();

    CHECK(WifiMgr::state() == NetState::SCAN_DONE);
    CHECK_EQ(2, WifiMgr::scanCount());
    CHECK_STR_EQ("Home", WifiMgr::scanEntry(0)->ssid);
    CHECK_EQ(-42, (int)WifiMgr::scanEntry(0)->rssi);
    CHECK(!WifiMgr::scanEntry(0)->open);
    CHECK(WifiMgr::scanEntry(1)->open);
    CHECK_EQ(1, fake::scanDeleteCalls);   // results released
}

static void test_scan_entry_out_of_range_is_null() {
    setup();
    WifiMgr::startScan();
    fake::finishScanWith({fake::ap("Home", -42)});
    WifiMgr::poll();
    CHECK(WifiMgr::scanEntry(-1) == nullptr);
    CHECK(WifiMgr::scanEntry(1)  == nullptr);
    CHECK_NOT_NULL(WifiMgr::scanEntry(0));
}

// Mashing "C" must not stack scans.
static void test_repeated_start_scan_is_ignored() {
    setup();
    WifiMgr::startScan();
    WifiMgr::startScan();
    WifiMgr::startScan();
    CHECK_EQ(1, fake::startScanCalls);
}

static void test_scan_timeout() {
    setup();
    WifiMgr::startScan();
    fake::advance(9000);
    CHECK(WifiMgr::state() == NetState::SCANNING);

    fake::advance(2000);
    CHECK(WifiMgr::state() == NetState::SCAN_FAILED);
    CHECK_STR_EQ("scan timeout", WifiMgr::lastErrorMessage());
}

static void test_scan_start_failure() {
    setup();
    fake::startScanSucceeds = false;
    WifiMgr::startScan();
    CHECK(WifiMgr::state() == NetState::SCAN_FAILED);
    CHECK_STR_EQ("could not start scan", WifiMgr::lastErrorMessage());
}

static void test_empty_scan_reports_no_networks() {
    setup();
    WifiMgr::startScan();
    fake::finishScanWith({});
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::SCAN_DONE);
    CHECK_EQ(0, WifiMgr::scanCount());
    CHECK_STR_EQ("no networks found", WifiMgr::lastErrorMessage());
}

// Hidden networks advertise an empty SSID and cannot be picked from a
// list, so they must not occupy a row.
static void test_hidden_ssids_are_skipped() {
    setup();
    WifiMgr::startScan();
    fake::finishScanWith({fake::ap("Visible", -50), fake::ap("", -60),
                          fake::ap("AlsoVisible", -70)});
    WifiMgr::poll();
    CHECK_EQ(2, WifiMgr::scanCount());
    CHECK_STR_EQ("Visible",     WifiMgr::scanEntry(0)->ssid);
    CHECK_STR_EQ("AlsoVisible", WifiMgr::scanEntry(1)->ssid);
}

static void test_scan_results_are_capped() {
    setup();
    WifiMgr::startScan();
    std::vector<NetScanEntry> many;
    for (int i = 0; i < 100; ++i) {
        many.push_back(fake::ap(("AP" + std::to_string(i)).c_str(), -50));
    }
    fake::finishScanWith(many);
    WifiMgr::poll();
    CHECK(WifiMgr::scanCount() <= WifiMgr::kMaxScanResults);
    CHECK_EQ(WifiMgr::kMaxScanResults, WifiMgr::scanCount());
}

// ─── connecting ─────────────────────────────────────────────────────────────

/// Scan, then select index 0 with the given password.
static void scanAndSelect(const char* pass, bool open = false) {
    WifiMgr::startScan();
    fake::finishScanWith({fake::ap("Home", -42, open)});
    WifiMgr::poll();
    WifiMgr::connect(0, pass);
}

static void test_connect_success_stores_credentials() {
    setup();
    scanAndSelect("s3cret");
    CHECK(WifiMgr::state() == NetState::CONNECTING);
    CHECK_EQ(std::string("Home"),   fake::attempts[0].first);
    CHECK_EQ(std::string("s3cret"), fake::attempts[0].second);

    WifiMgr::notifyGotIp(0xC0A80164);
    WifiMgr::poll();

    CHECK(WifiMgr::state() == NetState::CONNECTED);
    CHECK(WifiMgr::isConnected());
    CHECK_EQ(0xC0A80164u, WifiMgr::localIP());
    CHECK_STR_EQ("Home", WifiMgr::connectedSSID());
    CHECK_STR_EQ("", WifiMgr::lastErrorMessage());

    // Only now may the credentials reach NVS.
    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK(cfg.hasCredentials());
    CHECK_STR_EQ("Home",   cfg.nets[0].ssid);
    CHECK_STR_EQ("s3cret", cfg.nets[0].pass);
    CHECK(WifiMgr::credentialSource() == NetCredSource::NVS);
}

// A mistyped password IS stored when the user commits it from the
// password screen — see connect() in network_manager.cpp. The rationale
// inverted from the original "save only after success" design: if the
// first attempt fails (wrong password, weak signal, …) the user would
// otherwise have to re-type the entire passphrase after every reboot
// just to retry. The explicit forget path is the X key on the network-
// info screen (disconnectAndForget), so a credential the user does not
// want is never permanently stuck in NVS.
static void test_failed_connect_does_not_store_credentials() {
    setup();
    scanAndSelect("wrongpw");
    WifiMgr::notifyDisconnected(202);   // AUTH_FAIL
    WifiMgr::poll();

    CHECK(WifiMgr::state() == NetState::CONNECT_FAILED);
    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK(cfg.hasCredentials());              // saved on commit, NOT on success
    CHECK_STR_EQ("wrongpw", cfg.nets[0].pass);
    // disconnectAndForget is the explicit "remove bad creds" path.
    WifiMgr::disconnectAndForget();
    netConfigLoad(cfg);
    CHECK(!cfg.hasCredentials());
}

static void test_connect_to_open_network_sends_empty_password() {
    setup();
    scanAndSelect("", /*open=*/true);
    CHECK_EQ(std::string(""), fake::attempts[0].second);
    WifiMgr::notifyGotIp(0x0A000002);
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::CONNECTED);
}

static void test_connect_with_bad_index_fails_cleanly() {
    setup();
    WifiMgr::connect(0, "pw");    // no scan results at all
    CHECK(WifiMgr::state() == NetState::CONNECT_FAILED);
    CHECK_STR_EQ("no network selected", WifiMgr::lastErrorMessage());
    CHECK_EQ(0, fake::beginStaCalls);
}

static void test_begin_sta_failure_is_reported() {
    setup();
    fake::beginStaSucceeds = false;
    scanAndSelect("pw");
    CHECK(WifiMgr::state() == NetState::CONNECT_FAILED);
    CHECK_STR_EQ("could not start connection",
                 WifiMgr::lastErrorMessage());
}

static void test_connect_timeout() {
    setup();
    scanAndSelect("pw");
    fake::advance(14000);
    CHECK(WifiMgr::state() == NetState::CONNECTING);
    fake::advance(2000);
    CHECK(WifiMgr::state() == NetState::CONNECT_FAILED);
    CHECK_STR_EQ("connect timeout", WifiMgr::lastErrorMessage());
    CHECK(fake::disconnectCalls > 0);
}

// ─── error messages ─────────────────────────────────────────────────────────
//
// The whole reason for using Wi-Fi events rather than polling
// WiFi.status() is that only the event carries this reason code. These
// assertions are the payoff.

static void test_reason_messages() {
    CHECK_STR_EQ("wrong password", netReasonToMessage(202));
    CHECK_STR_EQ("wrong password", netReasonToMessage(15));
    CHECK_STR_EQ("wrong password", netReasonToMessage(204));
    CHECK_STR_EQ("network not found (5GHz-only?)", netReasonToMessage(201));
    CHECK_STR_EQ("signal lost, reconnecting", netReasonToMessage(200));
    CHECK_STR_EQ("signal lost, reconnecting", netReasonToMessage(8));
    CHECK_STR_EQ("access point refused connection", netReasonToMessage(203));
    CHECK_STR_EQ("connect failed (reason 77)", netReasonToMessage(77));
}

static void test_wrong_password_surfaces_to_user() {
    setup();
    scanAndSelect("nope");
    WifiMgr::notifyDisconnected(202);
    WifiMgr::poll();
    CHECK_STR_EQ("wrong password", WifiMgr::lastErrorMessage());
}

static void test_missing_network_surfaces_to_user() {
    setup();
    scanAndSelect("pw");
    WifiMgr::notifyDisconnected(201);
    WifiMgr::poll();
    CHECK_STR_EQ("network not found (5GHz-only?)",
                 WifiMgr::lastErrorMessage());
}

// ─── retry policy ───────────────────────────────────────────────────────────

// A wrong passphrase will never fix itself; retrying only hammers the AP.
static void test_auth_failure_does_not_retry() {
    setup();
    scanAndSelect("wrongpw");
    WifiMgr::notifyDisconnected(202);
    WifiMgr::poll();
    const int attemptsAfterFailure = fake::beginStaCalls;

    CHECK_EQ(0u, WifiMgr::secondsUntilRetry());
    fake::advance(600000, 20);        // ten minutes of polling
    CHECK_EQ(attemptsAfterFailure, fake::beginStaCalls);
    CHECK(WifiMgr::state() == NetState::CONNECT_FAILED);
}

// Everything else is potentially transient and is retried.
static void test_transient_failure_retries() {
    setup();
    scanAndSelect("pw");
    WifiMgr::notifyDisconnected(201);   // NO_AP_FOUND
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::CONNECT_FAILED);
    CHECK(WifiMgr::secondsUntilRetry() > 0);

    const int before = fake::beginStaCalls;
    fake::advance(1100);
    CHECK_EQ(before + 1, fake::beginStaCalls);
    CHECK(WifiMgr::state() == NetState::CONNECTING);
}

static void test_retry_backoff_grows() {
    setup();
    scanAndSelect("pw");

    uint32_t previousWait = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        WifiMgr::notifyDisconnected(201);
        WifiMgr::poll();
        const uint32_t wait = WifiMgr::secondsUntilRetry();
        CHECK(wait >= previousWait);
        previousWait = wait;
        // Jump past the scheduled retry so the next attempt starts.
        fake::advance(400000);
    }
    // Backoff must be growing, not fixed at one second.
    CHECK(previousWait > 1);
}

static void test_backoff_is_capped() {
    setup();
    scanAndSelect("pw");
    for (int i = 0; i < 20; ++i) {
        WifiMgr::notifyDisconnected(201);
        WifiMgr::poll();
        fake::advance(400000);
    }
    WifiMgr::notifyDisconnected(201);
    WifiMgr::poll();
    // Cap is five minutes.
    CHECK(WifiMgr::secondsUntilRetry() <= 300);
}

static void test_success_resets_backoff() {
    setup();
    scanAndSelect("pw");
    for (int i = 0; i < 5; ++i) {
        WifiMgr::notifyDisconnected(201);
        WifiMgr::poll();
        fake::advance(400000);
    }
    WifiMgr::notifyGotIp(0x0A000003);
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::CONNECTED);
    CHECK_EQ(0u, WifiMgr::secondsUntilRetry());

    // A fresh drop must wait the *initial* backoff, not the grown one.
    WifiMgr::notifyDisconnected(200);
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::DISCONNECTED);
    CHECK(WifiMgr::secondsUntilRetry() <= 1);
}

static void test_manual_retry_ignores_backoff() {
    setup();
    scanAndSelect("pw");
    WifiMgr::notifyDisconnected(201);
    WifiMgr::poll();
    const int before = fake::beginStaCalls;

    WifiMgr::retry();
    CHECK_EQ(before + 1, fake::beginStaCalls);
    CHECK(WifiMgr::state() == NetState::CONNECTING);
}

// ─── link drop while connected ──────────────────────────────────────────────

static void test_link_drop_moves_to_disconnected_and_retries() {
    setup();
    scanAndSelect("pw");
    WifiMgr::notifyGotIp(0xC0A80164);
    WifiMgr::poll();
    CHECK(WifiMgr::isConnected());

    WifiMgr::notifyDisconnected(200);   // BEACON_TIMEOUT
    WifiMgr::poll();

    CHECK(WifiMgr::state() == NetState::DISCONNECTED);
    CHECK(!WifiMgr::isConnected());
    CHECK_EQ(0u, WifiMgr::localIP());
    CHECK_STR_EQ("", WifiMgr::connectedSSID());
    CHECK_STR_EQ("signal lost, reconnecting",
                 WifiMgr::lastErrorMessage());

    const int before = fake::beginStaCalls;
    fake::advance(1100);
    CHECK_EQ(before + 1, fake::beginStaCalls);
}

static void test_reconnect_after_drop_restores_connected() {
    setup();
    scanAndSelect("pw");
    WifiMgr::notifyGotIp(0xC0A80164);
    WifiMgr::poll();
    WifiMgr::notifyDisconnected(200);
    WifiMgr::poll();
    fake::advance(1100);
    CHECK(WifiMgr::state() == NetState::CONNECTING);

    WifiMgr::notifyGotIp(0xC0A80165);
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::CONNECTED);
    CHECK_EQ(0xC0A80165u, WifiMgr::localIP());
}

// ─── cancel ─────────────────────────────────────────────────────────────────

static void test_cancel_during_scan() {
    setup();
    WifiMgr::startScan();
    WifiMgr::cancel();
    CHECK(WifiMgr::state() == NetState::IDLE);
    CHECK(fake::disconnectCalls > 0);

    // A late completion must not resurrect the scan screen.
    fake::finishScanWith({fake::ap("Late", -50)});
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::IDLE);
}

static void test_cancel_during_connect_stops_the_attempt() {
    setup();
    scanAndSelect("pw");
    WifiMgr::cancel();
    CHECK(WifiMgr::state() == NetState::IDLE);
    CHECK(fake::disconnectCalls > 0);

    // A late success must not silently connect after the user backed out.
    WifiMgr::notifyGotIp(0xC0A80164);
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::IDLE);
    CHECK(!WifiMgr::isConnected());
}

static void test_cancel_clears_pending_retry() {
    setup();
    scanAndSelect("pw");
    WifiMgr::notifyDisconnected(201);
    WifiMgr::poll();
    CHECK(WifiMgr::secondsUntilRetry() > 0);

    WifiMgr::cancel();
    const int before = fake::beginStaCalls;
    fake::advance(600000, 20);
    CHECK_EQ(before, fake::beginStaCalls);
}

// ─── forget ─────────────────────────────────────────────────────────────────

static void test_disconnect_and_forget_clears_everything() {
    setup();
    scanAndSelect("s3cret");
    WifiMgr::notifyGotIp(0xC0A80164);
    WifiMgr::poll();

    WifiMgr::disconnectAndForget();

    CHECK(WifiMgr::state() == NetState::IDLE);
    CHECK(!WifiMgr::isConnected());
    CHECK_EQ(0u, WifiMgr::localIP());
    CHECK(!WifiMgr::hasSavedCredentials());
    CHECK(WifiMgr::credentialSource() == NetCredSource::NONE);
    CHECK(fake::disconnectCalls > 0);

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK(!cfg.hasCredentials());
}

// After forgetting, nothing may bring the device back online by itself.
// On device this is also what esp_wifi_set_storage(WIFI_STORAGE_RAM)
// guarantees; here we assert the manager's half of the contract.
static void test_forget_prevents_auto_reconnect() {
    setup();
    scanAndSelect("s3cret");
    WifiMgr::notifyGotIp(0xC0A80164);
    WifiMgr::poll();
    WifiMgr::disconnectAndForget();

    const int before = fake::beginStaCalls;
    fake::advance(600000, 50);
    CHECK_EQ(before, fake::beginStaCalls);

    WifiMgr::connectWithSaved();
    CHECK_EQ(before, fake::beginStaCalls);
    CHECK(WifiMgr::state() == NetState::IDLE);
}

// ─── reboot behaviour ───────────────────────────────────────────────────────

static void test_auto_connect_after_reboot() {
    setup();
    scanAndSelect("s3cret");
    WifiMgr::notifyGotIp(0xC0A80164);
    WifiMgr::poll();

    // Simulate a power cycle: fresh manager, same NVS.
    fake::reset();
    WifiMgr::setHalForTest(fake::hal());

    WifiMgr::begin();
    CHECK(WifiMgr::hasSavedCredentials());
    WifiMgr::connectWithSaved();

    CHECK(WifiMgr::state() == NetState::CONNECTING);
    CHECK_EQ(1u, fake::attempts.size());
    CHECK_EQ(std::string("Home"),   fake::attempts[0].first);
    CHECK_EQ(std::string("s3cret"), fake::attempts[0].second);

    WifiMgr::notifyGotIp(0xC0A80164);
    WifiMgr::poll();
    CHECK(WifiMgr::isConnected());
}

// begin() must not associate on its own — boot ordering depends on it
// being a pure load.
static void test_begin_does_not_associate() {
    setup();
    netConfigSaveSingle("StoredAP", "pw");
    WifiMgr::begin();
    CHECK_EQ(0, fake::beginStaCalls);
    CHECK(WifiMgr::state() == NetState::IDLE);
}

// ─── event ordering robustness ──────────────────────────────────────────────

// Events arrive on the Wi-Fi task and poll() runs on the loop core, so an
// event can land before, during or after any poll. None of it may wedge
// the machine.
static void test_events_before_poll_are_not_lost() {
    setup();
    scanAndSelect("pw");
    WifiMgr::notifyGotIp(0xC0A80164);
    WifiMgr::notifyGotIp(0xC0A80164);   // duplicate
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::CONNECTED);
}

static void test_stray_events_in_idle_are_ignored() {
    setup();
    WifiMgr::notifyDisconnected(200);
    WifiMgr::notifyScanDone();
    WifiMgr::poll();
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::IDLE);
}

static void test_disconnect_event_while_connecting_after_got_ip() {
    setup();
    scanAndSelect("pw");
    WifiMgr::notifyGotIp(0xC0A80164);
    WifiMgr::notifyDisconnected(200);   // both pending
    WifiMgr::poll();
    // GOT_IP wins the same tick; the drop is then handled from CONNECTED.
    CHECK(WifiMgr::state() == NetState::CONNECTED);
    WifiMgr::poll();
    CHECK(WifiMgr::state() == NetState::CONNECTED);
}

static void test_state_changed_at_tracks_transitions() {
    setup();
    fake::clockMs = 5000;
    WifiMgr::startScan();
    CHECK_EQ(5000u, WifiMgr::stateChangedAt());

    fake::clockMs = 7000;
    fake::finishScanWith({fake::ap("Home", -42)});
    WifiMgr::poll();
    CHECK_EQ(7000u, WifiMgr::stateChangedAt());
}

int main() {
    RUN(test_starts_idle);
    RUN(test_begin_without_credentials_stays_idle);
    RUN(test_begin_loads_stored_credentials);

    RUN(test_scan_success);
    RUN(test_scan_entry_out_of_range_is_null);
    RUN(test_repeated_start_scan_is_ignored);
    RUN(test_scan_timeout);
    RUN(test_scan_start_failure);
    RUN(test_empty_scan_reports_no_networks);
    RUN(test_hidden_ssids_are_skipped);
    RUN(test_scan_results_are_capped);

    RUN(test_connect_success_stores_credentials);
    RUN(test_failed_connect_does_not_store_credentials);
    RUN(test_connect_to_open_network_sends_empty_password);
    RUN(test_connect_with_bad_index_fails_cleanly);
    RUN(test_begin_sta_failure_is_reported);
    RUN(test_connect_timeout);

    RUN(test_reason_messages);
    RUN(test_wrong_password_surfaces_to_user);
    RUN(test_missing_network_surfaces_to_user);

    RUN(test_auth_failure_does_not_retry);
    RUN(test_transient_failure_retries);
    RUN(test_retry_backoff_grows);
    RUN(test_backoff_is_capped);
    RUN(test_success_resets_backoff);
    RUN(test_manual_retry_ignores_backoff);

    RUN(test_link_drop_moves_to_disconnected_and_retries);
    RUN(test_reconnect_after_drop_restores_connected);

    RUN(test_cancel_during_scan);
    RUN(test_cancel_during_connect_stops_the_attempt);
    RUN(test_cancel_clears_pending_retry);

    RUN(test_disconnect_and_forget_clears_everything);
    RUN(test_forget_prevents_auto_reconnect);

    RUN(test_auto_connect_after_reboot);
    RUN(test_begin_does_not_associate);

    RUN(test_events_before_poll_are_not_lost);
    RUN(test_stray_events_in_idle_are_ignored);
    RUN(test_disconnect_event_while_connecting_after_got_ip);
    RUN(test_state_changed_at_tracks_transitions);
    return test_summary();
}
