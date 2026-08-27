#include "test_framework.h"
#include "morse_encoder.h"

// Wabun-specific tests: the encoder/decoder must switch to the Wabun table
// and resolve katakana dit-dah patterns.
//
// Notes on the actual API surface:
//   - MorseEncoder::morseFromChar(char) is byte-oriented — it cannot
//     resolve a UTF-8 multi-byte sequence (the Wabun table stores each
//     katakana as a 3-byte UTF-8 string). There is no
//     morseFromUTF8() in the encoder. Wabun encoding is handled by the
//     encoder's byte-stream driver when the user has switched the
//     MorseTableMode to WABUN_* (see MorseModel::setMorseTableMode).
//   - MorseEncoder::charFromMorse(ditDah) IS table-aware and returns
//     the multi-byte UTF-8 string for Wabun lookups, so the test uses
//     it as the round-trip entry point.

static void test_wabun_table_is_available() {
    // The Wabun table should exist and have entries.
    // kWabunMorseTable is declared in MorseTable.h
    const MorseTable* tbl = &kWabunMorseTable;
    CHECK_NOT_NULL(tbl);
    CHECK(tbl->count > 0);
    CHECK_NOT_NULL(tbl->name);
    CHECK_STR_EQ("Wabun", tbl->name);
}

static void test_wabun_switch_and_decode() {
    // Switch to Wabun table, decode known pattern.
    MorseEncoder::setTable(&kWabunMorseTable);

    // イ (i) = ".-" — the table entry's chr is the 3-byte UTF-8
    // sequence \xe3\x82\xa4.
    const char* dec = MorseEncoder::charFromMorse(".-");
    CHECK_NOT_NULL(dec);
    CHECK(dec[0] != '\0');
    // First byte of UTF-8 must be 0xE3 (start of a 3-byte sequence).
    CHECK_EQ(0xE3, (unsigned char)dec[0]);

    // Switch back to International
    MorseEncoder::setTable(&kInternationalMorseTable);
}

static void test_wabun_round_trip_dit_dah_to_char() {
    // Switch to Wabun table
    MorseEncoder::setTable(&kWabunMorseTable);

    // Decode a few well-known Wabun patterns back to their UTF-8
    // katakana strings. This is the working "round trip" path —
    // morseFromChar(char) cannot accept multi-byte UTF-8 input, so
    // we go via charFromMorse on the table directly. See the file
    // header comment for the rationale.
    struct Sample { const char* ditDah; unsigned char firstByte; };
    const Sample samples[] = {
        {".-",     0xE3},  // イ i   .-
        {"--.--",  0xE3},  // ア a   --.--
        {"--",     0xE3},  // ヨ yo  --
        {"---",    0xE3},  // レ re  ---
        {"-.-",    0xE3},  // ワ wa  -.-
    };
    for (const auto& s : samples) {
        const char* chr = MorseEncoder::charFromMorse(s.ditDah);
        CHECK_NOT_NULL(chr);
        CHECK_EQ(s.firstByte, (unsigned char)chr[0]);
        // All Wabun kana are 3-byte UTF-8 starting with 0xE3.
        CHECK_EQ(0x80, (unsigned char)chr[1] & 0xC0);
        CHECK_EQ(0x80, (unsigned char)chr[2] & 0xC0);
        CHECK_EQ('\0', chr[3]);
    }

    // Switch back to International
    MorseEncoder::setTable(&kInternationalMorseTable);
}

static void test_wabun_international_lookup_unchanged() {
    // After Wabun table round trip, switching back to International
    // must still resolve ".-" to "a" (not to the Wabun イ).
    MorseEncoder::setTable(&kWabunMorseTable);
    MorseEncoder::setTable(&kInternationalMorseTable);

    const char* a = MorseEncoder::charFromMorse(".-");
    CHECK_NOT_NULL(a);
    CHECK_STR_EQ("a", a);
}

static void test_wabun_katakana_hiragana_conversion() {
    // Verify that the wabunKatakanaToHiragana function works
    // Katakana イ (U+30A4) → Hiragana い (U+3044)
    char buf[8] = {0};
    size_t n = wabunKatakanaToHiragana("\xe3\x82\xa4", buf, sizeof(buf));
    CHECK(n > 0);
    // Hiragana い is U+3044 = 0xE3 0x81 0x84
    CHECK_EQ(0xE3, (unsigned char)buf[0]);
    CHECK_EQ(0x81, (unsigned char)buf[1]);
    CHECK_EQ(0x84, (unsigned char)buf[2]);
    CHECK_EQ('\0',  buf[3]);
}

int main() {
    printf("=== test_wabun ===\n");
    RUN(test_wabun_table_is_available);
    RUN(test_wabun_switch_and_decode);
    RUN(test_wabun_round_trip_dit_dah_to_char);
    RUN(test_wabun_international_lookup_unchanged);
    RUN(test_wabun_katakana_hiragana_conversion);
    return test_summary();
}
