#include "test_framework.h"

#include "Preferences.h"   // mock; must precede net_config.h's own include
#include "net_config.h"

#include <string>

// ─── helpers ────────────────────────────────────────────────────────────────

static NetConfig freshConfig(const char* ssid, const char* pass) {
    NetConfig cfg;
    cfg.count = 1;
    netCopyStr(cfg.nets[0].ssid, kSsidBufLen, ssid);
    netCopyStr(cfg.nets[0].pass, kPassBufLen, pass);
    return cfg;
}

/// Raw key lookup, to assert on what actually reached the store.
static bool rawHas(const char* key) {
    return prefsMockStore().count(std::string("net/") + key) > 0;
}
static std::string raw(const char* key) {
    auto it = prefsMockStore().find(std::string("net/") + key);
    return it == prefsMockStore().end() ? std::string("<missing>") : it->second;
}

// ─── netCopyStr ─────────────────────────────────────────────────────────────

static void test_copy_str_basic() {
    char buf[8];
    netCopyStr(buf, sizeof(buf), "abc");
    CHECK_STR_EQ("abc", buf);
}

static void test_copy_str_truncates_and_terminates() {
    char buf[4];
    netCopyStr(buf, sizeof(buf), "abcdefgh");
    CHECK_STR_EQ("abc", buf);
    CHECK_EQ('\0', buf[3]);
}

static void test_copy_str_null_source_yields_empty() {
    char buf[8] = "stale";
    netCopyStr(buf, sizeof(buf), nullptr);
    CHECK_STR_EQ("", buf);
}

static void test_copy_str_exact_fit() {
    char buf[4];
    netCopyStr(buf, sizeof(buf), "abc");
    CHECK_STR_EQ("abc", buf);
}

// ─── empty / unprovisioned device ───────────────────────────────────────────

static void test_load_from_empty_nvs() {
    prefsMockReset();
    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_EQ(0, (int)cfg.count);
    CHECK(!cfg.hasCredentials());
    CHECK(cfg.nets[0].isEmpty());
    // Defaults must be DHCP + station, not zero-by-accident.
    CHECK(cfg.ipMode == IpMode::DHCP);
    CHECK(cfg.mode == NetMode::STATION);
}

// A struct handed to load() must be fully reset, not merged into.
static void test_load_overwrites_caller_struct() {
    prefsMockReset();
    NetConfig cfg = freshConfig("stale", "leftover");
    netConfigLoad(cfg);
    CHECK_EQ(0, (int)cfg.count);
    CHECK_STR_EQ("", cfg.nets[0].ssid);
    CHECK_STR_EQ("", cfg.nets[0].pass);
}

// ─── round trip ─────────────────────────────────────────────────────────────

static void test_save_then_load_single() {
    prefsMockReset();
    CHECK(netConfigSave(freshConfig("MyHomeWiFi", "s3cret pw")));

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_EQ(1, (int)cfg.count);
    CHECK(cfg.hasCredentials());
    CHECK_STR_EQ("MyHomeWiFi", cfg.nets[0].ssid);
    CHECK_STR_EQ("s3cret pw", cfg.nets[0].pass);
}

static void test_round_trip_reserved_fields() {
    prefsMockReset();
    NetConfig cfg = freshConfig("AP", "pw");
    cfg.ipMode = IpMode::STATIC;
    cfg.ip     = 0xC0A80164;   // 192.168.1.100
    cfg.gw     = 0xC0A80101;
    cfg.mask   = 0xFFFFFF00;
    cfg.dns    = 0x08080808;
    cfg.mode   = NetMode::ACCESS_POINT;
    CHECK(netConfigSave(cfg));

    NetConfig back;
    netConfigLoad(back);
    CHECK(back.ipMode == IpMode::STATIC);
    CHECK_EQ(0xC0A80164u, back.ip);
    CHECK_EQ(0xC0A80101u, back.gw);
    CHECK_EQ(0xFFFFFF00u, back.mask);
    CHECK_EQ(0x08080808u, back.dns);
    CHECK(back.mode == NetMode::ACCESS_POINT);
}

