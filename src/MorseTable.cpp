/**
 * MorseTable.cpp — Morse code tables.
 *
 * Sources:
 *   - International: matching MorseEncoder::_morseMap + prosigns from extern/morse.h
 *   - Wabun: Japanese Government Radio Station Operations Regulations, Article 12,
 *     Attached Table No. 1, as cited in https://en.wikipedia.org/wiki/Wabun_code.
 *     Iroha ordering. Each entry stores a UTF-8 katakana glyph.
 *     See docs/wabun.md for the full chart.
 */

#include "MorseTable.h"

// ---------------------------------------------------------------------------
// International Morse Code (default)
// ---------------------------------------------------------------------------
static const MorseEntry kInternationalEntries[] = {
    {".-",   "a"}, {"-...", "b"}, {"-.-.", "c"}, {"-..",  "d"}, {".",    "e"},
    {"..-.", "f"}, {"--.",  "g"}, {"....", "h"}, {"..",   "i"}, {".---", "j"},
    {"-.-",  "k"}, {".-..", "l"}, {"--",   "m"}, {"-.",   "n"}, {"---",  "o"},
    {".--.", "p"}, {"--.-", "q"}, {".-.",  "r"}, {"...",  "s"}, {"-",    "t"},
    {"..-",  "u"}, {"...-", "v"}, {".--",  "w"}, {"-..-", "x"}, {"-.--", "y"},
    {"--..", "z"},
    {".----", "1"}, {"..---", "2"}, {"...--", "3"}, {"....-", "4"}, {".....", "5"},
    {"-....", "6"}, {"--...", "7"}, {"---..", "8"}, {"----.", "9"}, {"-----", "0"},
    {"--..--", ","}, {"..--..", "?"}, {".-.-.-", "."}, {"-...-",  "="},
    {"-..-.",  "/"}, {"-.-.--", "!"},
    {"-.-.-",  "<ka>"},  {"...-.-", "<sk>"},  {".-.-.",  "<ar>"},
    {"-.--.",  "<kn>"},  {"........", "<error>"},
};

const MorseTable kInternationalMorseTable = {
    "International",
    kInternationalEntries,
    sizeof(kInternationalEntries) / sizeof(kInternationalEntries[0])
};

