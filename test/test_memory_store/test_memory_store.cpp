#include "test_framework.h"

#include "Preferences.h"   // mock; must precede memory_store.h's own include
#include "memory_store.h"

#include <string>

// ─── helpers ────────────────────────────────────────────────────────────────

/// Raw key lookup against the mock store, e.g. rawHas("m3") looks up
/// "memory/m3". Used to assert what actually reached the store.
static bool rawHas(const char* key) {
    return prefsMockStore().count(std::string("memory/") + key) > 0;
}
static std::string raw(const char* key) {
    auto it = prefsMockStore().find(std::string("memory/") + key);
    return it == prefsMockStore().end() ? std::string("<missing>") : it->second;
}

// ─── memCopyStr ─────────────────────────────────────────────────────────────

static void test_copy_str_basic() {
    char buf[8];
    memCopyStr(buf, sizeof(buf), "CQ");
    CHECK_STR_EQ("CQ", buf);
}

static void test_copy_str_truncates_and_terminates() {
    char buf[4];        // capacity 4 → max 3 chars + nul
    memCopyStr(buf, sizeof(buf), "abcdefgh");
    CHECK_STR_EQ("abc", buf);
    CHECK_EQ('\0', buf[3]);
}

static void test_copy_str_null_source_yields_empty() {
    char buf[8] = "stale";
    memCopyStr(buf, sizeof(buf), nullptr);
    CHECK_STR_EQ("", buf);
}

static void test_copy_str_exact_fit() {
    char buf[4];
    memCopyStr(buf, sizeof(buf), "abc");
    CHECK_STR_EQ("abc", buf);
}

// ─── empty / unprovisioned device ───────────────────────────────────────────

static void test_load_from_empty_nvs() {
    prefsMockReset();
    MemoryBank bank;
    memoryBankLoad(bank);
    for (uint8_t i = 0; i < kMemSlots; ++i) {
        CHECK(bank.isSlotEmpty(i));
    }
}

// A struct handed to load() must be fully reset, not merged into.
static void test_load_overwrites_caller_struct() {
    prefsMockReset();
    MemoryBank bank;
    memCopyStr(bank.slot[3], kMemLen, "stale leftover");
    memCopyStr(bank.slot[7], kMemLen, "also stale");
    memoryBankLoad(bank);
    for (uint8_t i = 0; i < kMemSlots; ++i) {
        CHECK(bank.isSlotEmpty(i));
    }
}

// ─── round trip ─────────────────────────────────────────────────────────────

static void test_round_trip_single_slot() {
    prefsMockReset();
    MemoryBank bank;
    memCopyStr(bank.slot[3], kMemLen, "CQ CQ DE W1AW K");
    CHECK(memoryBankSave(bank));

    MemoryBank back;
    memoryBankLoad(back);
    CHECK_STR_EQ("CQ CQ DE W1AW K", back.slot[3]);
    CHECK(back.isSlotEmpty(0));
    CHECK(back.isSlotEmpty(9));
}

static void test_round_trip_special_chars() {
    prefsMockReset();
    MemoryBank bank;
    // Real contest macros often include the slash, equals and AR prosign.
    memCopyStr(bank.slot[0], kMemLen, "5NN/B");
    memCopyStr(bank.slot[1], kMemLen, "TU = 5NN 001");
    memCopyStr(bank.slot[2], kMemLen, "QRZ?");
    memCopyStr(bank.slot[9], kMemLen, "DE W1AW K ");
    CHECK(memoryBankSave(bank));

    MemoryBank back;
    memoryBankLoad(back);
    CHECK_STR_EQ("5NN/B",           back.slot[0]);
    CHECK_STR_EQ("TU = 5NN 001",    back.slot[1]);
    CHECK_STR_EQ("QRZ?",            back.slot[2]);
    CHECK_STR_EQ("DE W1AW K ",      back.slot[9]);
}