static void test_multiple_slots_round_trip() {
    prefsMockReset();
    NetConfig cfg;
    cfg.count = 3;
    netCopyStr(cfg.nets[0].ssid, kSsidBufLen, "home");
    netCopyStr(cfg.nets[0].pass, kPassBufLen, "pw0");
    netCopyStr(cfg.nets[1].ssid, kSsidBufLen, "field");
    netCopyStr(cfg.nets[1].pass, kPassBufLen, "pw1");
    netCopyStr(cfg.nets[2].ssid, kSsidBufLen, "cafe");
    // slot 2 is an open network
    CHECK(netConfigSave(cfg));

    NetConfig back;
    netConfigLoad(back);
    CHECK_EQ(3, (int)back.count);
    CHECK_STR_EQ("home",  back.nets[0].ssid);
    CHECK_STR_EQ("field", back.nets[1].ssid);
    CHECK_STR_EQ("cafe",  back.nets[2].ssid);
    CHECK_STR_EQ("pw1",   back.nets[1].pass);
    CHECK(back.nets[2].isOpen());
    CHECK(back.nets[3].isEmpty());
}

// ─── open networks ──────────────────────────────────────────────────────────

static void test_open_network_has_empty_password() {
    prefsMockReset();
    CHECK(netConfigSave(freshConfig("FreeWiFi", "")));

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK(cfg.hasCredentials());
    CHECK(cfg.nets[0].isOpen());
    CHECK_STR_EQ("", cfg.nets[0].pass);
}

// ─── length limits ──────────────────────────────────────────────────────────

static void test_max_length_ssid_and_passphrase() {
    prefsMockReset();
    const std::string ssid32(32, 'S');   // 802.11 maximum
    const std::string pass63(63, 'P');   // WPA maximum
    CHECK(netConfigSave(freshConfig(ssid32.c_str(), pass63.c_str())));

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_EQ(32u, std::string(cfg.nets[0].ssid).size());
    CHECK_EQ(63u, std::string(cfg.nets[0].pass).size());
    CHECK_STR_EQ(ssid32.c_str(), cfg.nets[0].ssid);
    CHECK_STR_EQ(pass63.c_str(), cfg.nets[0].pass);
}

static void test_oversized_values_are_truncated_not_overflowed() {
    prefsMockReset();
    const std::string huge(200, 'X');
    CHECK(netConfigSaveSingle(huge.c_str(), huge.c_str()));

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_EQ(kSsidBufLen - 1, std::string(cfg.nets[0].ssid).size());
    CHECK_EQ(kPassBufLen - 1, std::string(cfg.nets[0].pass).size());
}

// ─── clear ("forget network") ───────────────────────────────────────────────

static void test_clear_removes_everything() {
    prefsMockReset();
    CHECK(netConfigSave(freshConfig("MyHomeWiFi", "s3cret")));
    CHECK(netConfigClear());

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_EQ(0, (int)cfg.count);
    CHECK(!cfg.hasCredentials());
}

// After a forget, no fragment of the passphrase may remain readable in
// the namespace. This is the assertion that catches "we cleared the
// count but left the strings behind".
static void test_clear_leaves_no_passphrase_behind() {
    prefsMockReset();
    CHECK(netConfigSave(freshConfig("MyHomeWiFi", "s3cret")));
    CHECK(netConfigClear());

    for (const auto& kv : prefsMockStore()) {
        CHECK(kv.second.find("s3cret") == std::string::npos);
    }
    CHECK(!rawHas("pass0"));
    CHECK(!rawHas("ssid0"));
}

static void test_clear_does_not_touch_morse_namespace() {
    prefsMockReset();
    prefsMockStore()["morse/wpm"] = "25";
    CHECK(netConfigSave(freshConfig("AP", "pw")));
    CHECK(netConfigClear());
    // The keyer settings must be untouched — the whole reason for a
    // separate namespace.
    CHECK_EQ(std::string("25"), prefsMockStore()["morse/wpm"]);
}

// ─── shrinking the list must not leave stale credentials ────────────────────

