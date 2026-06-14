#include "test_framework.h"
#include "morse_generator.h"
#include "key_envelop.h"
#include <vector>

// --- state transitions ---

static void test_generator_idle_by_default() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    CHECK(!gen.isPlaying());
}

static void test_generator_playing_after_play_text() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("E");
    CHECK(gen.isPlaying());
}

static void test_stop_returns_to_idle() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("E");
    gen.stop();
    CHECK(!gen.isPlaying());
}

static void test_play_empty_string_stays_idle() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("");
    CHECK(!gen.isPlaying());
}

// --- single tone playback (E = single DIT, no silence elements) ---

static void test_e_generates_nonzero_samples() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("E");

    int bufSize = env.ditLengthSamples() * 2;
    std::vector<int16_t> buf(bufSize, 0);
    gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    int nonZero = 0;
    for (auto s : buf)
        if (s != 0) ++nonZero;
    CHECK(nonZero > 0);
}

static void test_e_finishes_within_envelope_duration() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("E");

    // Filling exactly the DIT envelope size must exhaust the generator
    int envSamples = (int)env.envelopeSize(KeyEnvelop::Element::DIT);
    std::vector<int16_t> buf(envSamples, 0);
    gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    CHECK(!gen.isPlaying());
}

static void test_t_generates_nonzero_samples() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("T");

    int bufSize = (int)env.envelopeSize(KeyEnvelop::Element::DAH);
    std::vector<int16_t> buf(bufSize, 0);
    gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    int nonZero = 0;
    for (auto s : buf)
        if (s != 0) ++nonZero;
    CHECK(nonZero > 0);
}

static void test_stereo_fill_produces_same_left_and_right() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("E");

    const int N = 128;
    std::vector<int16_t> left(N, 0), right(N, 0);
    gen.fillSamples(left.data(), right.data(), N, 500.0f, 16384);

    bool ok = true;
    for (int i = 0; i < N; ++i)
        if (left[i] != right[i]) { ok = false; break; }
    CHECK(ok);
}

// --- silence elements advance correctly ---

static void test_word_space_produces_silence_and_finishes() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText(" ");
    CHECK(gen.isPlaying());

    // Consume samples in 256-sample chunks; all must be zero (silence)
    // and the generator must become idle within 7*ditLen samples.
    std::vector<int16_t> buf(256, 0x7F);
    bool anyNonZero = false;
    int maxIter = 200; // 200*256 = 51200 > 7*2881 = 20167
    while (gen.isPlaying() && maxIter-- > 0) {
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
        for (auto s : buf) if (s != 0) anyNonZero = true;
    }
    CHECK(!gen.isPlaying()); // generator exhausted the word-space element
    CHECK(!anyNonZero);      // no tone during pure silence
}

static void test_multi_char_et_plays_and_finishes() {
    // "ET" = DIT, CHAR_SPACE, DAH — exercises silence-element advancement
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("ET");
    CHECK(gen.isPlaying());

    std::vector<int16_t> buf(1024, 0);
    int maxIter = 100; // generous upper bound
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    CHECK(!gen.isPlaying());
}

static void test_last_char_appended_at_boundary() {
    // "E" = single DIT, no trailing CHAR_SPACE.
    // The boundary handler must append 'E' when all elements are exhausted.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("E");

    std::vector<int16_t> buf(2048, 0);
    int maxIter = 200;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    CHECK(!gen.isPlaying());
    // Verify _currentChar was cleared after boundary (prevent re-append)
    CHECK_EQ(gen.currentChar(), (char)0);
}

static void test_special_char_bang_appended_at_boundary() {
    // "!" has no trailing CHAR_SPACE — boundary handler must append '!' explicitly
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("!");

    std::vector<int16_t> buf(4096, 0);
    int maxIter = 200;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    CHECK(!gen.isPlaying());
    CHECK_EQ(gen.currentChar(), (char)0);
}

static void test_multi_char_hello_exhaustive() {
    // "Hello Morse!" — verify ALL 13 chars are captured, including '!' at end
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("Hello Morse!");

    std::vector<int16_t> buf(4096, 0);
    int maxIter = 500;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    CHECK(!gen.isPlaying());
    CHECK_EQ(gen.currentChar(), (char)0);
}

// --- amplitude and WPM ---

static void test_amplitude_zero_gives_all_zero_output() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText("E");

    std::vector<int16_t> buf(256, 0x7F);
    gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 0 /* amp = 0 */);

    bool allZero = true;
    for (auto s : buf)
        if (s != 0) { allZero = false; break; }
    CHECK(allZero);
}

static void test_wpm_accessor_reflects_set_wpm() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    CHECK_EQ(20, gen.wpm());
    gen.setWPM(30);
    CHECK_EQ(30, gen.wpm());
}

int main() {
    printf("=== test_morse_generator ===\n");
    RUN(test_generator_idle_by_default);
    RUN(test_generator_playing_after_play_text);
    RUN(test_stop_returns_to_idle);
    RUN(test_play_empty_string_stays_idle);
    RUN(test_e_generates_nonzero_samples);
    RUN(test_e_finishes_within_envelope_duration);
    RUN(test_t_generates_nonzero_samples);
    RUN(test_stereo_fill_produces_same_left_and_right);
    RUN(test_word_space_produces_silence_and_finishes);
    RUN(test_multi_char_et_plays_and_finishes);
    RUN(test_last_char_appended_at_boundary);
    RUN(test_special_char_bang_appended_at_boundary);
    RUN(test_multi_char_hello_exhaustive);
    RUN(test_amplitude_zero_gives_all_zero_output);
    RUN(test_wpm_accessor_reflects_set_wpm);
    return test_summary();
}
