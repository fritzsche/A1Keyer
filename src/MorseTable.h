#pragma once
/**
 * MorseTable.h — Shared morse code table infrastructure.
 *
 * Defines the MorseEntry and MorseTable types used by both MorseEncoder
 * (character → morse) and MorseDecoder (morse → character).
 *
 * Tables:
 *   kInternationalMorseTable — default, covers A-Z, 0-9, punctuation, prosigns
 *   kWabunMorseTable         — Japanese kana (Wabun code, 和文モールス)
 */

#include <cstddef>

enum class MorseTableMode : int {
    INTERNATIONAL   = 0,
    WABUN_KATAKANA  = 1,
    WABUN_HIRAGANA  = 2,
};

struct MorseEntry {
    const char* ditDah;
    const char* chr;
};

struct MorseTable {
    const char* name;
    const MorseEntry* entries;
    size_t count;
};

extern const MorseTable kInternationalMorseTable;

extern const MorseTable kWabunMorseTable;

extern const MorseTable kAmericanMorseTable;

size_t wabunKatakanaToHiragana(const char* utf8, char* out, size_t cap);