static void test_shrinking_list_erases_unused_slots() {
    prefsMockReset();
    NetConfig cfg;
    cfg.count = 3;
    netCopyStr(cfg.nets[0].ssid, kSsidBufLen, "a");
    netCopyStr(cfg.nets[1].ssid, kSsidBufLen, "b");
    netCopyStr(cfg.nets[1].pass, kPassBufLen, "oldsecret");
    netCopyStr(cfg.nets[2].ssid, kSsidBufLen, "c");
    CHECK(netConfigSave(cfg));
    CHECK(rawHas("ssid1"));

    // Now save just one network.
    CHECK(netConfigSave(freshConfig("only", "pw")));

    CHECK(!rawHas("ssid1"));
    CHECK(!rawHas("pass1"));
    CHECK(!rawHas("ssid2"));
    CHECK_EQ(std::string("1"), raw("n"));

    NetConfig back;
    netConfigLoad(back);
    CHECK_EQ(1, (int)back.count);
    CHECK(back.nets[1].isEmpty());
}

// ─── netConfigSaveSingle ────────────────────────────────────────────────────

static void test_save_single_stores_one_network() {
    prefsMockReset();
    CHECK(netConfigSaveSingle("MyAP", "pw"));

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_EQ(1, (int)cfg.count);
    CHECK_STR_EQ("MyAP", cfg.nets[0].ssid);
    CHECK_STR_EQ("pw",   cfg.nets[0].pass);
}

static void test_save_single_replaces_previous() {
    prefsMockReset();
    CHECK(netConfigSaveSingle("first", "pw1"));
    CHECK(netConfigSaveSingle("second", "pw2"));

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_EQ(1, (int)cfg.count);
    CHECK_STR_EQ("second", cfg.nets[0].ssid);
    CHECK_STR_EQ("pw2",    cfg.nets[0].pass);
}

// Joining a new network from the scan list must not wipe a static-IP
// setup once that feature lands.
static void test_save_single_preserves_reserved_fields() {
    prefsMockReset();
    NetConfig cfg = freshConfig("old", "pw");
    cfg.ipMode = IpMode::STATIC;
    cfg.ip     = 0x0A000005;
    cfg.dns    = 0x01010101;
    CHECK(netConfigSave(cfg));

    CHECK(netConfigSaveSingle("new", "pw2"));

    NetConfig back;
    netConfigLoad(back);
    CHECK_STR_EQ("new", back.nets[0].ssid);
    CHECK(back.ipMode == IpMode::STATIC);
    CHECK_EQ(0x0A000005u, back.ip);
    CHECK_EQ(0x01010101u, back.dns);
}

static void test_save_single_rejects_empty_ssid() {
    prefsMockReset();
    CHECK(!netConfigSaveSingle("", "pw"));
    CHECK(!netConfigSaveSingle(nullptr, "pw"));
    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_EQ(0, (int)cfg.count);
}

static void test_save_single_open_network() {
    prefsMockReset();
    CHECK(netConfigSaveSingle("FreeWiFi", ""));
    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK(cfg.hasCredentials());
    CHECK(cfg.nets[0].isOpen());
}

// ─── corrupt / hostile store ────────────────────────────────────────────────

// A corrupted count must not send the loader past the end of the array.
static void test_corrupt_count_is_clamped() {
    prefsMockReset();
    prefsMockStore()["net/n"] = "200";
    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK(cfg.count <= kMaxNetworks);
}

// A non-zero count with no matching strings is inconsistent; the loader
// must survive it and report no usable credentials.
static void test_count_without_ssid_is_not_usable() {
    prefsMockReset();
    prefsMockStore()["net/n"] = "1";
    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_EQ(1, (int)cfg.count);
    CHECK(cfg.nets[0].isEmpty());
    CHECK(!cfg.hasCredentials());
}

// ─── AP passphrase ──────────────────────────────────────────────────────────

static void test_ap_password_round_trip() {
    prefsMockReset();
    CHECK(netConfigSaveAp("ap-secret"));

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_STR_EQ("ap-secret", cfg.apPass);
    CHECK(cfg.mode == NetMode::ACCESS_POINT);
}

static void test_ap_password_open_when_empty() {
    prefsMockReset();
    CHECK(netConfigSaveAp(""));

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_STR_EQ("", cfg.apPass);
    CHECK(cfg.mode == NetMode::ACCESS_POINT);
}

