#include "test_framework.h"
#include "morse_encoder.h"
#include "morse_decoder.h"
#include "iambic_keyer.h"
#include "key_envelop.h"
#include <vector>
#include <cstdint>

// --- charFromMorse prosign lookups (decoder uses this) ---

static void test_decode_ka_prosign() {
    // KA = "-.-.-" → "<ka>"
    const char* result = MorseEncoder::charFromMorse("-.-.-");
    CHECK_NOT_NULL(result);
    CHECK_STR_EQ("<ka>", result);
}

static void test_decode_ar_prosign() {
    // AR = ".-.-." → "<ar>"
    const char* result = MorseEncoder::charFromMorse(".-.-.");
    CHECK_NOT_NULL(result);
    CHECK_STR_EQ("<ar>", result);
}

static void test_decode_sk_prosign() {
    // SK = "...-.-" → "<sk>"
    const char* result = MorseEncoder::charFromMorse("...-.-");
    CHECK_NOT_NULL(result);
    CHECK_STR_EQ("<sk>", result);
}

static void test_decode_kn_prosign() {
    // KN = "-.--." → "<kn>"
    const char* result = MorseEncoder::charFromMorse("-.--.");
    CHECK_NOT_NULL(result);
    CHECK_STR_EQ("<kn>", result);
}

static void test_decode_error_prosign() {
    // error = "........" → "<error>"
    const char* result = MorseEncoder::charFromMorse("........");
    CHECK_NOT_NULL(result);
    CHECK_STR_EQ("<error>", result);
}

static void test_decode_standard_letters_still_work() {
    CHECK_STR_EQ("a",  MorseEncoder::charFromMorse(".-"));
    CHECK_STR_EQ("b",  MorseEncoder::charFromMorse("-..."));
    CHECK_STR_EQ("i",  MorseEncoder::charFromMorse(".."));
    CHECK_STR_EQ("o",  MorseEncoder::charFromMorse("---"));
    CHECK_STR_EQ("e",  MorseEncoder::charFromMorse("."));
}

static void test_decode_unknown_morse_returns_null() {
    CHECK(MorseEncoder::charFromMorse("...--.-.-..") == nullptr);
}

// --- Verify prosign decoded result has multiple characters ---

static void test_prosign_result_expands_to_multiple_chars() {
    // Verify that charFromMorse for a prosign returns multiple characters.
    // "<ka>" has 4 chars: '<', 'k', 'a', '>'.
    const char* decoded = MorseEncoder::charFromMorse("-.-.-");
    CHECK_NOT_NULL(decoded);
    int count = 0;
    for (const char* p = decoded; *p; ++p) count++;
    CHECK_EQ(4, count);  // "<ka>" has 4 characters
}

static void test_prosign_ar_expands_to_multiple_chars() {
    const char* decoded = MorseEncoder::charFromMorse(".-.-.");
    CHECK_NOT_NULL(decoded);
    int count = 0;
    for (const char* p = decoded; *p; ++p) count++;
    CHECK_EQ(4, count);  // "<ar>" has 4 characters
}

// --- IambicKeyer decode ring buffer ---

static void test_decoder_read_empty_returns_false() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);

    char c = '?';
    CHECK(!keyer.decoderRead(&c));
    CHECK(c == '?');
}

static void test_decoder_available_starts_at_zero() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    CHECK(keyer.decoderAvailable() == 0);
}

static void test_decoder_read_after_manual_write() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);

    s_keyState.memory[IambicKeyer::DIT].store(1, std::memory_order_relaxed);
    s_keyState.state[IambicKeyer::DIT].store(0, std::memory_order_relaxed);

    int ditLen = env.envelopeSize(KeyEnvelop::Element::DIT);
    std::vector<int16_t> buf(ditLen, 0);
    keyer.fillSamples(buf.data(), ditLen, 500.0f, 16384, 48000);

    CHECK(keyer.decoderAvailable() >= 2);

    char c;
    CHECK(keyer.decoderRead(&c));
    CHECK(c == '.');
    CHECK(keyer.decoderRead(&c));
    CHECK(c == '*');
}

// ─── Decoded-char hook (docs/winkey.md § 16.8) ────────────────────────
//
// MorseDecoder fires a DecodedCharFn once per character produced by
// flush() (including the SPACE_CHAR for word gaps). The hook is the
// integration point for piping decoded paddle text into the WK
// bridge so RUMlogNG / N1MM log the operator's manual keying.

struct HookCapture {
    std::vector<char> chars;
    void*             ctxSeen = nullptr;
};

static void hookCapture(char c, void* ctx) {
    auto* h = static_cast<HookCapture*>(ctx);
    h->chars.push_back(c);
}

