#pragma once
/**
 * net_config.h — Persistence of Wi-Fi credentials in NVS.
 *
 * Storage lives in the Preferences namespace "net", deliberately separate
 * from the "morse" namespace that holds the keyer settings, so that
 * clearing the network configuration can never disturb WPM, sidetone or
 * keying state.
 *
 * See docs/network.md §4 and §5 for the schema and for why the
 * passphrase is stored in plaintext.
 *
 * Two design points worth knowing before editing this file:
 *
 *  - The store holds a LIST of up to kMaxNetworks entries even though the
 *    current UI only ever writes slot 0. Storage layout is the expensive
 *    thing to change once firmware is in the field, so the slots exist
 *    from the first release; adding "remember several networks" later is
 *    then a UI change, not a migration.
 *
 *  - Static-address and access-point fields are present, persisted and
 *    round-tripped, but not yet surfaced in the UI. They are the seam for
 *    the next iteration.
 *
 * Everything here is free functions over a plain struct so the whole
 * module can be exercised on the host against a mock Preferences —
 * see test/test_net_config.
 */
#include <cstddef>
#include <cstdint>

/// NVS namespace. Separate from "morse".
inline constexpr const char* kNetNamespace = "net";

/// Buffer sizes include the terminator.
inline constexpr size_t kSsidBufLen = 33;   ///< SSID is at most 32 bytes
inline constexpr size_t kPassBufLen = 64;   ///< WPA passphrase at most 63

/// Number of credential slots the schema reserves.
inline constexpr uint8_t kMaxNetworks = 4;

/// How the station obtains its address.
enum class IpMode : uint8_t {
    DHCP   = 0,
    STATIC = 1,   ///< reserved; not yet exposed in the UI
};

/// Which role the radio takes.
enum class NetMode : uint8_t {
    STATION = 0,
    ACCESS_POINT = 1,   ///< reserved; not yet exposed in the UI
};

/// One stored access point.
struct NetCredential {
    char ssid[kSsidBufLen] = {0};
    char pass[kPassBufLen] = {0};

    bool isEmpty() const { return ssid[0] == '\0'; }
    bool isOpen()  const { return pass[0] == '\0'; }
};

/// The complete contents of the "net" namespace.
struct NetConfig {
    uint8_t       count = 0;                 ///< populated slots, 0..kMaxNetworks
    NetCredential nets[kMaxNetworks];

    // Reserved for static addressing (docs/network.md §12). Persisted and
    // round-tripped now so the schema does not need migrating later.
    IpMode   ipMode = IpMode::DHCP;
    uint32_t ip     = 0;
    uint32_t gw     = 0;
    uint32_t mask   = 0;
    uint32_t dns    = 0;

    // Reserved for access-point mode.
    NetMode mode = NetMode::STATION;

    /// True when at least one credential is stored.
    bool hasCredentials() const { return count > 0 && !nets[0].isEmpty(); }
};

/// Read the whole namespace. Missing keys yield defaults, so an
/// unprovisioned device returns a zeroed config with count == 0.
/// Never fails; a fresh NVS simply reads as empty.
void netConfigLoad(NetConfig& out);

/// Write the whole namespace, replacing any previous contents. Slots
/// beyond `count` are erased rather than left stale.
/// Returns false if the namespace could not be opened for writing.
bool netConfigSave(const NetConfig& cfg);

/// Erase every key in the "net" namespace. This is what "forget network"
/// calls. Note that on device it must be paired with
/// esp_wifi_set_storage(WIFI_STORAGE_RAM) at startup, or the Wi-Fi
/// driver's own shadow copy of the credentials survives this call —
/// see docs/network.md §4.2.
bool netConfigClear();

/// Convenience: store a single network as the only entry, preserving the
/// reserved addressing fields. This is the path the scan-and-connect UI
/// uses today.
bool netConfigSaveSingle(const char* ssid, const char* pass);

/// Copy `src` into `dst` (capacity `cap`, including terminator),
/// truncating if necessary and always terminating. Exposed because both
/// the config layer and the network manager need it.
void netCopyStr(char* dst, size_t cap, const char* src);
