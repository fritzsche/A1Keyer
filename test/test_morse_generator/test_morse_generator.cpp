#include "test_framework.h"
#include "morse_generator.h"
#include "key_envelop.h"
#include "display_task.h"
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
    // Strategy: drain "T" to completion. Then playText("U", true)
    // — the `true` models the WinKeyBridge marking the next chunk
    // as a continuation (its audio follows the previous chunk's
    // audio without a gap) — and sample the first 2*ditSamples of
    // audio. With the fix, that window is entirely silence (the
    // prepended CHAR_SPACE). Without the fix, U's first DIT lands
    // within that window.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("T");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 200;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    CHECK(!gen.isPlaying());

    gen.playText("U", true);
    CHECK(gen.isPlaying());

    int nz = countNonZeroInWindow(gen, 2 * ditSamples);
    CHECK(nz == 0);
}

static void test_split_with_trailing_space_no_double_gap() {
    // "T " (T then space) ends with a WORD_SPACE; next chunk "U"
    // must NOT prepend another CHAR_SPACE on top, otherwise the gap
    // would be 7 + 3 = 10 units instead of the expected 7 units.
    //
    // Strategy: drain "T " to completion. Then playText("U", true)
    // — the `true` models the bridge marking this as a continuation
    // — and sample the first 3*ditSamples. With the fix, that
    // window contains U's first DIT (non-zero). Without the fix
    // (if we incorrectly prepended), the window would be silent.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("T ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 200;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    CHECK(!gen.isPlaying());

    gen.playText("U", true);
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

// --- Regression test for the back-to-back independent playback bug ---
//
// (Defined later, after readDecodedText(), because it uses that
// helper. Declared here so the RUN() in main() can resolve it.)
static void test_back_to_back_independent_playback_no_prepend();

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
    // Chunk 1 ends with WORD_SPACE → no prepend (even with
    // isContinuation=true) → encoder emits
    // [DAH, CHAR_SPACE, DIT, DIT, DAH] for chunk 2.
    //   timeline (DAH envelope = 4u, CHAR_SPACE = 2u, DIT = 2u):
    //     T_DAH(4u: 3 tone + 1 trailing) | CHAR_SPACE(2u silence) | U_DIT(2u)
    //   The CHAR_SPACE provides 2u of silence AFTER T's DAH envelope;
    //   T's envelope trailing silence (1u, internal) plus CHAR_SPACE (2u)
    //   give the spec's 3u gap between T's tone end and U's tone start.
    // Verify: 2 units after T's DAH envelope are silent (the CHAR_SPACE),
    // then U's first DIT begins.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("UR 599 ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 4000;  // generous; "UR 599 " needs ~85*ditLen/256 ≈ 1000 iter
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    gen.playText("TU", true);  // continuation — bridge marks it as such
    // First 4 units = T's DAH envelope (3 tone + 1 trailing silence)
    std::vector<int16_t> tMark(4 * ditSamples, 0);
    gen.fillSamplesMono(tMark.data(), tMark.size(), 500.0f, 16384);
    int nz = 0;
    for (auto s : tMark) if (s != 0) ++nz;
    CHECK(nz > 0);  // T's DAH tone is audible

    // Next 2 units = CHAR_SPACE silence (the inter-character gap;
    // plus the 1u envelope trailing from T = 3 spec units total).
    std::vector<int16_t> tToU(2 * ditSamples, 0);
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

    gen.playText("TU", true);  // continuation
    std::vector<int16_t> tMark(4 * ditSamples, 0);
    gen.fillSamplesMono(tMark.data(), tMark.size(), 500.0f, 16384);
    int nz = 0;
    for (auto s : tMark) if (s != 0) ++nz;
    CHECK(nz > 0);

    std::vector<int16_t> tToU(2 * ditSamples, 0);
    gen.fillSamplesMono(tToU.data(), tToU.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : tToU) if (s != 0) ++nz;
    CHECK(nz == 0);
}