// Saving the AP password MUST NOT touch the STA credentials — that is
// the whole point of keeping them in separate keys.
static void test_save_ap_preserves_sta_credentials() {
    prefsMockReset();
    CHECK(netConfigSaveSingle("HomeWiFi", "sta-pw"));

    CHECK(netConfigSaveAp("ap-pw"));

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_STR_EQ("HomeWiFi", cfg.nets[0].ssid);
    CHECK_STR_EQ("sta-pw",   cfg.nets[0].pass);
    CHECK_STR_EQ("ap-pw",    cfg.apPass);
}

// And vice versa: saving a STA credential after the AP password was
// set must not erase the AP password.
static void test_save_sta_preserves_ap_password() {
    prefsMockReset();
    CHECK(netConfigSaveAp("ap-pw"));

    CHECK(netConfigSaveSingle("HomeWiFi", "sta-pw"));

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_STR_EQ("HomeWiFi", cfg.nets[0].ssid);
    CHECK_STR_EQ("sta-pw",   cfg.nets[0].pass);
    CHECK_STR_EQ("ap-pw",    cfg.apPass);
    CHECK(cfg.mode == NetMode::ACCESS_POINT);   // mode NOT reset by STA save
}

static void test_clear_ap_keeps_sta_credentials() {
    prefsMockReset();
    CHECK(netConfigSaveSingle("HomeWiFi", "sta-pw"));
    CHECK(netConfigSaveAp("ap-pw"));
    CHECK(netConfigClearAp());

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_STR_EQ("HomeWiFi", cfg.nets[0].ssid);
    CHECK_STR_EQ("sta-pw",   cfg.nets[0].pass);
    CHECK_STR_EQ("",         cfg.apPass);
    // Mode flag stays whatever it was — clearing the AP pw is about
    // "open the next AP", not "leave AP mode".
}

static void test_clear_erases_ap_password_too() {
    prefsMockReset();
    CHECK(netConfigSaveAp("ap-pw"));
    CHECK(netConfigClear());

    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_STR_EQ("", cfg.apPass);
    CHECK(cfg.mode == NetMode::STATION);
}

static void test_ap_password_truncates_at_max_len() {
    prefsMockReset();
    const std::string huge(100, 'Q');
    CHECK(netConfigSaveAp(huge.c_str()));
    NetConfig cfg;
    netConfigLoad(cfg);
    CHECK_EQ(kPassBufLen - 1, std::string(cfg.apPass).size());
}

int main() {
    RUN(test_copy_str_basic);
    RUN(test_copy_str_truncates_and_terminates);
    RUN(test_copy_str_null_source_yields_empty);
    RUN(test_copy_str_exact_fit);

    RUN(test_load_from_empty_nvs);
    RUN(test_load_overwrites_caller_struct);

    RUN(test_save_then_load_single);
    RUN(test_round_trip_reserved_fields);
    RUN(test_multiple_slots_round_trip);
    RUN(test_open_network_has_empty_password);

    RUN(test_max_length_ssid_and_passphrase);
    RUN(test_oversized_values_are_truncated_not_overflowed);

    RUN(test_clear_removes_everything);
    RUN(test_clear_leaves_no_passphrase_behind);
    RUN(test_clear_does_not_touch_morse_namespace);
    RUN(test_shrinking_list_erases_unused_slots);

    RUN(test_save_single_stores_one_network);
    RUN(test_save_single_replaces_previous);
    RUN(test_save_single_preserves_reserved_fields);
    RUN(test_save_single_rejects_empty_ssid);
    RUN(test_save_single_open_network);

    RUN(test_corrupt_count_is_clamped);
    RUN(test_count_without_ssid_is_not_usable);

    RUN(test_ap_password_round_trip);
    RUN(test_ap_password_open_when_empty);
    RUN(test_save_ap_preserves_sta_credentials);
    RUN(test_save_sta_preserves_ap_password);
    RUN(test_clear_ap_keeps_sta_credentials);
    RUN(test_clear_erases_ap_password_too);
    RUN(test_ap_password_truncates_at_max_len);
    return test_summary();
}
