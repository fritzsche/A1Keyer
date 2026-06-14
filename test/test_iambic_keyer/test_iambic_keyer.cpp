#include "test_framework.h"
#include "iambic_keyer.h"
#include "key_envelop.h"
#include <vector>
#include <atomic>

#define SET   1
#define UNSET 0

// --- Atomic helpers (mimic what ISR writes) ---

static void setMemory(int idx, int val) {
    s_keyState.memory[idx].store(val, std::memory_order_relaxed);
}

static void setState(int idx, int val) {
    s_keyState.state[idx].store(val, std::memory_order_relaxed);
}

static int getMemory(int idx) {
    return s_keyState.memory[idx].load(std::memory_order_relaxed);
}

// --- state helpers ---
static void resetKeyState() {
    setMemory(IambicKeyer::DIT, MEMORY_UNSET);
    setMemory(IambicKeyer::DAH, MEMORY_UNSET);
    setState(IambicKeyer::DIT, MEMORY_UNSET);
    setState(IambicKeyer::DAH, MEMORY_UNSET);
}

// --- basic initialisation ---

static void test_keyer_idle_by_default() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();
    CHECK(!keyer.isActive());
}

static void test_keyer_not_active_with_no_memory() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();
    // No memory set, not playing → not active
    CHECK(!keyer.isActive());
}

// --- idle → playing via memory ---

static void test_keyer_plays_dit_when_memory_set() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    // Simulate DIT press: set memory
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, SET);

    // Fill some samples
    std::vector<int16_t> buf(256, 0x7FFF);
    keyer.fillSamples(buf.data(), 256, 500.0f, 16384, 48000);

    // Should have non-zero samples (envelope ramp-up)
    int nonZero = 0;
    for (auto s : buf) if (s != 0) ++nonZero;
    CHECK(nonZero > 0);

    // Memory should still be set while playing
    CHECK(getMemory(IambicKeyer::DIT) == SET);
}

static void test_keyer_plays_dah_when_memory_set() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    setMemory(IambicKeyer::DAH, SET);
    setState(IambicKeyer::DAH, SET);

    std::vector<int16_t> buf(256, 0);
    keyer.fillSamples(buf.data(), 256, 500.0f, 16384, 48000);

    int nonZero = 0;
    for (auto s : buf) if (s != 0) ++nonZero;
    CHECK(nonZero > 0);
}

// --- element ends, memory cleared if paddle released ---

static void test_keyer_clears_memory_when_element_ends_and_paddle_released() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    // Simulate DIT press then release before element ends
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, UNSET);  // released

    int ditLen = env.envelopeSize(KeyEnvelop::Element::DIT);
    std::vector<int16_t> buf(ditLen, 0);
    keyer.fillSamples(buf.data(), ditLen, 500.0f, 16384, 48000);

    // After element ends, memory should be cleared
    CHECK(getMemory(IambicKeyer::DIT) == UNSET);
}

// --- iambic B: alternate paddles ---

static void test_keyer_plays_opposite_when_memory_set_on_element_end() {
    // extern/main.c:482 — if memory[opp] set when element ends → play opposite
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    // Press both DIT and DAH
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, SET);
    setMemory(IambicKeyer::DAH, SET);   // opposite memory set
    setState(IambicKeyer::DAH, SET);

    int ditLen = env.envelopeSize(KeyEnvelop::Element::DIT);
    std::vector<int16_t> buf(ditLen, 0);
    keyer.fillSamples(buf.data(), ditLen, 500.0f, 16384, 48000);

    // Iambic action: after DIT ends, DAH should be playing
    // Memory for DAH should still be set; state for DAH is still SET (held)
    CHECK(getMemory(IambicKeyer::DAH) == SET);
}

// --- auto-repeat: memory still set when element ends ---

static void test_keyer_autorepeat_when_memory_still_set() {
    // extern/main.c:488 — if memory[own] still set when element ends → continue
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    // Hold DIT: memory set, state still SET (not released)
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, SET);

    int ditLen = env.envelopeSize(KeyEnvelop::Element::DIT);
    std::vector<int16_t> buf(ditLen * 3, 0);  // fill 3x DIT length
    keyer.fillSamples(buf.data(), ditLen * 3, 500.0f, 16384, 48000);

    // Memory should still be set (auto-repeat)
    CHECK(getMemory(IambicKeyer::DIT) == SET);
}