static void test_rumlog_ur_5NN_T_then_U() {
    // Worst case: "UR 5NN " then "T" then "U" (host paused mid-word).
    // After "UR 5NN " chunk ends with WORD_SPACE (no prepend for "T"
    // because _endedWithBoundarySilence is true).
    // Encoder emits [DAH] for "T" — no trailing CHAR_SPACE because T
    // is the last char in the chunk. After T's DAH drains, _ended-
    // WithBoundarySilence = false, so playText("U", true) MUST
    // prepend a CHAR_SPACE (the explicit continuation flag is what
    // gates the prepend now, not _wasPlaying). The first 2*ditSamples
    // after playText("U") must be silence (the prepended boundary
    // CHAR_SPACE; total gap with T's envelope trailing is 3 spec
    // units).
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    int ditSamples = env.ditLengthSamples();

    gen.playText("UR 5NN ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 4000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    gen.playText("T", true);  // continuation
    maxIter = 4000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    gen.playText("U", true);  // continuation — MUST prepend
    int nz = countNonZeroInWindow(gen, 2 * ditSamples);
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

    gen.playText("TU", true);  // continuation
    // First 4 units = T's DAH (tone)
    std::vector<int16_t> tMark(4 * ditSamples, 0);
    gen.fillSamplesMono(tMark.data(), tMark.size(), 500.0f, 16384);
    int nz = 0;
    for (auto s : tMark) if (s != 0) ++nz;
    CHECK(nz > 0);

    // Next 2 units = T→U gap (CHAR_SPACE silence; plus T's 1u envelope
    // trailing silence = 3 spec units total).
    std::vector<int16_t> tToU(2 * ditSamples, 0);
    gen.fillSamplesMono(tToU.data(), tToU.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : tToU) if (s != 0) ++nz;
    CHECK(nz == 0);
}


// ─── Screensaver wake-up (docs/winkey.md § 16.9) ─────────────────────
//
// MorseGenerator must bump the screensaver wake-up flag on every
// key-down element boundary so the screen unblanks during stored-text
// playback (Cardputer P-key) and WinKey text playback (RUMlogNG
// typing). Mirrors the paddle-ISR wake path in src/morse_key.cpp:33,48.

static void test_playText_wakes_screensaver_on_each_keydown_element() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    // Drain any pending wake from earlier tests.
    while (DisplayTask::consumeWakeRequest()) {}
    gen.playText("E");  // E = single dit
    // Advance enough samples for the dit's key-down element to fire.
    int ditLen = env.envelopeSize(KeyEnvelop::Element::DIT);
    std::vector<int16_t> buf(ditLen * 2, 0);
    gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    // The dit's key-down element must have bumped the wake flag.
    CHECK(DisplayTask::consumeWakeRequest());
}

static void test_silence_only_text_does_not_wake() {
    // No text played → no element boundaries → no wake requests.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    while (DisplayTask::consumeWakeRequest()) {}
    int ditLen = env.envelopeSize(KeyEnvelop::Element::DIT);
    std::vector<int16_t> buf(ditLen * 4, 0);
    gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    CHECK(!DisplayTask::consumeWakeRequest());
}

// ─── First-play-after-power-cycle (TU sounds like X bug) ─────────────
//
// User-reported: "if i press the cardputer rst or powercycle cardpoter
// it does not outpu the correct sound a 'TU' sounds like a 'x' letter.
// on later 'TU' the sound is correct only the fist sending is messen."
//
// Repro shape: at boot the envelope is constructed at wpm=20 (the
// sharedEnvelope() default), then MorseModel::setWPM(savedWpm) is called
// from NVS, then the host (RUMlogNG) sends WK_SPEED which propagates to
// the model. We must verify that the FIRST playText() of "TU" with that
// sequence of WPM changes produces the correct inter-character silence
// (not zero, which would make "TU" sound like "X" = -..-).
static void test_first_play_after_wpm_change_produces_TU_not_X() {
    // Boot defaults
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);

    // Boot sequence:
    //   1. MorseModel::setWPM(savedWpm=15) from NVS
    gen.setWPM(15);
    //   2. Host WK_SPEED(25) overrides via MorseModel::setWPM(25)
    gen.setWPM(25);

    // FIRST playText call after power-cycle
    gen.playText("TU");

    int ditSamples = env.ditLengthSamples();

    // T's DAH = 4 units (3 tone + 1 envelope trailing silence)
    std::vector<int16_t> tMark(4 * ditSamples, 0);
    gen.fillSamplesMono(tMark.data(), tMark.size(), 500.0f, 16384);
    int nz = 0;
    for (auto s : tMark) if (s != 0) ++nz;
    CHECK(nz > 0);  // T's DAH tone is audible

    // Next 2 units = T→U inter-character gap (CHAR_SPACE silence)
    // If the gap is missing, the audio continues with U's first DIT
    // (which sounds like "-..-.." = X+Y, indistinguishable as X).
    std::vector<int16_t> tToU(2 * ditSamples, 0);
    gen.fillSamplesMono(tToU.data(), tToU.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : tToU) if (s != 0) ++nz;
    CHECK(nz == 0);  // TU bug: would fail here if gap is missing

    // U's first DIT (1 tone unit + 1 trailing silence)
    std::vector<int16_t> uStart(ditSamples, 0);
    gen.fillSamplesMono(uStart.data(), uStart.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : uStart) if (s != 0) ++nz;
    CHECK(nz > 0);  // U's DIT tone is audible
}

