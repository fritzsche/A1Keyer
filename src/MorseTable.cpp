/**
 * MorseTable.cpp — International Morse Code table.
 *
 * Sources:
 *   - Letters, numbers, punctuation: matching MorseEncoder::_morseMap
 *   - Prosigns: from extern/morse.h
 */

#include "MorseTable.h"

static const MorseEntry kEntries[] = {
    // alpha
    {".-",   "a"}, {"-...", "b"}, {"-.-.", "c"}, {"-..",  "d"}, {".",    "e"},
    {"..-.", "f"}, {"--.",  "g"}, {"....", "h"}, {"..",   "i"}, {".---", "j"},
    {"-.-",  "k"}, {".-..", "l"}, {"--",   "m"}, {"-.",   "n"}, {"---",  "o"},
    {".--.", "p"}, {"--.-", "q"}, {".-.",  "r"}, {"...",  "s"}, {"-",    "t"},
    {"..-",  "u"}, {"...-", "v"}, {".--",  "w"}, {"-..-", "x"}, {"-.--", "y"},
    {"--..", "z"},
    // numbers
    {".----", "1"}, {"..---", "2"}, {"...--", "3"}, {"....-", "4"}, {".....", "5"},
    {"-....", "6"}, {"--...", "7"}, {"---..", "8"}, {"----.", "9"}, {"-----", "0"},
    // punctuation
    {"--..--", ","}, {"..--..", "?"}, {".-.-.-", "."}, {"-...-",  "="},
    {"-..-.",  "/"}, {"-.-.--", "!"},
    // prosigns
    {"-.-.-",  "<ka>"},  // Message begins / Start of work
    {"...-.-", "<sk>"},  // End of contact / End of work
    {".-.-.",  "<ar>"},  // End of transmission / End of message
    {"-.--.",  "<kn>"},  // Go ahead, specific named station
    {"........", "<error>"}, // Error
};

const MorseTable kInternationalMorseTable = {
    "International",
    kEntries,
    sizeof(kEntries) / sizeof(kEntries[0])
};

// Stubs for future tables — not yet implemented
#if 0
const MorseTable kWabunMorseTable = {
    "Wabun",
    nullptr,
    0
};

const MorseTable kAmericanMorseTable = {
    "American",
    nullptr,
    0
};
#endif
