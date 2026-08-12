#include "net_config.h"

#include <Preferences.h>

#include <cstdio>
#include <cstring>

void netCopyStr(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = std::strlen(src);
    if (n > cap - 1) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

namespace {

/// Build the per-slot key name, e.g. "ssid0". Keeping this in one place
/// stops the load and save paths from drifting apart.
void slotKey(char* out, size_t cap, const char* stem, uint8_t slot) {
    std::snprintf(out, cap, "%s%u", stem, (unsigned)slot);
}

}  // namespace

void netConfigLoad(NetConfig& out) {
    out = NetConfig{};

    Preferences prefs;
    if (!prefs.begin(kNetNamespace, /*readOnly=*/true)) {
        // Namespace does not exist yet — an unprovisioned device. Leaving
        // `out` at its defaults is the correct answer, not an error.
        return;
    }

    uint8_t count = prefs.getUChar("n", 0);
    if (count > kMaxNetworks) count = kMaxNetworks;   // guard corrupt store
    out.count = count;

    for (uint8_t i = 0; i < count; ++i) {
        char key[16];
        slotKey(key, sizeof(key), "ssid", i);
        prefs.getString(key, out.nets[i].ssid, kSsidBufLen);
        slotKey(key, sizeof(key), "pass", i);
        prefs.getString(key, out.nets[i].pass, kPassBufLen);
    }

    out.ipMode = (IpMode)prefs.getUChar("ip_mode", (uint8_t)IpMode::DHCP);
    out.ip     = prefs.getUInt("ip", 0);
    out.gw     = prefs.getUInt("gw", 0);
    out.mask   = prefs.getUInt("mask", 0);
    out.dns    = prefs.getUInt("dns", 0);
    out.mode   = (NetMode)prefs.getUChar("mode", (uint8_t)NetMode::STATION);

    prefs.end();
}

bool netConfigSave(const NetConfig& cfg) {
    Preferences prefs;
    if (!prefs.begin(kNetNamespace, /*readOnly=*/false)) return false;

    uint8_t count = cfg.count;
    if (count > kMaxNetworks) count = kMaxNetworks;

    prefs.putUChar("n", count);

    for (uint8_t i = 0; i < kMaxNetworks; ++i) {
        char key[16];
        if (i < count) {
            slotKey(key, sizeof(key), "ssid", i);
            prefs.putString(key, cfg.nets[i].ssid);
            slotKey(key, sizeof(key), "pass", i);
            prefs.putString(key, cfg.nets[i].pass);
        } else {
            // Drop unused slots rather than leaving a stale passphrase
            // readable in flash after the user shrinks the list.
            slotKey(key, sizeof(key), "ssid", i);
            prefs.remove(key);
            slotKey(key, sizeof(key), "pass", i);
            prefs.remove(key);
        }
    }

    prefs.putUChar("ip_mode", (uint8_t)cfg.ipMode);
    prefs.putUInt ("ip",      cfg.ip);
    prefs.putUInt ("gw",      cfg.gw);
    prefs.putUInt ("mask",    cfg.mask);
    prefs.putUInt ("dns",     cfg.dns);
    prefs.putUChar("mode",    (uint8_t)cfg.mode);

    prefs.end();
    return true;
}

bool netConfigClear() {
    Preferences prefs;
    if (!prefs.begin(kNetNamespace, /*readOnly=*/false)) return false;
    const bool ok = prefs.clear();
    prefs.end();
    return ok;
}

bool netConfigSaveSingle(const char* ssid, const char* pass) {
    if (!ssid || ssid[0] == '\0') return false;

    // Preserve the reserved addressing fields so that saving a new
    // network from the scan list does not silently reset a static-IP
    // configuration once that feature exists.
    NetConfig cfg;
    netConfigLoad(cfg);

    cfg.count = 1;
    netCopyStr(cfg.nets[0].ssid, kSsidBufLen, ssid);
    netCopyStr(cfg.nets[0].pass, kPassBufLen, pass);
    for (uint8_t i = 1; i < kMaxNetworks; ++i) cfg.nets[i] = NetCredential{};

    return netConfigSave(cfg);
}