// Direct probe: encode "TU" without any prior playback, verify the
// CHAR_SPACE between T and U is actually present in the element list.
// This is the most isolated reproduction of the bug — if the encoder
// skips the inter-character CHAR_SPACE on a fresh encode, that explains
// everything.
static void test_encoder_emits_char_space_between_T_and_U() {
    MorseEncoder enc(20);
    auto elems = enc.encode("TU");

    // Expected: [DAH(3,true), CHAR_SPACE(2,false), DIT(1,true), DIT(1,true), DAH(3,true)]
    CHECK_EQ(elems.size(), (size_t)5);
    CHECK_EQ((int)elems[0].type, (int)MorseEncoder::Element::DAH);
    CHECK(elems[0].keyDown);
    CHECK_EQ((int)elems[1].type, (int)MorseEncoder::Element::CHAR_SPACE);
    CHECK(!elems[1].keyDown);
    CHECK_EQ((int)elems[2].type, (int)MorseEncoder::Element::DIT);
    CHECK(elems[2].keyDown);
    CHECK_EQ((int)elems[3].type, (int)MorseEncoder::Element::DIT);
    CHECK(elems[3].keyDown);
    CHECK_EQ((int)elems[4].type, (int)MorseEncoder::Element::DAH);
    CHECK(elems[4].keyDown);
}

// ─── Char tracking across WORD_SPACE (memory playback display bug) ───
//
// User-reported: playing back a memory slot containing "CQ CQ …", the
// device screen and web UI showed "CQ " (a trailing space) while the
// audio was keying the second C. The encoder emits one WORD_SPACE per
// ASCII space; the generator's silence branch only advances _charIdx
// by one position per silence element, so the next mark's branch set
// _currentChar from a position that still pointed at the space, and
// the following CHAR_SPACE appended the space instead of 'C' to the
// model buffer.
//
// These tests verify that the mark branch's new skip-spaces guard
// advances _charIdx past any consecutive spaces before assigning
// _currentChar, so the right letter gets captured for the boundary.

#include "display_model.h"

static std::string readDecodedText() {
    auto& m = MorseModel::instance();
    const size_t tl  = m.decodedTextLen();
    const size_t tail = m.textTail();
    std::string out;
    out.reserve(tl);
    for (size_t i = 0; i < tl; ++i) {
        size_t idx = (tail + i) % MorseModel::TEXT_BUF_SIZE;
        char c = m.textAt(idx);
        if (c) out.push_back(c);
    }
    return out;
}

