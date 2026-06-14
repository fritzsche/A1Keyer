#pragma once
/**
 * MorseTable.h — Shared morse code table infrastructure.
 *
 * Defines the MorseEntry and MorseTable types used by both MorseEncoder
 * (character → morse) and MorseDecoder (morse → character).
 *
 * Tables:
 *   kInternationalMorseTable — default, covers A-Z, 0-9, punctuation, prosigns
 *   kWabunMorseTable        — Japanese kana (not yet implemented)
 *   kAmericanMorseTable     — American Morse (not yet implemented)
 */

#include <cstddef>

struct MorseEntry {
    /** Dit-dah representation, e.g. ".-", "-...", etc. */
    const char* ditDah;
    /** Character(s) this represents, e.g. "a", "1", "<ka>", "ä" */
    const char* chr;
};

struct MorseTable {
    /** Human-readable name, e.g. "International", "Wabun", "American" */
    const char* name;
    /** Array of entries */
    const MorseEntry* entries;
    /** Number of entries */
    size_t count;
};

/** International Morse Code (default table). */
extern const MorseTable kInternationalMorseTable;

/** Wabun Morse (Japanese kana) — not yet implemented. */
extern const MorseTable kWabunMorseTable;

/** American Morse — not yet implemented. */
extern const MorseTable kAmericanMorseTable;
