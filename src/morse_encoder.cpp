#include "morse_encoder.h"
#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------
const MorseTable* MorseEncoder::s_currentTable = &kInternationalMorseTable;
bool MorseEncoder::s_lookupBuilt = false;

// Lookup arrays for char→morse (sorted by character)
static const char* g_charSorted[128]   = { nullptr };  // sorted alphabetically: [i] = chr, g_morseSorted[i] = morse
static const char* g_morseForChar[128] = { nullptr };

// Lookup arrays for morse→char (sorted by dit-dah string)
static const char* g_morseSorted[128]  = { nullptr };  // sorted by dit-dah: [i] = morse, g_charForMorse[i] = chr
static const char* g_charForMorse[128]  = { nullptr };

static size_t g_entryCount = 0;

// ---------------------------------------------------------------------------
// MorseEncoder::buildSortedLookup
// ---------------------------------------------------------------------------
void MorseEncoder::buildSortedLookup() {
    if (s_lookupBuilt) return;

    const MorseTable* tbl = s_currentTable;
    g_entryCount = tbl->count;

    // Copy entries into temp arrays
    const char* tmpChar[128]  = { nullptr };
    const char* tmpMorse[128] = { nullptr };
    for (size_t i = 0; i < g_entryCount; ++i) {
        tmpChar[i]  = tbl->entries[i].chr;
        tmpMorse[i] = tbl->entries[i].ditDah;
    }

    // Sort by character: build g_charSorted / g_morseForChar
    for (size_t i = 0; i < g_entryCount; ++i) {
        for (size_t j = i + 1; j < g_entryCount; ++j) {
            if (strcmp(tmpChar[i], tmpChar[j]) > 0) {
                std::swap(tmpChar[i], tmpChar[j]);
                std::swap(tmpMorse[i], tmpMorse[j]);
            }
        }
    }
    for (size_t i = 0; i < g_entryCount; ++i) {
        g_charSorted[i]    = tmpChar[i];
        g_morseForChar[i]  = tmpMorse[i];
    }

    // Re-copy for morse sort: build g_morseSorted / g_charForMorse
    for (size_t i = 0; i < g_entryCount; ++i) {
        tmpChar[i]  = tbl->entries[i].chr;
        tmpMorse[i] = tbl->entries[i].ditDah;
    }
    // Sort by morse: swap both in parallel
    for (size_t i = 0; i < g_entryCount; ++i) {
        for (size_t j = i + 1; j < g_entryCount; ++j) {
            if (strcmp(tmpMorse[i], tmpMorse[j]) > 0) {
                std::swap(tmpMorse[i], tmpMorse[j]);
                std::swap(tmpChar[i], tmpChar[j]);
            }
        }
    }
    for (size_t i = 0; i < g_entryCount; ++i) {
        g_morseSorted[i]   = tmpMorse[i];
        g_charForMorse[i]  = tmpChar[i];
    }

    s_lookupBuilt = true;
}