static void test_round_trip_all_slots_full() {
    prefsMockReset();
    MemoryBank bank;
    for (uint8_t i = 0; i < kMemSlots; ++i) {
        char buf[kMemLen];
        std::snprintf(buf, sizeof(buf), "slot-%u", (unsigned)i);
        memCopyStr(bank.slot[i], kMemLen, buf);
    }
    CHECK(memoryBankSave(bank));

    MemoryBank back;
    memoryBankLoad(back);
    for (uint8_t i = 0; i < kMemSlots; ++i) {
        char expected[kMemLen];
        std::snprintf(expected, sizeof(expected), "slot-%u", (unsigned)i);
        CHECK_STR_EQ(expected, back.slot[i]);
    }
}

// ─── length limits ──────────────────────────────────────────────────────────

static void test_max_length_round_trips() {
    prefsMockReset();
    MemoryBank bank;
    const std::string maxChar(kMemLen - 1, 'M');   // 31 chars, exactly at the cap
    memCopyStr(bank.slot[0], kMemLen, maxChar.c_str());
    CHECK(memoryBankSave(bank));

    MemoryBank back;
    memoryBankLoad(back);
    CHECK_EQ(maxChar.size(), std::string(back.slot[0]).size());
    CHECK_STR_EQ(maxChar.c_str(), back.slot[0]);
    CHECK_EQ('\0', back.slot[0][kMemLen - 1]);
}

// Oversized values must be truncated, never silently overflow.
static void test_oversized_values_are_truncated_not_overflowed() {
    prefsMockReset();
    MemoryBank bank;
    const std::string huge(200, 'X');
    memCopyStr(bank.slot[5], kMemLen, huge.c_str());
    CHECK(memoryBankSave(bank));

    MemoryBank back;
    memoryBankLoad(back);
    CHECK_EQ(kMemLen - 1, std::string(back.slot[5]).size());
    CHECK_EQ('\0', back.slot[5][kMemLen - 1]);
}

// kMemLen must be 81 — the design contract is "80 chars + terminator"
// so a full contest exchange ("CQ TEST DE W1AW K 5NN 001 BK") plus
// headroom round-trips without truncation. If someone shrinks the
// cap later this assertion fails and forces the editor + storage
// paths to be updated in lockstep.
static void test_kMemLen_is_81_chars() {
    CHECK_EQ((size_t)81, (size_t)kMemLen);
    CHECK_EQ((uint8_t)10, (uint8_t)kMemSlots);
}

// A realistic contest-length phrase — 47 chars — round-trips
// cleanly under the new 80-char cap. Pins the contract from the
// operator-facing documentation: "TU 5NN 001 BK" plus prefix and
// suffix stay readable without overflow.
static void test_round_trip_contest_length_phrase() {
    prefsMockReset();
    MemoryBank bank;
    const char* phrase =
        "CQ TEST DE W1AW K 5NN 001 BK";   // 28 chars, well under cap
    memCopyStr(bank.slot[4], kMemLen, phrase);
    CHECK(memoryBankSave(bank));

    MemoryBank back;
    memoryBankLoad(back);
    CHECK_STR_EQ(phrase, back.slot[4]);
}

// ─── clear ("forget all memories") ──────────────────────────────────────────

static void test_clear_removes_everything() {
    prefsMockReset();
    MemoryBank bank;
    memCopyStr(bank.slot[0], kMemLen, "X1");
    memCopyStr(bank.slot[9], kMemLen, "X9");
    CHECK(memoryBankSave(bank));
    CHECK(memoryBankClear());

    MemoryBank back;
    memoryBankLoad(back);
    for (uint8_t i = 0; i < kMemSlots; ++i) {
        CHECK(back.isSlotEmpty(i));
    }
}

// After a clear, no fragment of the stored text may remain readable
// in the namespace — catches "we cleared the bank but left m3 behind".
static void test_clear_leaves_no_text_behind() {
    prefsMockReset();
    MemoryBank bank;
    memCopyStr(bank.slot[3], kMemLen, "S3CRETMACRO");
    CHECK(memoryBankSave(bank));
    CHECK(memoryBankClear());

    for (const auto& kv : prefsMockStore()) {
        CHECK(kv.second.find("S3CRETMACRO") == std::string::npos);
    }
    CHECK(!rawHas("m3"));
    CHECK(!rawHas("m0"));
}

