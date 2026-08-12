#pragma once
// Minimal in-memory stand-in for the Arduino ESP32 Preferences (NVS) API,
// sufficient for net_config.cpp on the native test host.
//
// Only the subset net_config actually uses is modelled: begin/end,
// getString/putString, getUChar/putUChar, getUInt/putUInt, remove,
// clear, isKey. Storage is a process-global map keyed by
// "<namespace>/<key>", so a test can construct several Preferences
// objects over the same namespace and see one consistent store —
// mirroring how NVS behaves on device.
//
// Tests reset it between cases with prefsMockReset().

#include <cstdint>
#include <cstring>
#include <map>
#include <string>

/// Backing store. Exposed so tests can inspect or seed raw keys.
inline std::map<std::string, std::string>& prefsMockStore() {
    static std::map<std::string, std::string> s;
    return s;
}

/// Wipe every namespace. Call at the top of each test.
inline void prefsMockReset() {
    prefsMockStore().clear();
}

class Preferences {
public:
    bool begin(const char* ns, bool readOnly = false) {
        _ns       = ns ? ns : "";
        _readOnly = readOnly;
        _open     = true;
        return true;
    }

    void end() { _open = false; }

    bool isKey(const char* key) {
        return prefsMockStore().count(full(key)) > 0;
    }

    bool remove(const char* key) {
        if (_readOnly) return false;
        return prefsMockStore().erase(full(key)) > 0;
    }

    /// Erase every key in the current namespace (NVS nvs_erase_all).
    bool clear() {
        if (_readOnly) return false;
        const std::string prefix = _ns + "/";
        auto& store = prefsMockStore();
        for (auto it = store.begin(); it != store.end(); ) {
            if (it->first.rfind(prefix, 0) == 0) it = store.erase(it);
            else                                 ++it;
        }
        return true;
    }

    // ── strings ─────────────────────────────────────────────────────────
    // Only the char-buffer overload is modelled. net_config deliberately
    // avoids the Arduino String overload so the firmware does no heap
    // allocation when reading credentials.
    size_t getString(const char* key, char* out, size_t maxLen) {
        if (!out || maxLen == 0) return 0;
        auto& store = prefsMockStore();
        auto  it    = store.find(full(key));
        if (it == store.end()) { out[0] = '\0'; return 0; }
        size_t n = it->second.size();
        if (n > maxLen - 1) n = maxLen - 1;
        std::memcpy(out, it->second.data(), n);
        out[n] = '\0';
        return n;
    }

    size_t putString(const char* key, const char* value) {
        if (_readOnly) return 0;
        prefsMockStore()[full(key)] = value ? value : "";
        return value ? std::string(value).size() : 0;
    }

    // ── integers ────────────────────────────────────────────────────────
    uint8_t getUChar(const char* key, uint8_t def = 0) {
        return (uint8_t)getNum(key, def);
    }

    size_t putUChar(const char* key, uint8_t value) {
        return putNum(key, value);
    }

    uint32_t getUInt(const char* key, uint32_t def = 0) {
        return (uint32_t)getNum(key, def);
    }

    size_t putUInt(const char* key, uint32_t value) {
        return putNum(key, value);
    }

private:
    std::string full(const char* key) const {
        return _ns + "/" + (key ? key : "");
    }

    unsigned long long getNum(const char* key, unsigned long long def) {
        auto& store = prefsMockStore();
        auto  it    = store.find(full(key));
        if (it == store.end()) return def;
        return std::stoull(it->second);
    }

    size_t putNum(const char* key, unsigned long long value) {
        if (_readOnly) return 0;
        prefsMockStore()[full(key)] = std::to_string(value);
        return sizeof(value);
    }

    std::string _ns;
    bool        _readOnly = false;
    bool        _open     = false;
};
