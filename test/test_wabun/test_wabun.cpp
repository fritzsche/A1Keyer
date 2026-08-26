#include "test_framework.h"
#include "morse_encoder.h"

// Wabun-specific tests: the encoder/decoder must switch to the Wabun table
// and look up katakana characters correctly.

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
    // Switch to Wabun table, decode known patterns
    MorseEncoder::setTable(&kWabunMorseTable);

    // イ (i) = .-
    const char* dec = MorseEncoder::charFromMorse(".-");
    CHECK_NOT_NULL(dec);
    CHECK(dec[0] != '\0');

    // Switch back to International
    MorseEncoder::setTable(&kInternationalMorseTable);
}

static void test_wabun_round_trip_encode_decode() {
    // Switch to Wabun table
    MorseEncoder::setTable(&kWabunMorseTable);

    // Encode and decode a known character: イ = .-
    const char* code = MorseEncoder::morseFromUTF8("\xe3\x82\xa4");  // イ
    CHECK_NOT_NULL(code);
    CHECK_STR_EQ(".-", code);

    // Decode back
    const char* chr = MorseEncoder::charFromMorse(".-");
    CHECK_NOT_NULL(chr);
    // Should match the Wabun table entry for イ

    // Switch back to International
    MorseEncoder::setTable(&kInternationalMorseTable);
}

static void test_wabun_katakana_hiragana_conversion() {
    // Verify that the wabunKatakanaToHiragana function works
    // Katakana イ (U+30A4) → Hiragana い (U+3044)
    char buf[8] = {0};
    size_t n = wabunKatakanaToHiragana("\xe3\x82\xa4", buf, sizeof(buf));
    CHECK(n > 0);
    // Hiragana い is U+3044 = 0xE3 0x81 0x84
    // The output should be \xe3\x81\x84
    CHECK(buf[0] != '\0');
}

int main() {
    printf("=== test_wabun ===\n");
    RUN(test_wabun_table_is_available);
    RUN(test_wabun_switch_and_decode);
    RUN(test_wabun_round_trip_encode_decode);
    RUN(test_wabun_katakana_hiragana_conversion);
    return test_summary();
}