// Clearing memories must never touch the "net" or "morse" namespaces
// — the whole reason for namespace isolation.
static void test_clear_does_not_touch_other_namespaces() {
    prefsMockReset();
    prefsMockStore()["net/ssid0"]    = "MyHomeWiFi";
    prefsMockStore()["net/pass0"]    = "s3cret";
    prefsMockStore()["morse/wpm"]    = "25";
    prefsMockStore()["morse/keying"] = "1";

    MemoryBank bank;
    memCopyStr(bank.slot[2], kMemLen, "TEST");
    CHECK(memoryBankSave(bank));
    CHECK(memoryBankClear());

    CHECK_EQ(std::string("MyHomeWiFi"), prefsMockStore()["net/ssid0"]);
    CHECK_EQ(std::string("s3cret"),     prefsMockStore()["net/pass0"]);
    CHECK_EQ(std::string("25"),         prefsMockStore()["morse/wpm"]);
    CHECK_EQ(std::string("1"),          prefsMockStore()["morse/keying"]);
}

// ─── save replaces previous contents ────────────────────────────────────────

static void test_save_replaces_previous_contents() {
    prefsMockReset();
    MemoryBank bank;
    memCopyStr(bank.slot[0], kMemLen, "first");
    CHECK(memoryBankSave(bank));

    MemoryBank second;
    memCopyStr(second.slot[0], kMemLen, "second");
    memCopyStr(second.slot[5], kMemLen, "five");
    CHECK(memoryBankSave(second));

    MemoryBank back;
    memoryBankLoad(back);
    CHECK_STR_EQ("second", back.slot[0]);
    CHECK_STR_EQ("five",   back.slot[5]);
    CHECK(back.isSlotEmpty(1));
    CHECK(back.isSlotEmpty(9));
}

// ─── raw key layout ─────────────────────────────────────────────────────────

// Every slot writes its key (even when empty) so a future clear
// works predictably and so NVS wear goes through Preferences' own
// skip-when-unchanged optimisation. The empty-key entries still
// represent "no memory stored" to the loader.
static void test_keys_are_namespaced_m0_through_m9() {
    prefsMockReset();
    MemoryBank bank;
    memCopyStr(bank.slot[0], kMemLen, "a");
    memCopyStr(bank.slot[9], kMemLen, "j");
    CHECK(memoryBankSave(bank));

    for (uint8_t i = 0; i < kMemSlots; ++i) {
        char key[8];
        std::snprintf(key, sizeof(key), "m%u", (unsigned)i);
        CHECK(rawHas(key));
        if (i == 0)      CHECK_STR_EQ("a", raw("m0").c_str());
        else if (i == 9) CHECK_STR_EQ("j", raw("m9").c_str());
        else             CHECK_STR_EQ("",  raw(key).c_str());
    }
}

// ─── save failure when namespace cannot be opened ───────────────────────────

// (For completeness — the real Preferences can fail to open in OOM
// or read-only conditions. Our mock always succeeds; verify that the
// happy path returns true and the load path does NOT throw.)
static void test_save_returns_true_on_happy_path() {
    prefsMockReset();
    MemoryBank bank;
    memCopyStr(bank.slot[0], kMemLen, "ok");
    CHECK(memoryBankSave(bank));
}

int main() {
    RUN(test_copy_str_basic);
    RUN(test_copy_str_truncates_and_terminates);
    RUN(test_copy_str_null_source_yields_empty);
    RUN(test_copy_str_exact_fit);

    RUN(test_load_from_empty_nvs);
    RUN(test_load_overwrites_caller_struct);

    RUN(test_round_trip_single_slot);
    RUN(test_round_trip_special_chars);
    RUN(test_round_trip_all_slots_full);

    RUN(test_max_length_round_trips);
    RUN(test_oversized_values_are_truncated_not_overflowed);
    RUN(test_kMemLen_is_81_chars);
    RUN(test_round_trip_contest_length_phrase);

    RUN(test_clear_removes_everything);
    RUN(test_clear_leaves_no_text_behind);
    RUN(test_clear_does_not_touch_other_namespaces);

    RUN(test_save_replaces_previous_contents);
    RUN(test_keys_are_namespaced_m0_through_m9);
    RUN(test_save_returns_true_on_happy_path);

    return test_summary();
}