static void test_word_boundary_displays_inter_word_space_at_word_boundary() {
    // "CQ CQ" — the regression case. Before the fix, the second C
    // was captured as ' ' (a space) and the display read "CQ " with
    // a trailing space during the second C's audio. The fix appends
    // the inter-word space at the start of the WORD_SPACE silence
    // (in sync with the audio gap), so the display now reads "CQ CQ"
    // with the space between the two words — exactly where the user
    // expects it.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    MorseModel::instance().clearDecodedText();

    gen.playText("CQ CQ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 4000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    std::string got = readDecodedText();
    CHECK(got == "CQ CQ");
}

static void test_leading_multi_space_input() {
    // "  CQ" — encoder emits two WORD_SPACE elements. The silence
    // branch appends _currentChar at each WS, and _currentChar is
    // initialised to _playText[0] (' ') in playText. So both leading
    // spaces get appended to the decoded buffer, then C and Q are
    // appended via the normal CS / boundary paths. End state is
    // "  CQ" (two leading spaces preserved). The skip-spaces fix is
    // only in the mark branch, so leading-space preservation is
    // unaffected — the fix must not regress this behaviour.
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    MorseModel::instance().clearDecodedText();

    gen.playText("  CQ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 4000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    std::string got = readDecodedText();
    CHECK(got == "  CQ");
}

static void test_trailing_space_after_words() {
    // "CQ CQ " — the trailing WORD_SPACE's append-' ' separator is
    // suppressed by the peek guard (the trailing space is followed
    // by nothing, so peek walks past size). The exhausted branch
    // then appends the trailing space directly. End state is
    // "CQ CQ " — the inter-word space is shown (fix correctness)
    // and the trailing space is preserved (unchanged behaviour).
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    MorseModel::instance().clearDecodedText();

    gen.playText("CQ CQ ");
    std::vector<int16_t> buf(256, 0);
    int maxIter = 4000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    std::string got = readDecodedText();
    CHECK(got == "CQ CQ ");
}

static void test_full_memory_playback_appends_in_order() {
    // The user's reported memory slot: "CQ CQ JJ1QPB/1 JJ1QPB/1".
    // Decoded text must be appended in the order the audio plays:
    //   C, Q, ' ', C, Q, ' ', J, J, 1, Q, P, B, /, 1, ' ', J, J, 1, Q, P, B, /, 1
    // i.e. "CQ CQ JJ1QPB/1 JJ1QPB/1". With the skip-spaces fix
    // and the WS-append fix, this is what a single playText() call
    // produces. If the user is seeing spaces shifted one position
    // to the right, the bug is in the chunked playback path (the
    // chunked variant plays "CQ " as one chunk, then "CQ " as the
    // next, with the WS boundary straddling a chunk — the
    // boundary-prepend interact with the silence branch in a way
    // the single-chunk path doesn't).
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    MorseModel::instance().clearDecodedText();

    gen.playText("CQ CQ JJ1QPB/1 JJ1QPB/1");
    std::vector<int16_t> buf(1024, 0);
    int maxIter = 20000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    std::string got = readDecodedText();
    CHECK(got == "CQ CQ JJ1QPB/1 JJ1QPB/1");
}

// --- Regression test for the back-to-back independent playback bug ---
//
// The user's symptom: playing back "CQ CQ JJ1QPB/1 JJ1QPB/1" once via
// the keypad produced correct output, but playing it back AGAIN via
// the web UI produced "cqc qj j1qpb/1j j1qpb/1" — spaces shifted one
// position to the right. Root cause: the OLD design gated the
// boundary prepend on the `_wasPlaying` flag (set on every playText,
// never reset except by stop()), so any second consecutive playText()
// prepended an unwanted CHAR_SPACE that advanced _charIdx past the
// first char of the new text. The new design gates the prepend on
// the explicit `isContinuation` parameter; this test asserts that
// two INDEPENDENT playText() calls (no continuation flag, no stop()
// between them) both render correctly.
static void test_back_to_back_independent_playback_no_prepend() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    MorseModel::instance().clearDecodedText();

    // First playback: fresh, default isContinuation=false.
    gen.playText("CQ CQ JJ1QPB/1 JJ1QPB/1");
    std::vector<int16_t> buf(1024, 0);
    int maxIter = 20000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    std::string firstGot = readDecodedText();
    CHECK(firstGot == "CQ CQ JJ1QPB/1 JJ1QPB/1");

    // Second playback: ALSO fresh, default isContinuation=false.
    // With the OLD bug, _wasPlaying was still true from the first
    // call and the boundary prepend fired, shifting every space one
    // position to the right ("cqc qj j1qpb/1j j1qpb/1"). With the
    // fix, the prepend is gated on the explicit isContinuation flag
    // (false here), so the second playback renders identically to
    // the first.
    MorseModel::instance().clearDecodedText();
    gen.playText("CQ CQ JJ1QPB/1 JJ1QPB/1");
    maxIter = 20000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);

    std::string secondGot = readDecodedText();
    CHECK(secondGot == "CQ CQ JJ1QPB/1 JJ1QPB/1");
    CHECK(firstGot == secondGot);
}

