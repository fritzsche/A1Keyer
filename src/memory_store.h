#pragma once
/**
 * memory_store.h — Persistence of CW memory-keyer slots in NVS.
 *
 * Storage lives in the Preferences namespace "memory", deliberately
 * separate from the "morse" namespace (keyer settings) and the "net"
 * namespace (Wi-Fi credentials) so that clearing memories can never
 * disturb WPM, sidetone, keying state, or saved networks.
 *
 * Ten slots addressed by the keyboard digits '0'..'9'. Each slot holds
 * a short CW phrase — typical contest macros ("CQ CQ DE W1AW K",
 * "TU 5NN 001") fit comfortably under kMemLen-1 = 31 characters.
 *
 * The store is a plain struct so the whole module can be exercised on
 * the host against a mock Preferences — see test/test_memory_store.
 */
#include <cstddef>
#include <cstdint>

/// NVS namespace. Separate from "morse" and "net".
inline constexpr const char* kMemoryNamespace = "memory";

/// Number of memory slots, addressed by the keyboard digits '0'..'9'.
inline constexpr uint8_t kMemSlots = 10;

/// Maximum length of one slot's CW text. Includes the terminator, so the
/// longest accepted string is kMemLen - 1 = 80 characters — long enough
/// for contest exchanges like "TU 5NN 001 BK" or a multi-line CQ. The
/// editor screen scrolls horizontally when the cursor runs past the
/// visible window at the size-2 font, so longer macros stay readable.
inline constexpr size_t kMemLen = 81;

/// All ten memory slots as one persisted blob.
struct MemoryBank {
    char slot[kMemSlots][kMemLen] = {{0}};

    /// True when this slot has no stored text (empty / never set).
    bool isSlotEmpty(uint8_t i) const { return slot[i][0] == '\0'; }
};

/// Read the whole namespace. Missing keys yield empty slots, so an
/// unprovisioned device returns a zeroed bank. Never fails; a fresh
/// NVS simply reads as empty.
void memoryBankLoad(MemoryBank& out);

/// Write the whole namespace, replacing any previous contents.
/// Returns false if the namespace could not be opened for writing.
bool memoryBankSave(const MemoryBank& bank);

/// Erase every key in the "memory" namespace. Useful for a future
/// "forget all memories" gesture and as a defensive wipe in tests.
bool memoryBankClear();

/// Copy `src` into `dst` (capacity `cap`, including terminator),
/// truncating if necessary and always terminating. Exposed so callers
/// that own a slot buffer don't have to duplicate the truncation
/// dance (see MorseModel::setMemory).
void memCopyStr(char* dst, size_t cap, const char* src);