// ---------------------------------------------------------------------------
// MorseEncoder::binarySearchChar — find morse code by character
// g_charSorted is sorted alphabetically; g_morseForChar is parallel
// ---------------------------------------------------------------------------
const char* MorseEncoder::binarySearchChar(char c) {
    buildSortedLookup();
    char tmp[2] = { static_cast<char>(::tolower(c)), 0 };

    int lo = 0;
    int hi = static_cast<int>(g_entryCount) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int cmp = strcmp(tmp, g_charSorted[mid]);
        if (cmp == 0) return g_morseForChar[mid];
        if (cmp < 0)  hi = mid - 1;
        else          lo = mid + 1;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// MorseEncoder::binarySearchMorse — find character by morse code
// g_morseSorted is sorted by dit-dah; g_charForMorse is parallel
// ---------------------------------------------------------------------------
const char* MorseEncoder::binarySearchMorse(const char* ditDah) {
    buildSortedLookup();
    int lo = 0;
    int hi = static_cast<int>(g_entryCount) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int cmp = strcmp(ditDah, g_morseSorted[mid]);
        if (cmp == 0) return g_charForMorse[mid];
        if (cmp < 0)  hi = mid - 1;
        else          lo = mid + 1;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// MorseEncoder::morseFromChar
// ---------------------------------------------------------------------------
const char* MorseEncoder::morseFromChar(char c) {
    return binarySearchChar(c);
}

// ---------------------------------------------------------------------------
// MorseEncoder::charFromMorse
// ---------------------------------------------------------------------------
const char* MorseEncoder::charFromMorse(const char* ditDah) {
    return binarySearchMorse(ditDah);
}

// ---------------------------------------------------------------------------
// MorseEncoder::canEncode
// ---------------------------------------------------------------------------
bool MorseEncoder::canEncode(char c) {
    return morseFromChar(c) != nullptr;
}

// ---------------------------------------------------------------------------
// MorseEncoder::setTable / currentTable
// ---------------------------------------------------------------------------
void MorseEncoder::setTable(const MorseTable* table) {
    if (table) {
        s_currentTable = table;
        s_lookupBuilt = false;  // force rebuild on next lookup
    }
}

const MorseTable* MorseEncoder::currentTable() {
    return s_currentTable;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
MorseEncoder::MorseEncoder(int wpm) : _wpm(wpm) {
    buildSortedLookup();
}

// ---------------------------------------------------------------------------
// Encode a text string to a vector of Elements
// ---------------------------------------------------------------------------
std::vector<MorseEncoder::Element> MorseEncoder::encode(const char* text) const {
    std::vector<Element> out;

    if (!text) return out;

    while (*text) {
        char c = *text;

        // Word space: ASCII space.
        // The trailing silence of the last element (1 unit) already provides
        // 1 unit of the standard 7-unit inter-word gap.  We add only the
        // remaining 6 units here.
        if (c == ' ') {
            out.emplace_back(Element::WORD_SPACE, 6, false);
            ++text;
            continue;
        }

        const char* code = morseFromChar(c);
        if (!code) {
            // Unknown character — skip
            ++text;
            continue;
        }

        // Encode each dit/dah
        while (*code) {
            char el = *code++;
            bool isDit = (el == '.');

            // The mark (dit or dah)
            out.emplace_back(
                isDit ? Element::DIT : Element::DAH,
                isDit ? 1 : 3,
                true   // key down (tone on)
            );

            // Intra-element pause (between dit/dah within same character).
            // KeyEnvelop envelopes already include trailing silence (1 unit) as the
            // intra-character gap — no extra ELEMENT_SPACE is needed.
            if (*code) {
                // out.emplace_back(Element::ELEMENT_SPACE, 1, false);
            }
        }

        // After a character: inter-character space (key up).
        // The trailing silence of the last element (1 unit) already provides
        // 1 unit of the standard 3-unit inter-character gap.  We add only the
        // remaining 2 units here.
        const char* next = text + 1;
        while (*next == ' ') ++next;   // skip any spaces to find real next char
        if (*next != '\0') {
            // There is a next character; insert a CHAR_SPACE unless the next
            // non-space char is separated from us by at least one space (word
            // space will be emitted by the outer loop; don't double-count).
            bool hasWordSpace = false;
            for (const char* p = text + 1; p < next; ++p) {
                if (*p == ' ') { hasWordSpace = true; break; }
            }
            if (!hasWordSpace) {
                out.emplace_back(Element::CHAR_SPACE, 2, false);
            }
        }

        ++text;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Debug print morse as dots/dashes
// ---------------------------------------------------------------------------
void MorseEncoder::debugPrint(const char* text) {
    if (!text) return;
    Serial.print("[morse] \"");
    Serial.print(text);
    Serial.print("\" -> \"");
    while (*text) {
        char c = *text;
        if (c == ' ') { Serial.print(" / "); ++text; continue; }
        const char* code = morseFromChar(c);
        if (code) Serial.print(code);
        ++text;
    }
    Serial.println("\"");
}