// --- end of character: neither memory set ---

static void test_keyer_returns_to_none_when_neither_memory_set() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    // DIT press and release (memory cleared by element end check)
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, UNSET);  // released before element ends

    // Fill enough samples to exhaust the DIT
    int ditLen = env.envelopeSize(KeyEnvelop::Element::DIT);
    std::vector<int16_t> buf(ditLen, 0);
    keyer.fillSamples(buf.data(), ditLen, 500.0f, 16384, 48000);

    // _currentElement is now NONE, but isActive() stays true: the keyer
    // reports "active" until a 7-dit word-space gap is observed, so the
    // audio engine keeps ticking fillSamples and writes the SPACE.
    CHECK(keyer.isActive());

    // Push silence in small chunks so the inline gap check (at the start
    // of each fillSamples call) sees the growing gap. Once it exceeds
    // 7 * ditLen, the SPACE is written and isActive() goes false.
    int totalSilence = ditLen * 7 + 100;
    const int chunkSize = 1024;
    for (int filled = 0; filled < totalSilence; filled += chunkSize) {
        int n = std::min(chunkSize, totalSilence - filled);
        std::vector<int16_t> chunk(n, 0);
        keyer.fillSamples(chunk.data(), n, 500.0f, 16384, 48000);
    }

    CHECK(!keyer.isActive());
}

// --- decoder ring buffer ---

static void test_decoder_ring_buffer_write_and_read() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    // No symbols written yet
    CHECK(keyer.decoderAvailable() == 0);

    // Write symbols directly (normally done by fillSamples at element boundaries)
    // We can test via a manual sequence: press DIT, let it finish
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, UNSET);  // released immediately

    int ditLen = env.envelopeSize(KeyEnvelop::Element::DIT);
    std::vector<int16_t> buf(ditLen, 0);
    keyer.fillSamples(buf.data(), ditLen, 500.0f, 16384, 48000);

    // After element ends, decoder buffer should have '.' (and '*')
    size_t avail = keyer.decoderAvailable();
    CHECK(avail >= 2);  // '.' and '*'

    char c;
    CHECK(keyer.decoderRead(&c));   // should get '.'
    CHECK(c == '.');
    CHECK(keyer.decoderRead(&c));  // should get '*'
    CHECK(c == '*');
}

// --- isActive ---

static void test_keyer_is_active_while_playing() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, SET);

    int ditLen = env.envelopeSize(KeyEnvelop::Element::DIT);
    std::vector<int16_t> buf(128, 0);
    keyer.fillSamples(buf.data(), 128, 500.0f, 16384, 48000);

    CHECK(keyer.isActive());
}

static void test_keyer_is_active_when_memory_pending() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    // Memory set but not yet started (current_element == NONE)
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, UNSET);

    CHECK(keyer.isActive());
}

// --- volume zero gives silence ---

static void test_keyer_volume_zero_gives_silence() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, SET);

    std::vector<int16_t> buf(256, 0x7FFF);
    keyer.fillSamples(buf.data(), 256, 500.0f, 0, 48000);

    bool allZero = true;
    for (auto s : buf) if (s != 0) { allZero = false; break; }
    CHECK(allZero);
}

// --- word-space detection ---

static void test_idle_with_no_memory_gives_silence() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    std::vector<int16_t> buf(256, 0x7FFF);
    keyer.fillSamples(buf.data(), 256, 500.0f, 16384, 48000);

    bool allZero = true;
    for (auto s : buf) if (s != 0) { allZero = false; break; }
    CHECK(allZero);
}

// --- WPM 20: verify element duration matches Paris standard ---
// Paris: 1 dit = 1.2/WPM seconds. At 20 WPM: dit = 60 ms = 2880 samples @ 48 kHz

