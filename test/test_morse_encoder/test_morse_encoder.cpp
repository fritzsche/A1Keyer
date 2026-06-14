#include "test_framework.h"
#include "morse_encoder.h"

// --- single-character encoding ---

static void test_encode_E_is_single_dit() {
    MorseEncoder enc(20);
    auto seq = enc.encode("E");
    CHECK_EQ(1, (int)seq.size());
    CHECK_EQ((int)MorseEncoder::Element::DIT, (int)seq[0].type);
    CHECK_EQ(1, seq[0].units);
    CHECK(seq[0].keyDown);
}

static void test_encode_T_is_single_dah() {
    MorseEncoder enc(20);
    auto seq = enc.encode("T");
    CHECK_EQ(1, (int)seq.size());
    CHECK_EQ((int)MorseEncoder::Element::DAH, (int)seq[0].type);
    CHECK_EQ(3, seq[0].units);
    CHECK(seq[0].keyDown);
}

static void test_encode_A_has_no_intra_element_space() {
    // A = ".-" → DIT, DAH  (no ELEMENT_SPACE — trailing silence of DIT
    // envelope IS the intra-character gap; DAH follows immediately).
    MorseEncoder enc(20);
    auto seq = enc.encode("A");
    CHECK_EQ(2, (int)seq.size());
    CHECK(seq[0].keyDown);
    CHECK_EQ((int)MorseEncoder::Element::DIT, (int)seq[0].type);
    CHECK(seq[1].keyDown);
    CHECK_EQ((int)MorseEncoder::Element::DAH, (int)seq[1].type);
}

static void test_encode_two_chars_separated_by_char_space() {
    // ET → DIT, CHAR_SPACE(2 units), DAH.
    // The trailing silence of DIT (1 unit) + CHAR_SPACE (2 units) = 3-unit inter-char gap.
    MorseEncoder enc(20);
    auto seq = enc.encode("ET");
    CHECK_EQ(3, (int)seq.size());
    CHECK(seq[0].keyDown);
    CHECK_EQ((int)MorseEncoder::Element::CHAR_SPACE, (int)seq[1].type);
    CHECK_EQ(2, seq[1].units);
    CHECK(!seq[1].keyDown);
    CHECK(seq[2].keyDown);
}

static void test_encode_word_space_between_words() {
    // "E T": E ends, then word space separates words, then T begins.
    // The word space (6 units) is the *additional* inter-word silence after
    // the 1 unit already provided by the trailing silence of E's DIT envelope.
    // Sequence: DIT, WORD_SPACE, DAH.
    MorseEncoder enc(20);
    auto seq = enc.encode("E T");
    CHECK_EQ(3, (int)seq.size());
    CHECK_EQ((int)MorseEncoder::Element::DIT,        (int)seq[0].type);
    CHECK_EQ((int)MorseEncoder::Element::WORD_SPACE, (int)seq[1].type);
    CHECK_EQ(6, seq[1].units);
    CHECK(!seq[1].keyDown);
    CHECK_EQ((int)MorseEncoder::Element::DAH,        (int)seq[2].type);
}

static void test_encode_case_insensitive() {
    MorseEncoder enc(20);
    auto upper = enc.encode("A");
    auto lower = enc.encode("a");
    CHECK_EQ((int)upper.size(), (int)lower.size());
    for (size_t i = 0; i < upper.size(); ++i) {
        CHECK_EQ((int)upper[i].type, (int)lower[i].type);
        CHECK_EQ(upper[i].keyDown, lower[i].keyDown);
    }
}

static void test_encode_unknown_char_skipped() {
    MorseEncoder enc(20);
    CHECK_EQ(0, (int)enc.encode("@").size());
}

static void test_encode_empty_string_returns_empty() {
    MorseEncoder enc(20);
    CHECK_EQ(0, (int)enc.encode("").size());
}

static void test_encode_null_returns_empty() {
    MorseEncoder enc(20);
    CHECK_EQ(0, (int)enc.encode((const char*)nullptr).size());
}

// --- SOS key-down count ---

static void test_encode_sos_has_nine_marks() {
    // S = ..., O = ---, S = ...  →  3 dits + 3 dahs + 3 dits = 9 key-down elements
    MorseEncoder enc(20);
    auto seq = enc.encode("SOS");
    int keyDownCount = 0;
    for (auto& el : seq)
        if (el.keyDown) ++keyDownCount;
    CHECK_EQ(9, keyDownCount);
}

// --- static helpers ---

static void test_morse_from_char_A_is_dot_dash() {
    const char* code = MorseEncoder::morseFromChar('A');
    CHECK_NOT_NULL(code);
    CHECK_STR_EQ(".-", code);
}

static void test_morse_from_char_0_is_five_dahs() {
    const char* code = MorseEncoder::morseFromChar('0');
    CHECK_NOT_NULL(code);
    CHECK_STR_EQ("-----", code);
}

static void test_can_encode_letters_and_digits() {
    CHECK(MorseEncoder::canEncode('A'));
    CHECK(MorseEncoder::canEncode('z'));
    CHECK(MorseEncoder::canEncode('5'));
}

static void test_cannot_encode_at_sign() {
    CHECK(!MorseEncoder::canEncode('@'));
}

// --- timing helpers ---

static void test_units_to_sec_at_20_wpm() {
    MorseEncoder enc(20);
    // 1 dit = 1.2 / 20 = 0.060 s
    CHECK_NEAR(0.060f, enc.unitsToSec(1), 0.001f);
    // 3 units (dah) = 0.180 s
    CHECK_NEAR(0.180f, enc.unitsToSec(3), 0.001f);
}

static void test_dit_length_samples_at_20wpm_48k() {
    MorseEncoder enc(20);
    // 0.060 s * 48000 Hz = 2880 samples
    CHECK_EQ(2880, enc.ditLengthSamples(48000));
}

static void test_dit_length_halves_at_double_wpm() {
    MorseEncoder enc(20);
    int s20 = enc.ditLengthSamples(48000);
    enc.setWPM(40);
    CHECK_EQ(s20 / 2, enc.ditLengthSamples(48000));
}

int main() {
    printf("=== test_morse_encoder ===\n");
    RUN(test_encode_E_is_single_dit);
    RUN(test_encode_T_is_single_dah);
    RUN(test_encode_A_has_no_intra_element_space);
    RUN(test_encode_two_chars_separated_by_char_space);
    RUN(test_encode_word_space_between_words);
    RUN(test_encode_case_insensitive);
    RUN(test_encode_unknown_char_skipped);
    RUN(test_encode_empty_string_returns_empty);
    RUN(test_encode_null_returns_empty);
    RUN(test_encode_sos_has_nine_marks);
    RUN(test_morse_from_char_A_is_dot_dash);
    RUN(test_morse_from_char_0_is_five_dahs);
    RUN(test_can_encode_letters_and_digits);
    RUN(test_cannot_encode_at_sign);
    RUN(test_units_to_sec_at_20_wpm);
    RUN(test_dit_length_samples_at_20wpm_48k);
    RUN(test_dit_length_halves_at_double_wpm);
    return test_summary();
}
