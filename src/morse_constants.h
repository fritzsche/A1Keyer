#pragma once
/**
 * morse_constants.h — Shared constants for the CW codec.
 *
 * These constants define the timing and symbol alphabet used throughout
 * the morse encoding/decoding pipeline:
 *
 *   IambicKeyer  → writes '.', '-', '*', ' ' to its decoder ring buffer
 *   MorseDecoder ← reads  '.', '-', '*', ' ' from the ring buffer
 *   MorseEncoder → produces DIT (=1 unit), DAH (=3 units), CHAR_SPACE, WORD_SPACE
 *
 * Timing is based on the Paris standard: 1 WPM = 1 dit/sec = 1.2 seconds per dit.
 */

#include <cstddef>

struct MorseConstants {
    // --- Decoder symbols (written by IambicKeyer, read by MorseDecoder) ---

    /** Written when a character ends (neither paddle memory set). */
    static constexpr char END_OF_CHAR = '*';

    /** Written when a word gap exceeds WORD_SPACE_DITS dit-lengths. */
    static constexpr char SPACE_CHAR = ' ';

    // --- Morse element symbols (used internally and in morse_encoder output) ---

    /** Symbol for a DIT element. */
    static constexpr char DIT_SYMBOL = '.';

    /** Symbol for a DAH element. */
    static constexpr char DAH_SYMBOL = '-';

    // --- Timing in morse units (1 unit = 1 dit duration) ---

    /** Duration of a DIT mark in morse units. */
    static constexpr int DIT_UNITS = 1;

    /** Duration of a DAH mark in morse units. */
    static constexpr int DAH_UNITS = 3;

    /** Intra-character space between dit/dah within a character (1 unit).
     *  Note: KeyEnvelop envelopes already include 1 unit of trailing silence,
     *  so no extra silence is needed between consecutive elements. */
    static constexpr int ELEMENT_SPACE_UNITS = 1;

    /** Inter-character space between characters (3 units total).
     *  The last element's envelope provides 1 unit; this constant represents
     *  the remaining 2 units that MorseEncoder adds. */
    static constexpr int CHAR_SPACE_UNITS = 3;

    /** Inter-word space between words (7 units). */
    static constexpr int WORD_SPACE_UNITS = 7;

    // --- Envelope size multipliers (relative to ditLengthSamples) ---

    /** DIT envelope size = 2 × ditLen samples.
     *  Layout: [tone(ditLen) | silence(ditLen)]
     *  Cmorse-compatible: tone + trailing silence provides intra-character gap. */
    static constexpr int DIT_ENV_MULTIPLIER = 2;

    /** DAH envelope size = 4 × ditLen samples.
     *  Layout: [tone(3×ditLen) | silence(3×ditLen)]
     *  Cmorse-compatible: tone + trailing silence. */
    static constexpr int DAH_ENV_MULTIPLIER = 4;

    // --- Word-space gap threshold ---

    /** Word-space detection threshold: gap must exceed this many dit-lengths. */
    static constexpr size_t WORD_SPACE_DITS = 7;
};