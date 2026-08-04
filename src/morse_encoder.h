#pragma once
/**
 * MorseEncoder — converts text strings to morse code (DIT/DAH/ELEMENT_SPACE/CHAR_SPACE/WORD_SPACE).
 *
 * Usage:
 *   MorseEncoder enc(20);          // 20 WPM
 *   auto seq = enc.encode("CQ");   // returns vector of Element
 *   for (auto el : seq) { ... }
 *
 * WPM can be changed at any time; it only affects timing (sample counts)
 * when used together with KeyEnvelop.
 *
 * Morse table: MorseEncoder uses the current MorseTable (default: International).
 * The table can be switched via setTable() for alternative codes (Wabun, American).
 */

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include "MorseTable.h"

class MorseEncoder {
public:
    /** A single morse element with its duration in morse units. */
    struct Element {
        enum Type : int8_t {
            // IMPORTANT: values must be UNIQUE. The chunk-boundary
            // detection in MorseGenerator::advanceToNextElement compares
            // _elements.back().type against CHAR_SPACE / WORD_SPACE to
            // decide whether the chunk ended with silence. If the mark
            // types (DIT/DAH) shared a numeric value with the silence
            // types (ELEMENT_SPACE/CHAR_SPACE), any DAH would falsely
            // register as a CHAR_SPACE and the next playText() would
            // skip its leading-silence prepend. See docs/winkey.md
            // §13.6 for the symptom.
            DIT          = 1,   ///< 1-unit mark (key-down)
            DAH          = 3,   ///< 3-unit mark (key-down)
            ELEMENT_SPACE = 2,  ///< 1-unit intra-character space (key-up)
            CHAR_SPACE   = 4,  ///< 3-unit inter-character space (key-up)
            WORD_SPACE   = 7,  ///< 7-unit inter-word space (key-up)
        };
        Type  type;
        int   units;    // morse units (1 = dit, 3 = dah)
        bool  keyDown;  // true = tone on, false = silence

        Element(Type t, int u, bool kd) : type(t), units(u), keyDown(kd) {}
    };

    /**
     * @param wpm  Initial words-per-minute (Paris standard).
     */
    explicit MorseEncoder(int wpm = 20);

    /** Change WPM (affects sample counts returned by timing methods). */
    void setWPM(int wpm) { _wpm = wpm; }
    int  wpm() const { return _wpm; }

    /**
     * Encode a NUL-terminated string to a vector of Elements.
     * @param text  Input text (ASCII / Latin-1).  Case-insensitive.
     * @return Vector of Elements in playback order.
     */
    std::vector<Element> encode(const char* text) const;

    /**
     * Encode a std::string.
     */
    std::vector<Element> encode(const std::string& text) const {
        return encode(text.c_str());
    }

    /**
     * @return Duration of @p units in seconds.
     */
    float unitsToSec(int units) const {
        return units * 1.2f / static_cast<float>(_wpm);
    }

    /**
     * @return Duration of @p units in samples at the given sample rate.
     */
    int unitsToSamples(int units, int sampleRate) const {
        return static_cast<int>(std::round(sampleRate * unitsToSec(units)));
    }

    /** @return Dit length in seconds. */
    float ditLengthSec() const { return 1.2f / static_cast<float>(_wpm); }

    /** @return Dit length in samples at @p sampleRate. */
    int ditLengthSamples(int sampleRate) const {
        return unitsToSamples(1, sampleRate);
    }

    // ----- Static lookup tables (可以直接在其他文件里使用) -----
    /** Given a plain ASCII character, return its morse code string (e.g. ".-", "-...").
     *  Returns nullptr if character has no morse representation. */
    static const char* morseFromChar(char c);

    /** Given a dit-dah string, return the character(s) it represents.
     *  Returns nullptr if not found in the current table. */
    static const char* charFromMorse(const char* ditDah);

    /** Return true if @p c is a printable character that can be encoded. */
    static bool canEncode(char c);

    /** Print the morse representation of @p text to Serial (for debugging). */
    static void debugPrint(const char* text);

    /** Switch to a different morse table (future: Wabun, American Morse). */
    static void setTable(const MorseTable* table);

    /** Get the currently active morse table. */
    static const MorseTable* currentTable();

private:
    int _wpm;

    static const MorseTable* s_currentTable;
    static bool s_lookupBuilt;
    static void buildSortedLookup();
    static const char* binarySearchChar(char c);
    static const char* binarySearchMorse(const char* ditDah);
};