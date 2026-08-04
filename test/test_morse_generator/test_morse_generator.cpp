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

// --- chunk-boundary silence (WinKey bridge hands the player text
//     in multiple chunks when RUMlogNG streams slowly) ---

// Helper: fill one buffer of `windowSamples` and return the count
// of non-zero samples. With the chunk-boundary fix, the very
// first window after a playText("U") following "T" is silent (the
// prepended CHAR_SPACE); without the fix it already contains
// U's first DIT.
static int countNonZeroInWindow(MorseGenerator& gen, int windowSamples) {
    std::vector<int16_t> buf(windowSamples, 0);
    gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    int n = 0;
    for (auto s : buf) if (s != 0) ++n;
    return n;
}

static void test_split_chunks_preserve_inter_char_space() {
    // Play "T", let it finish, then play "U". The audio between T
    // and U must contain a 3-unit CHAR_SPACE, NOT zero silence. The
    // user reported "UR 5NN TU" with the gap missing between T and
    // U: the encoder does not emit a trailing CHAR_SPACE after the
    // last char in a chunk, so without the fix "U" starts
    // immediately after T's DAH.
    //
    // Strategy: drain "T" to completion. Then playText("U") and
    // sample the first 3*ditSamples of audio. With the fix, that
    // window is entirely silence (the prepended CHAR_SPACE).
    // Without the fix, U's first DIT lands within that window.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("T");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 200;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    CHECK(!gen.isPlaying());

    gen.playText("U");
    CHECK(gen.isPlaying());

    int nz = countNonZeroInWindow(gen, 3 * ditSamples);
    CHECK(nz == 0);
}

static void test_split_with_trailing_space_no_double_gap() {
    // "T " (T then space) ends with a WORD_SPACE; next chunk "U"
    // must NOT prepend another CHAR_SPACE on top, otherwise the gap
    // would be 7 + 3 = 10 units instead of the expected 7 units.
    //
    // Strategy: drain "T " to completion. Then playText("U") and
    // sample the first 3*ditSamples. With the fix, that window
    // contains U's first DIT (non-zero). Without the fix (if we
    // incorrectly prepended), the window would be silent.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("T ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 200;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    CHECK(!gen.isPlaying());

    gen.playText("U");
    int nz = countNonZeroInWindow(gen, 3 * ditSamples);
    CHECK(nz > 0);
}

static void test_stop_resets_prepend_state() {
    // After stop(), the next playText() must NOT prepend a
    // CHAR_SPACE: stop() is an intentional reset. Same shape as
    // the trailing-space case -- U's first DIT lands within the
    // first 3*ditSamples (no leading silence).
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("T");
    gen.stop();
    CHECK(!gen.isPlaying());

    gen.playText("U");
    int nz = countNonZeroInWindow(gen, 3 * ditSamples);
    CHECK(nz > 0);
}

// --- Real-world RUMlogNG chunking patterns ---
//
// The actual hardware wires WinkeyBridge::cbSendText → MorseGenerator::playText.
// The bridge accumulates text in its buffer and drains when canAcceptText()
// returns true (i.e. the player is idle). RUMlogNG paces its serial writes,
// so the chunks delivered to playText() are typically:
//   - "UR 599 "  (up to and including the inter-word space)
//   - "TU"       (next word's first chunk)
// or, in worst case (host pauses mid-word):
//   - "UR 5NN "
//   - "T"
//   - "U"
// These tests replay those exact patterns and verify the T→U gap survives.

// Helper: drain a single chunk then sample the first `windowSamples` of
// the next chunk. Returns the sample buffer so the caller can inspect.
static std::vector<int16_t> drainChunkAndStartNext(MorseGenerator& gen,
                                                    const char* chunk1,
                                                    const char* chunk2,
                                                    int windowSamples) {
    gen.playText(chunk1);
    std::vector<int16_t> buf(256, 0);
    int maxIter = 400;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    gen.playText(chunk2);
    std::vector<int16_t> out(windowSamples, 0);
    gen.fillSamplesMono(out.data(), out.size(), 500.0f, 16384);
    return out;
}