// ─── Bridge integration regression (2026-08-18) ────────────────────────
//
// The previous tests above call MorseGenerator::playText directly with
// an explicit isContinuation=true flag to model what the bridge does
// in production. But the bridge actually derives the flag from its
// own _isContinuation state, and if that state is wrong the tests
// won't catch it — they'd model behaviour that never happens.
//
// This test wires a real WinkeyBridge → MorseGenerator::playText
// pipeline (the same as production), then feeds "UR 5NN " followed
// by "TU" via the host-side byte stream (the RUMlogNG chunking
// pattern the user reported). It then checks the audio between
// T's DAH and U's first DIT contains the expected 2-unit silence
// from the encoder's CHAR_SPACE between T and U. If the bridge's
// _isContinuation tracking is wrong and the prepend is dropped
// (or, conversely, applied when it shouldn't be), this test will
// fail with the symptom the user heard: "TU" sounding like "X"
// because the inter-character gap is missing.

#include "winkey_bridge.h"
#include "morse_encoder.h"

namespace bridge_test {

struct BridgeRunner {
    WinkeyBridge br;
    MorseGenerator* gen = nullptr;

    static void cbSendText(const char* t, void* ctx) {
        auto* self = static_cast<BridgeRunner*>(ctx);
        MorseGenerator* g = self->gen;
        if (!g) return;
        // Mirror what src/winkey.cpp does: only call playText when
        // the generator is idle, and pass the bridge's continuation
        // flag through. This is the EXACT production path.
        if (!g->isPlaying()) {
            g->playText(t, self->br.isContinuation());
        }
    }
    static bool cbCanAccept(void* ctx) {
        auto* self = static_cast<BridgeRunner*>(ctx);
        return self->gen && !self->gen->isPlaying();
    }
    static void cbStop(void*) {}
    static void cbOut(uint8_t, void*) {}
    static void cbWpm(int, void*) {}
    static void cbSidetone(int, void*) {}
    static void cbOutEn(bool, void*) {}

    void setup() {
        WinkeyBridge::Callbacks cb{};
        cb.sendText      = &cbSendText;
        cb.canAcceptText = &cbCanAccept;
        cb.stopSending   = &cbStop;
        cb.ctx           = this;   // so the static callbacks can find `this`
        br.begin(&cbOut, nullptr, cb);
        br.resetParams();   // start clean
        // Host-open → version, status mode, GET_POT to prime.
        br.feed(0x00); br.feed(0x02);
        br.feed(0x00); br.feed(0x0B);
        br.feed(0x07);
    }

    void feedText(const char* s) {
        for (const char* p = s; *p; ++p) br.feed((uint8_t)*p);
    }
};

}  // namespace bridge_test

static void test_bridge_chunks_preserve_inter_char_T_to_U() {
    using namespace bridge_test;
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);

    BridgeRunner r;
    r.gen = &gen;
    r.setup();

    // Feed chunk 1 ("UR 5NN ", with trailing space — typical
    // RUMlogNG pacing) and drain.
    r.feedText("UR 5NN ");
    r.br.poll();
    int ditSamples = env.ditLengthSamples();
    std::vector<int16_t> buf(256, 0);
    int maxIter = 4000;
    while (gen.isPlaying() && maxIter-- > 0)
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    CHECK(!gen.isPlaying());

    // After chunk 1 has been dispatched to cbSendText, the bridge's
    // _isContinuation flag MUST be true: the next poll() that drains
    // a non-empty buffer should treat the second chunk as a
    // continuation of the first, so playText() prepends the
    // boundary CHAR_SPACE that the encoder does not emit at the
    // chunk's tail. If this flag is wrong, the second chunk's T
    // and U run back-to-back with no inter-character gap → the
    // "TU sounds like X" symptom the user reported.
    CHECK(r.br.isContinuation());

    // Production-like interleaving: poll() runs every loop() tick,
    // not just when bytes arrive. Between chunk 1's drain and
    // chunk 2's arrival, multiple poll()s fire — and every one of
    // them sees an empty buffer and resets _isContinuation=false
    // (current bridge behaviour). To catch that regression, drive
    // poll() repeatedly here exactly the way the device does in its
    // main loop, then verify the flag is still true when chunk 2
    // finally arrives.
    for (int i = 0; i < 5; ++i) {
        r.br.poll();          // production-style: drains even when empty
    }
    CHECK(r.br.isContinuation());

    // Feed chunk 2 ("TU") and check the audio gap between T and U.
    r.feedText("TU");
    r.br.poll();
    CHECK(gen.isPlaying());

    // T's DAH envelope (4 units: 3 tone + 1 trailing). First 4u
    // should contain tone.
    std::vector<int16_t> tMark(4 * ditSamples, 0);
    gen.fillSamplesMono(tMark.data(), tMark.size(), 500.0f, 16384);
    int nz = 0;
    for (auto s : tMark) if (s != 0) ++nz;
    CHECK(nz > 0);  // T's DAH tone is audible

    // The encoder emits CHAR_SPACE between T and U (2 units of
    // silence). The next 2u should be silent.
    std::vector<int16_t> tToU(2 * ditSamples, 0);
    gen.fillSamplesMono(tToU.data(), tToU.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : tToU) if (s != 0) ++nz;
    CHECK(nz == 0);  // T→U encoder CHAR_SPACE preserved

    // U's first DIT (2 units: 1 tone + 1 trailing). First u should
    // be tone.
    std::vector<int16_t> uStart(ditSamples, 0);
    gen.fillSamplesMono(uStart.data(), uStart.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : uStart) if (s != 0) ++nz;
    CHECK(nz > 0);
}