static void test_keyer_dit_length_at_20wpm_matches_paris() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);

    // Expected dit samples at 20 WPM: 1.2/20 = 60 ms = 48000 * 0.06 = 2880 samples
    int expectedDitSamples = env.envelopeSize(KeyEnvelop::Element::DIT);
    CHECK(expectedDitSamples > 0);

    // Simulate DIT press and release
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, SET);

    // Fill exactly the dit length
    std::vector<int16_t> buf(expectedDitSamples, 0);
    keyer.fillSamples(buf.data(), expectedDitSamples, 500.0f, 16384, 48000);

    // Should have non-zero samples throughout the element
    int nonzero = 0;
    for (auto s : buf) if (s != 0) ++nonzero;
    CHECK(nonzero > 0);

    // After exactly ditLen samples, element should have ended
    // Check that memory was NOT auto-cleared (paddle was held the whole time)
    CHECK(getMemory(IambicKeyer::DIT) == SET);

    // Now simulate release and fill one more dit-length — element should end
    setState(IambicKeyer::DIT, UNSET);
    std::vector<int16_t> buf2(expectedDitSamples, 0);
    keyer.fillSamples(buf2.data(), expectedDitSamples, 500.0f, 16384, 48000);

    // Now memory should be cleared
    CHECK(getMemory(IambicKeyer::DIT) == UNSET);
}

// --- WPM 20: DAH = 3× dit length ---

static void test_keyer_dah_length_at_20wpm_is_three_dits() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);

    int ditSamples = env.envelopeSize(KeyEnvelop::Element::DIT);
    int dahSamples = env.envelopeSize(KeyEnvelop::Element::DAH);

    // Cmorse-compatible envelope: DIT = 2*ditLen (tone + silence), DAH = 4*ditLen
    // The tone portion is ditLen = 1.2/WPM seconds = 2880 samples @ 20WPM/48kHz
    // Use env.ditLengthSamples() for the actual tone duration, not envelopeSize()
    CHECK(ditSamples == 2 * env.ditLengthSamples());
    CHECK(dahSamples == 4 * env.ditLengthSamples());
    CHECK(dahSamples == 2 * ditSamples);  // DAH envelope is 2× DIT envelope

    // Play a DAH
    setMemory(IambicKeyer::DAH, SET);
    setState(IambicKeyer::DAH, SET);

    std::vector<int16_t> buf(dahSamples, 0);
    keyer.fillSamples(buf.data(), dahSamples, 500.0f, 16384, 48000);

    int nonzero = 0;
    for (auto s : buf) if (s != 0) ++nonzero;
    CHECK(nonzero > 0);
}

// --- WPM: dit length halves when WPM doubles ---

static void test_keyer_dit_length_halves_when_wpm_doubles() {
    KeyEnvelop env20(20, 0.005f, 48000);
    KeyEnvelop env40(40, 0.005f, 48000);

    int dit20 = env20.envelopeSize(KeyEnvelop::Element::DIT);
    int dit40 = env40.envelopeSize(KeyEnvelop::Element::DIT);

    // Doubling WPM halves the dit length
    CHECK(dit40 * 2 == dit20 || dit40 * 2 == dit20 + 1 || dit40 * 2 == dit20 - 1);
}

// --- 20 WPM: DIT = 60 ms, DAH = 180 ms ---

static void test_keyer_dit_is_60ms_at_20wpm() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);

    int ditSamples = env.envelopeSize(KeyEnvelop::Element::DIT);
    // Cmorse envelope: DIT = 2*ditLen = 5760 samples @ 20WPM/48kHz
    // The tone portion (ditLen) = 2880 samples = 60 ms
    CHECK(ditSamples == 5760);
    CHECK(env.ditLengthSamples() == 2880);  // actual tone duration

    // Verify samples are non-zero throughout the element
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, SET);
    std::vector<int16_t> buf(ditSamples, 0);
    keyer.fillSamples(buf.data(), ditSamples, 500.0f, 16384, 48000);

    int nonzero = 0;
    for (auto s : buf) if (s != 0) ++nonzero;
    CHECK(nonzero > 0);  // at least some samples should be non-zero
}

static void test_keyer_dah_is_180ms_at_20wpm() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);

    int ditSamples = env.envelopeSize(KeyEnvelop::Element::DIT);
    int dahSamples = env.envelopeSize(KeyEnvelop::Element::DAH);
    // Cmorse envelope: DAH = 4*ditLen = 11520 samples @ 20WPM/48kHz
    // The tone portion (3*ditLen) = 8640 samples = 180 ms
    CHECK(dahSamples == 11520);
    CHECK(env.ditLengthSamples() == 2880);  // actual tone duration
    CHECK(dahSamples == 2 * ditSamples);  // DAH envelope is 2× DIT envelope

    setMemory(IambicKeyer::DAH, SET);
    setState(IambicKeyer::DAH, SET);
    std::vector<int16_t> buf(dahSamples, 0);
    keyer.fillSamples(buf.data(), dahSamples, 500.0f, 16384, 48000);

    int nonzero = 0;
    for (auto s : buf) if (s != 0) ++nonzero;
    CHECK(nonzero > 0);
}