// ---------------------------------------------------------------------------
// Wabun code (Japanese kana) — 和文モールス符号
//
// Iroha order. Dit-dah strings from the Japanese Government Radio Station
// Operations Regulations, converted per: ▄ = '.' (dit), ▄▄▄ = '-' (dah).
// Each entry has a unique dit-dah length; exact-match strcmp lookup
// resolves any ambiguities.
//
// Dakuten (゛ = ..) and handakuten (゜ = ..--.) are sent as separate
// characters after the base kana. chōonpu (ー = .--.-) and punctuation
// are separate entries.
// ---------------------------------------------------------------------------
static const MorseEntry kWabunEntries[] = {
    // ── Monographs (Iroha order) ──────────────────────────────────────────
    {".-",     "\xe3\x82\xa4"},   // イ i     .-
    {".-.-",   "\xe3\x83\xad"},   // ロ ro    .-.-
    {"-...",   "\xe3\x83\x8f"},   // ハ ha    -...
    {"-.-.",   "\xe3\x83\x8b"},   // ニ ni    -.-.
    {"-..",    "\xe3\x83\x9b"},   // ホ ho    -..
    {".",      "\xe3\x83\x98"},   // ヘ he    .
    {"..-..",  "\xe3\x83\x88"},   // ト to    ..-..
    {"..-.",   "\xe3\x83\x81"},   // チ chi   ..-.
    {"--.",    "\xe3\x83\xaa"},   // リ ri    --.
    {"....",   "\xe3\x83\x8c"},   // ヌ nu    ....
    {"-.--.",  "\xe3\x83\xab"},   // ル ru    -.--.
    {".---",   "\xe3\x83\xb2"},   // ヲ wo    .---
    {"-.-",    "\xe3\x83\xaf"},   // ワ wa    -.-
    {".-..",   "\xe3\x82\xab"},   // カ ka    .-..
    {"--",     "\xe3\x83\xa8"},   // ヨ yo    --
    {"-.",     "\xe3\x82\xbf"},   // タ ta    -.
    {"---",    "\xe3\x83\xac"},   // レ re    ---
    {"---.",   "\xe3\x82\xbd"},   // ソ so    ---.
    {".--.",   "\xe3\x83\x84"},   // ツ tsu   .--.
    {"--.-",   "\xe3\x83\x8d"},   // ネ ne    --.-
    {".-.",    "\xe3\x83\x8a"},   // ナ na    .-.
    {"...",    "\xe3\x83\xa9"},   // ラ ra    ...
    {"-",      "\xe3\x83\xa0"},   // ム mu    -
    {"..-",    "\xe3\x82\xa6"},   // ウ u     ..-
    {".-..-",  "\xe3\x83\xb0"},   // ヰ wi    .-..-
    {"..--",   "\xe3\x83\x8e"},   // ノ no    ..--
    {".-...",  "\xe3\x82\xaa"},   // オ o     .-...
    {"...-",   "\xe3\x82\xaf"},   // ク ku    ...-
    {".--",    "\xe3\x83\xa4"},   // ヤ ya    .--
    {"-..-",   "\xe3\x83\x9e"},   // マ ma    -..-
    {"-.--",   "\xe3\x82\xb1"},   // ケ ke    -.--  (dah-dit-dah-dah)
    {"--..",   "\xe3\x83\x95"},   // フ fu    --..
    {"----",   "\xe3\x82\xb3"},   // コ ko    ----
    {"-.---",  "\xe3\x82\xa8"},   // エ e     -.--- (dah-dit-dah-dah-dah)
    {".-.--",  "\xe3\x83\x86"},   // テ te    .-.-- (dit-dah-dit-dah-dah)
    {"--.--",  "\xe3\x82\xa2"},   // ア a     --.-- (dah-dah-dit-dah-dah)
    {"-.-.-",  "\xe3\x82\xb5"},   // サ sa    -.-.-
    {"-.-..",  "\xe3\x82\xad"},   // キ ki    -.-..
    {"-..--",  "\xe3\x83\xa6"},   // ユ yu    -..--
    {"-...-",  "\xe3\x83\xa1"},   // メ me    -...-
    {"..-.-",  "\xe3\x83\x9f"},   // ミ mi    ..-.-
    {"--.-.",  "\xe3\x82\xb7"},   // シ shi   --.-.
    {".--..",  "\xe3\x83\xb1"},   // ヱ we    .--..
    {"--..-",  "\xe3\x83\x92"},   // ヒ hi    --..-
    {"-..-.",  "\xe3\x83\xa2"},   // モ mo    -..-.
    {".---.",  "\xe3\x82\xbb"},   // セ se    .---.
    {"---.-",  "\xe3\x82\xb9"},   // ス su    ---.-
    {".-.-.",  "\xe3\x83\xb3"},   // ン n     .-.-.

    // ── Small kana for digraphs ────────────────────────────────────────────
    {".--",    "\xe3\x83\xa3"},   // ャ ya (small) .--
    {"-..--",  "\xe3\x83\xa5"},   // ュ yu (small) -..--
    {"--",     "\xe3\x83\xa7"},   // ョ yo (small) --

    // ── Dakuten (゛) and handakuten (゜) modifiers ─────────────────────────
    {"..",     "\xe3\x82\x9b"},   // ゛ dakuten    ..
    {"..--.",  "\xe3\x82\x9c"},   // ゜ handakuten ..--.

    // ── Chōonpu (long-vowel mark) ─────────────────────────────────────────
    {".--.-",  "\xe3\x83\xbc"},   // ー chōonpu   .--.-

    // ── Punctuation ───────────────────────────────────────────────────────
    {".-.-.-", "\xe3\x80\x81"},   // 、 comma     .-.-.-
    {".-.-..", "\xe3\x80\x82"},   // 。 full stop .-.-..
    {"-.--.-", "("},              // (  left paren -.--.-
    {".-..-.", ")"},              // )  right paren .-..-.
};

const MorseTable kWabunMorseTable = {
    "Wabun",
    kWabunEntries,
    sizeof(kWabunEntries) / sizeof(kWabunEntries[0])
};

// ---------------------------------------------------------------------------
// Stubs for future tables
// ---------------------------------------------------------------------------
#if 0
const MorseTable kAmericanMorseTable = { "American", nullptr, 0 };
#endif

// ---------------------------------------------------------------------------
// Wabun Katakana → Hiragana conversion
// Katakana U+30A1..U+30F6 → Hiragana U+3041..U+3096 (subtract 0x60).
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstring>

size_t wabunKatakanaToHiragana(const char* utf8, char* out, size_t cap) {
    if (!utf8 || !*utf8) { if (out && cap > 0) out[0] = '\0'; return 0; }
    unsigned char c0 = (unsigned char)utf8[0];
    if (c0 < 0x80) {
        if (out && cap >= 2) { out[0] = (char)c0; out[1] = '\0'; }
        return 1;
    }
    if ((c0 & 0xE0) != 0xE0) {
        if (out && cap >= 2) { out[0] = (char)c0; out[1] = '\0'; }
        return 1;
    }
    uint32_t cp = ((uint32_t)(c0 & 0x0F)) << 12
                | ((uint32_t)((unsigned char)utf8[1] & 0x3F)) << 6
                | ((uint32_t)((unsigned char)utf8[2] & 0x3F));
    if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;
    if (out && cap >= 4) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0';
    }
    return 3;
}