static void test_bridge_single_chunk_TU_audio_correct() {
    // Single-chunk "TU" via the bridge. No preceding chunk, so the
    // bridge's _isContinuation is false (no prepend). The encoder
    // still emits CHAR_SPACE between T and U, so the gap must be
    // audible — this is the baseline that proves the encoder's
    // CHAR_SPACE is intact regardless of the prepend logic.
    using namespace bridge_test;
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);

    BridgeRunner r;
    r.gen = &gen;
    r.setup();

    r.feedText("TU");
    r.br.poll();
    int ditSamples = env.ditLengthSamples();

    // T's DAH (4 units).
    std::vector<int16_t> tMark(4 * ditSamples, 0);
    gen.fillSamplesMono(tMark.data(), tMark.size(), 500.0f, 16384);
    int nz = 0;
    for (auto s : tMark) if (s != 0) ++nz;
    CHECK(nz > 0);

    // Encoder's CHAR_SPACE between T and U (2 units).
    std::vector<int16_t> tToU(2 * ditSamples, 0);
    gen.fillSamplesMono(tToU.data(), tToU.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : tToU) if (s != 0) ++nz;
    CHECK(nz == 0);

    // U's first DIT.
    std::vector<int16_t> uStart(ditSamples, 0);
    gen.fillSamplesMono(uStart.data(), uStart.size(), 500.0f, 16384);
    nz = 0;
    for (auto s : uStart) if (s != 0) ++nz;
    CHECK(nz > 0);
}