static void test_hook_fires_per_decoded_char() {
    HookCapture cap;
    MorseDecoder::setDecodedCharHook(&hookCapture, &cap);
    char buf[16];
    size_t pos = 0;
    // C = -.-. → MorseEncoder::charFromMorse returns "c" (lowercase)
    // The decoder passes the raw decoded string to the hook verbatim;
    // the bridge's emitDecodedChar() does the uppercasing.
    MorseDecoder::accumulate('-', buf, sizeof(buf), &pos);
    MorseDecoder::accumulate('.', buf, sizeof(buf), &pos);
    MorseDecoder::accumulate('-', buf, sizeof(buf), &pos);
    MorseDecoder::accumulate('.', buf, sizeof(buf), &pos);
    MorseDecoder::accumulate(MorseDecoder::END_OF_CHAR, buf, sizeof(buf), &pos);
    CHECK_EQ((int)cap.chars.size(), 1);
    CHECK_EQ((int)cap.chars[0],     (int)'c');
    MorseDecoder::setDecodedCharHook(nullptr, nullptr);
}

static void test_hook_fires_per_prosign_char() {
    // AR = ".-.-." decodes to "<ar>" (4 chars: <, a, r, >). The hook
    // fires once per display character so the host receives the same
    // sequence it would for a typed prosign.
    HookCapture cap;
    MorseDecoder::setDecodedCharHook(&hookCapture, &cap);
    char buf[16];
    size_t pos = 0;
    MorseDecoder::accumulate('.',  buf, sizeof(buf), &pos);
    MorseDecoder::accumulate('-',  buf, sizeof(buf), &pos);
    MorseDecoder::accumulate('.',  buf, sizeof(buf), &pos);
    MorseDecoder::accumulate('-',  buf, sizeof(buf), &pos);
    MorseDecoder::accumulate('.',  buf, sizeof(buf), &pos);
    MorseDecoder::accumulate(MorseDecoder::END_OF_CHAR, buf, sizeof(buf), &pos);
    CHECK_EQ((int)cap.chars.size(), 4);
    CHECK_EQ((int)cap.chars[0], (int)'<');
    CHECK_EQ((int)cap.chars[1], (int)'a');
    CHECK_EQ((int)cap.chars[2], (int)'r');
    CHECK_EQ((int)cap.chars[3], (int)'>');
    // Note: the prosign string "<ar>" itself is the raw morse-decoder
    // output. The bridge (WinkeyBridge::emitDecodedChar) handles any
    // passthrough formatting — prosigns in particular are emitted as
    // the literal "<ar>" characters; the host logger interprets them
    // as a single prosign.
    MorseDecoder::setDecodedCharHook(nullptr, nullptr);
}

static void test_hook_fires_for_space_char() {
    HookCapture cap;
    MorseDecoder::setDecodedCharHook(&hookCapture, &cap);
    char buf[16];
    size_t pos = 0;
    MorseDecoder::accumulate(MorseDecoder::SPACE_CHAR, buf, sizeof(buf), &pos);
    CHECK_EQ((int)cap.chars.size(), 1);
    CHECK_EQ((int)cap.chars[0], (int)' ');
    MorseDecoder::setDecodedCharHook(nullptr, nullptr);
}

static void test_hook_silent_when_uninstalled() {
    // The default state — no hook installed. accumulate/flush must
    // not crash; the original Log::write + appendDecodedChar paths
    // remain the only side effects.
    MorseDecoder::setDecodedCharHook(nullptr, nullptr);
    char buf[16];
    size_t pos = 0;
    MorseDecoder::accumulate('.', buf, sizeof(buf), &pos);
    MorseDecoder::accumulate('-', buf, sizeof(buf), &pos);
    MorseDecoder::accumulate(MorseDecoder::END_OF_CHAR, buf, sizeof(buf), &pos);
    CHECK(true);  // survived without crashing
}

int main() {
    printf("=== test_morse_decoder ===\n");
    RUN(test_decode_ka_prosign);
    RUN(test_decode_ar_prosign);
    RUN(test_decode_sk_prosign);
    RUN(test_decode_kn_prosign);
    RUN(test_decode_error_prosign);
    RUN(test_decode_standard_letters_still_work);
    RUN(test_decode_unknown_morse_returns_null);
    RUN(test_prosign_result_expands_to_multiple_chars);
    RUN(test_prosign_ar_expands_to_multiple_chars);
    RUN(test_decoder_read_empty_returns_false);
    RUN(test_decoder_available_starts_at_zero);
    RUN(test_hook_fires_per_decoded_char);
    RUN(test_hook_fires_per_prosign_char);
    RUN(test_hook_fires_for_space_char);
    RUN(test_hook_silent_when_uninstalled);
    RUN(test_decoder_read_after_manual_write);
    return test_summary();
}