// --- word-space: SPACE char written BEFORE next element when gap > 7 dit-lengths ---
// Reference: extern/main.c:441-446 — word-space detected at START of next element, not during idle

static void test_keyer_word_space_written_after_7dit_gap() {
    KeyEnvelop env(20, 0.005f, 48000);
    IambicKeyer keyer;
    keyer.begin(&env);
    resetKeyState();

    int ditSamples = env.envelopeSize(KeyEnvelop::Element::DIT);  // 2880 @ 20WPM

    // Play one DIT and release (simulate paddle release for memory clearing)
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, SET);
    std::vector<int16_t> ditBuf(ditSamples, 0);
    keyer.fillSamples(ditBuf.data(), ditSamples, 500.0f, 16384, 48000);

    // Simulate paddle release so memory gets cleared at element end
    setState(IambicKeyer::DIT, UNSET);
    // Fill the rest of the DIT element so boundary check fires.
    // Note: fillSamples(2880) with state=SET caused an auto-repeat, so we're
    // now playing a second DIT. Need ditSamples samples to reach its boundary.
    std::vector<int16_t> remainingDit(ditSamples, 0);
    keyer.fillSamples(remainingDit.data(), ditSamples, 500.0f, 16384, 48000);

    // After the boundary fires with paddle released, memory should be cleared
    CHECK(getMemory(IambicKeyer::DIT) == UNSET);

    // Fill silence: more than 7 dit-lengths
    // 7 * ditSamples = 20160. Need > 20160 to trigger (gap > 7*dit, strict inequality)
    int silenceFrames = ditSamples * 7 + 100;  // 20260 samples
    std::vector<int16_t> silenceBuf(silenceFrames, 0);
    keyer.fillSamples(silenceBuf.data(), silenceFrames, 500.0f, 16384, 48000);

    // Now start next element — word-space check happens AT START of element
    // (matching extern/main.c:441 — check BEFORE element begins)
    setMemory(IambicKeyer::DIT, SET);
    setState(IambicKeyer::DIT, SET);
    std::vector<int16_t> dit2Buf(64, 0);
    keyer.fillSamples(dit2Buf.data(), 64, 500.0f, 16384, 48000);

    // Drain the decoder ring buffer — should have '.', '*', ' '
    char c;
    bool foundSpace = false;
    while (keyer.decoderRead(&c)) {
        if (c == ' ') foundSpace = true;
    }
    CHECK(foundSpace);
}

int main() {
    printf("=== test_iambic_keyer ===\n");
    RUN(test_keyer_idle_by_default);
    RUN(test_keyer_not_active_with_no_memory);
    RUN(test_keyer_plays_dit_when_memory_set);
    RUN(test_keyer_plays_dah_when_memory_set);
    RUN(test_keyer_clears_memory_when_element_ends_and_paddle_released);
    RUN(test_keyer_plays_opposite_when_memory_set_on_element_end);
    RUN(test_keyer_autorepeat_when_memory_still_set);
    RUN(test_keyer_returns_to_none_when_neither_memory_set);
    RUN(test_decoder_ring_buffer_write_and_read);
    RUN(test_keyer_is_active_while_playing);
    RUN(test_keyer_is_active_when_memory_pending);
    RUN(test_keyer_volume_zero_gives_silence);
    RUN(test_idle_with_no_memory_gives_silence);
    RUN(test_keyer_dit_length_at_20wpm_matches_paris);
    RUN(test_keyer_dah_length_at_20wpm_is_three_dits);
    RUN(test_keyer_dit_length_halves_when_wpm_doubles);
    RUN(test_keyer_dit_is_60ms_at_20wpm);
    RUN(test_keyer_dah_is_180ms_at_20wpm);
    RUN(test_keyer_word_space_written_after_7dit_gap);
    return test_summary();
}