// Two INDEPENDENT bridge-driven playbacks of the same multi-word
// memory slot, with multiple poll()s in between (the production
// cadence). After playback 1 drains and the bridge sits idle for
// many ticks, the next playback must NOT be flagged as a
// continuation: the user's "decoded text shifted +1" symptom is
// the encode-side appearance of the boundary CHAR_SPACE prepend
// firing on a fresh playback.
//
// The audio stays correct because the encoder naturally emits
// its own inter-character silence within a chunk, so a
// mistakenly-prepended CHAR_SPACE just adds an extra advance of
// _charIdx in the silence branch — which appends _currentChar
// (the first char of the new text, set by playText's init) and
// then the next CS appends the second char, shifting every
// subsequent space one position to the right.
static void test_bridge_two_independent_playbacks_do_not_shift_text() {
    using namespace bridge_test;
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);

    BridgeRunner r;
    r.gen = &gen;
    r.setup();
    MorseModel::instance().clearDecodedText();

    const char* kMemory = "CQC CQ DE DJ1TF DJ1TF DJ1TF PSE K";

    bool firstDrain = true;
    auto drainAndPump = [&](int emptyPumpTicks) {
        std::vector<int16_t> buf(256, 0);
        int maxIter = 8000;
        // Production cadence: poll() runs every loop() tick.
        // Alternate draining a few samples and polling so the bridge
        // observes the consumer busy→idle transition (audio playing
        // then audio finishing). Without interleaving, the bridge
        // never sees the busy state and the busy→idle edge is never
        // observed — many bridges rely on that edge for their state
        // machine, including this one (see winkey_bridge.cpp
        // `_prevConsumerBusy`).
        while (gen.isPlaying() && maxIter-- > 0) {
            gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
            r.br.poll();
        }
        // Now audio is idle. Pump several empty polls.
        for (int i = 0; i < emptyPumpTicks; ++i) r.br.poll();
        // After the FIRST playback drains and the device sits idle,
        // the bridge's flag MUST be false (the previous session
        // ended; the next cbSendText() will be a fresh playback).
        // We only check this on the first drain — playback 2's
        // drainAndPump(0) ends right after cbSendText fired and set
        // _isContinuation back to true (which is correct: the *next*
        // playback, if any, would be a continuation of playback 2).
        if (firstDrain) {
            CHECK(!r.br.isContinuation());
            firstDrain = false;
        }
    };

    // Playback 1.
    r.feedText(kMemory);
    r.br.poll();
    drainAndPump(/*emptyPumpTicks=*/200);

    // Playback 2 — same memory, fed fresh.
    MorseModel::instance().clearDecodedText();
    r.feedText(kMemory);
    r.br.poll();
    drainAndPump(/*emptyPumpTicks=*/0);

    std::string got = readDecodedText();
    CHECK(got == std::string(kMemory));
}

// One-char-at-a-time pacing: every fed byte is followed by a poll and
// a tiny audio drain. Worst-case serial pacing from a slow logger or
// USB-CDC with Nagle off. Verifies the decoded text exactly matches
// the source even when the bridge sees no opportunity to coalesce
// bytes into larger chunks.
static void test_bridge_one_char_at_a_time_long_call_no_shift() {
    using namespace bridge_test;
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);

    BridgeRunner r;
    r.gen = &gen;
    r.setup();

    MorseModel::instance().clearDecodedText();
    const char* kSrc = "CQ CQ DE DJ1TF DJ1TF DJ1TF PSE K";
    auto drainOneElement = [&]() {
        std::vector<int16_t> buf(128, 0);
        // Drain enough to ensure the busy→idle edge is observed by the
        // bridge on the next poll. Take a small amount then poll, repeat.
        for (int i = 0; i < 8 && gen.isPlaying(); ++i) {
            gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
            r.br.poll();
        }
    };
    for (const char* p = kSrc; *p; ++p) {
        r.br.feed((uint8_t)*p);
        r.br.poll();
        drainOneElement();
    }
    // Drain remaining audio.
    std::vector<int16_t> buf(256, 0);
    int maxIter = 8000;
    while (gen.isPlaying() && maxIter-- > 0) {
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
        r.br.poll();
    }
    for (int i = 0; i < 50; ++i) r.br.poll();

    std::string got = readDecodedText();
    CHECK(got == std::string(kSrc));
}
//
// User-reported: a long memory slot ("CQ CQ DE DJ1TF DJ1TF DJ1TF PSE K")
// renders correctly on small texts but loses two spaces near the end
// on a "long call": "CQ CQ DE DJ1TF DJ1TF DJ1TFP SEK" (the spaces
// before "PSE" and before "K" vanish). Two characters get pulled to
// the left, removing the boundary spaces.
//
// Production chunking: RUMlogNG / N1MM stream the entire slot to the
// bridge in ~one serial-burst tick. The audio thread consumes slowly
// compared to the serial arrival — bytes accumulate in the bridge
// buffer while the first chunk's audio plays. When the first chunk's
// audio finishes, the accumulated buffer drains as the SECOND
// chunk. A long string may span several such drains if audio playback
// is slower than serial feeding.
//
// Each chunk-boundary at a non-trailing MARK position (e.g. last char
// of chunk 1 ends in DAH rather than silence) is the dangerous shape:
// the encoder emits no trailing silence at the chunk tail, and the
// bridge's "_isContinuation" signal must fire the boundary CHAR_SPACE
// prepend in MorseGenerator to preserve the inter-character gap. If
// the append's interaction with _charIdx in the silence branch
// mismatched against the existing decoded text, the prepend advances
// _charIdx by one — silently eating the source's space at the
// boundary.
//
// This test simulates a 3-chunk split of the user's source string
// where every chunk ends at a MARK (worst case for the prepend) and
// verifies that the decoded text matches the source exactly.

