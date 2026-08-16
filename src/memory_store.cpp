#include "memory_store.h"

#include <Preferences.h>

#include <cstdio>
#include <cstring>

void memCopyStr(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = std::strlen(src);
    if (n > cap - 1) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

namespace {

/// Build the per-slot key name, e.g. "m3". Keeping this in one place
/// stops the load and save paths from drifting apart.
void slotKey(char* out, size_t cap, uint8_t slot) {
    std::snprintf(out, cap, "m%u", (unsigned)slot);
}

}  // namespace

void memoryBankLoad(MemoryBank& out) {
    // Reset every slot so a load from a partial / corrupt namespace
    // never leaves stale strings lying around in the caller's struct.
    for (uint8_t i = 0; i < kMemSlots; ++i) out.slot[i][0] = '\0';

    Preferences prefs;
    if (!prefs.begin(kMemoryNamespace, /*readOnly=*/true)) {
        // Namespace does not exist yet — fresh device. Defaults are
        // already in `out` from the reset above; not an error.
        return;
    }

    for (uint8_t i = 0; i < kMemSlots; ++i) {
        char key[8];
        slotKey(key, sizeof(key), i);
        prefs.getString(key, out.slot[i], kMemLen);
    }

    prefs.end();
}

bool memoryBankSave(const MemoryBank& bank) {
    Preferences prefs;
    if (!prefs.begin(kMemoryNamespace, /*readOnly=*/false)) return false;

    for (uint8_t i = 0; i < kMemSlots; ++i) {
        char key[8];
        slotKey(key, sizeof(key), i);
        // Always write every slot — Preferences' own write-skip-when-
        // unchanged optimisation handles the wear side. Crucially we
        // also write empty strings so the user can clear a single slot
        // by saving it as "" without us having to track which keys
        // exist. NVS stores an empty blob cheaply.
        prefs.putString(key, bank.slot[i]);
    }

    prefs.end();
    return true;
}

bool memoryBankClear() {
    Preferences prefs;
    if (!prefs.begin(kMemoryNamespace, /*readOnly=*/false)) return false;
    const bool ok = prefs.clear();
    prefs.end();
    return ok;
}