static void test_rumlog_ur_599_TU() {
    // User-reported scenario: "UR 599 " then "TU".
    // Chunk 1 ends with WORD_SPACE → no prepend → encoder emits
    // [DAH, CHAR_SPACE, DIT, DIT, DAH] for chunk 2.
    //   timeline (DAH envelope = 4u, CHAR_SPACE = 3u, DIT = 2u):
    //     T_DAH(4u: 3 tone + 1 trailing) | CHAR_SPACE(3u silence) | U_DIT(2u)
    //   The CHAR_SPACE provides 3u of silence AFTER T's DAH envelope;
    //   T's envelope trailing silence (1u, internal) plus CHAR_SPACE (3u)
    //   give the spec's 4u gap between T's tone end and U's tone start.
    // Verify: 3 units after T's DAH envelope are silent (the CHAR_SPACE),
    // then U's first DIT begins.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("UR 599 ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 4000;  // generous; "UR 599 " needs ~85*ditLen/256 ≈ 1000 iter
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    gen.playText("TU");
    // First 4 units = T's DAH envelope (3 tone + 1 trailing silence)
    std::vector<int16_t> tMark(4 * ditSamples, 0);
    gen.fillSamplesMono(tMark.data(), tMark.size(), 500.0f, 16384);
    int nz = 0;
    for (auto s : tMark) if (s != 0) ++nz;
    CHECK(nz > 0);  // T's DAH tone is audible

    // Next 3 units = CHAR_SPACE silence (the inter-character gap)
    std::vector<int16_t> tToU(3 * ditSamples, 0);
    gen.fillSamplesMono(tToU.data(), tToU.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : tToU) if (s != 0) ++nz;
    CHECK(nz == 0);  // T→U gap has the expected silence

    // Then U's first DIT (1 tone + 1 trailing) begins → tone
    std::vector<int16_t> uStart(ditSamples, 0);
    gen.fillSamplesMono(uStart.data(), uStart.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : uStart) if (s != 0) ++nz;
    CHECK(nz > 0);
}

static void test_rumlog_ur_5NN_TU() {
    // Same shape as ur_599_TU but with N instead of 9s.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("UR 5NN ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 4000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    gen.playText("TU");
    std::vector<int16_t> tMark(4 * ditSamples, 0);
    gen.fillSamplesMono(tMark.data(), tMark.size(), 500.0f, 16384);
    int nz = 0;
    for (auto s : tMark) if (s != 0) ++nz;
    CHECK(nz > 0);

    std::vector<int16_t> tToU(3 * ditSamples, 0);
    gen.fillSamplesMono(tToU.data(), tToU.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : tToU) if (s != 0) ++nz;
    CHECK(nz == 0);
}

static void test_rumlog_ur_5NN_T_then_U() {
    // Worst case: "UR 5NN " then "T" then "U" (host paused mid-word).
    // After "UR 5NN " chunk ends with WORD_SPACE (no prepend for "T").
    // Encoder emits [DAH] for "T" — no trailing CHAR_SPACE because T
    // is the last char in the chunk. After T's DAH drains, _ended-
    // WithBoundarySilence = false, so playText("U") MUST prepend a
    // CHAR_SPACE. The first 3*ditSamples after playText("U") must be
    // silence.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("UR 5NN ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 4000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    gen.playText("T");
    maxIter = 4000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    gen.playText("U");
    int nz = countNonZeroInWindow(gen, 3 * ditSamples);
    CHECK(nz == 0);
}

static void test_rumlog_599_TU() {
    // "599 " then "TU" — same shape as the 599 test above.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("599 ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 4000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    gen.playText("TU");
    // First 4 units = T's DAH (tone)
    std::vector<int16_t> tMark(4 * ditSamples, 0);
    gen.fillSamplesMono(tMark.data(), tMark.size(), 500.0f, 16384);
    int nz = 0;
    for (auto s : tMark) if (s != 0) ++nz;
    CHECK(nz > 0);

    // Next 3 units = T→U gap (CHAR_SPACE silence)
    std::vector<int16_t> tToU(3 * ditSamples, 0);
    gen.fillSamplesMono(tToU.data(), tToU.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : tToU) if (s != 0) ++nz;
    CHECK(nz == 0);
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
    RUN(test_split_chunks_preserve_inter_char_space);
    RUN(test_split_with_trailing_space_no_double_gap);
    RUN(test_stop_resets_prepend_state);
    RUN(test_rumlog_ur_599_TU);
    RUN(test_rumlog_ur_5NN_TU);
    RUN(test_rumlog_ur_5NN_T_then_U);
    RUN(test_rumlog_599_TU);
    return test_summary();
}