static void test_bridge_three_chunks_long_call_no_text_shift() {
    using namespace bridge_test;
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);

    BridgeRunner r;
    r.gen = &gen;
    r.setup();

    MorseModel::instance().clearDecodedText();
    const char* full =
        "CQ CQ DE DJ1TF DJ1TF DJ1TF PSE K";

    // Three contiguous substrings chosen to end at MARK boundaries
    // (no trailing spaces). The last chunk has a trailing space —
    // the encoder produces a trailing WS for it which guards the
    // very last boundary.
    const char* kChunks[] = {
        "CQ CQ DE",            // ends after E (DAH, mark) → no boundary silence
        " DJ1TF DJ1TF",        // starts with leading space, ends F (DAH)
        " DJ1TF PSE K",        // ends with K (DAH) + trailing nothing
    };

    auto drainAndPump = [&](bool pumpMany) {
        std::vector<int16_t> buf(256, 0);
        int maxIter = 8000;
        while (gen.isPlaying() && maxIter-- > 0) {
            gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
            r.br.poll();
        }
        int n = pumpMany ? 50 : 5;
        for (int i = 0; i < n; ++i) r.br.poll();
    };

    for (size_t i = 0; i < sizeof(kChunks)/sizeof(kChunks[0]); ++i) {
        r.feedText(kChunks[i]);
        r.br.poll();
        drainAndPump(/*pumpMany=*/false);
    }

    std::string got = readDecodedText();
    CHECK(got == std::string(full));
}

// Same idea but every chunk ends WITHOUT a trailing space AND the
// first chunk's encoder output ends at a MARK. This is the
// worst-case shape for the boundary prepend — and also the shape
// that would surface the residual issue if the bridge's
// `_isContinuation` tracking on the non-empty / consumer-busy → idle
// edge mistakenly fired across multiple chunks.
static void test_bridge_chunks_ending_at_mark_no_text_shift() {
    using namespace bridge_test;
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);

    BridgeRunner r;
    r.gen = &gen;
    r.setup();

    MorseModel::instance().clearDecodedText();
    // Source has 3 "chunks" each ending at a MARK. The prepend fires
    // at every chunk boundary. Verify that nothing in the prepend /
    // silence-branch interaction causes the decoded text to lose any
    // of the source's inter-word spaces.
    const char* kSrc = "AB CD EF GH";

    auto drainAndPump = [&]() {
        std::vector<int16_t> buf(256, 0);
        int maxIter = 8000;
        while (gen.isPlaying() && maxIter-- > 0) {
            gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
            r.br.poll();
        }
        for (int i = 0; i < 5; ++i) r.br.poll();
    };

    const char* kChunks[] = { "AB", " C", "D E", "F G", "H" };
    for (size_t i = 0; i < sizeof(kChunks)/sizeof(kChunks[0]); ++i) {
        r.feedText(kChunks[i]);
        r.br.poll();
        drainAndPump();
    }

    std::string got = readDecodedText();
    CHECK(got == std::string(kSrc));
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
    RUN(test_bridge_chunks_preserve_inter_char_T_to_U);
    RUN(test_bridge_single_chunk_TU_audio_correct);
    RUN(test_rumlog_599_TU);
    RUN(test_playText_wakes_screensaver_on_each_keydown_element);
    RUN(test_silence_only_text_does_not_wake);
    RUN(test_first_play_after_wpm_change_produces_TU_not_X);
    RUN(test_encoder_emits_char_space_between_T_and_U);
    RUN(test_word_boundary_displays_inter_word_space_at_word_boundary);
    RUN(test_leading_multi_space_input);
    RUN(test_trailing_space_after_words);
    RUN(test_full_memory_playback_appends_in_order);
    RUN(test_back_to_back_independent_playback_no_prepend);
    RUN(test_bridge_two_independent_playbacks_do_not_shift_text);
    RUN(test_bridge_one_char_at_a_time_long_call_no_shift);
    RUN(test_bridge_three_chunks_long_call_no_text_shift);
    RUN(test_bridge_chunks_ending_at_mark_no_text_shift);
    return test_summary();
}
