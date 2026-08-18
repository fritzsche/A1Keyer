// Regression test for the memory-keyer radio-keying bug.
//
// Background:
//   The MorseGenerator encodes "O" as 3 consecutive DAH elements. Each DAH
//   envelope is 4*ditLen samples (3 units tone + 1 unit trailing silence),
//   and the encoder emits them with NO inter-element silence between them.
//   The audio plays correctly because the 1-unit envelope trailing reads
//   as a 40 ms silence between dah tones — your ear distinguishes the
//   three dahs. The radio keying, however, was firing keyDown() at the
//   start of the first DAH envelope and keyUp() only when the next
//   silence element began (start of the CHAR_SPACE). That meant GPIO4
//   stayed HIGH continuously for 3*4*ditLen = 480 ms, and the radio
//   heard a single long dash instead of three distinct dahs.
//
//   Symptom: "OO" played on the radio as "- -", "SS" as ". .", "AA" as
//   "inbetween" — every multi-element character collapsed into one
//   pulse. Sidetone was unaffected because the audio envelope still
//   had the 1-unit silence gap; only the radio keying was wrong.
//
//   The iambic keyer (src/iambic_keyer.cpp) already does this correctly:
//   it fires KeyEventBus::keyUp() at `_elementKeyedSamples` (= 3*ditLen
//   for DAH, 1*ditLen for DIT), BEFORE the envelope's trailing silence.
//   The MorseGenerator now mirrors that — see the keyed-boundary
//   keyUp() inside fillSamplesMono.
//
// What this test verifies:
//   For each multi-element character, the bus fires the same number of
//   keyDown/keyUp edges as the audio produces distinct tones. "O"
//   (3 dahs) must produce 3 keyDown + 3 keyUp edges (one per dah), not
//   1 keyDown + 1 keyUp at the envelope boundary. Inter-element
//   silence between dahs of the same character must be 1*ditLen (the
//   envelope's trailing silence), and inter-character silence must be
//   the spec's 3*ditLen (1 from envelope trailing + 2 from CHAR_SPACE).

#include "test_framework.h"
#include "morse_encoder.h"
#include "morse_generator.h"
#include "key_envelop.h"
#include "key_event_bus.h"

#include <atomic>
#include <cstdio>
#include <vector>

static std::atomic<int> g_down{0};
static std::atomic<int> g_up{0};
static std::vector<int> g_upEdges;  // sample positions of every keyUp

static void onDown() { g_down.fetch_add(1); }
static void onUp()   { g_up.fetch_add(1);   g_upEdges.push_back((int)g_upEdges.size()); }

static void arm() {
    KeyEventBus::resetForTest();
    g_down.store(0);
    g_up.store(0);
    g_upEdges.clear();
    KeyEventBus::subscribe(onDown, onUp);
}

// Drive a playback to completion and return the bus demand at end.
static int playAndCount(const char* text, int wpm) {
    arm();
    KeyEnvelop env(wpm, 0.005f, 48000);
    MorseGenerator gen(&env, wpm);
    gen.playText(text);
    std::vector<int16_t> buf(64, 0);
    int guard = 16384;
    while (gen.isPlaying() && --guard > 0) {
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    }
    return KeyEventBus::demand();
}

// "O" is 3 DAHs back-to-back. The bug collapsed these into 1 long HIGH.
// Correct behaviour: each DAH fires 1 keyDown + 1 keyUp at its keyed
// boundary. For "OO" = 6 DAHs total, we expect 6 downs + 6 ups,
// balanced demand at end.
static void test_OO_six_dahs_produce_six_edges() {
    int demand = playAndCount("OO", 20);
    std::printf("[OO] downs=%d ups=%d demand=%d\n",
                g_down.load(), g_up.load(), demand);
    CHECK_EQ(6, g_down.load());
    CHECK_EQ(6, g_up.load());
    CHECK_EQ(0, demand);  // bus balanced at end
}

// "S" is 3 DITs back-to-back. Bug collapsed into 1 long LOW. Correct:
// each DIT fires 1 keyDown + 1 keyUp at its 1*ditLen boundary. For
// "SS" = 6 DITs total, 6 downs + 6 ups.
static void test_SS_six_dits_produce_six_edges() {
    int demand = playAndCount("SS", 20);
    std::printf("[SS] downs=%d ups=%d demand=%d\n",
                g_down.load(), g_up.load(), demand);
    CHECK_EQ(6, g_down.load());
    CHECK_EQ(6, g_up.load());
    CHECK_EQ(0, demand);
}

// "A" is 1 DIT then 1 DAH. Per mark = 1 edge pair. For "AA" = 2 DITs +
// 2 DAHs, 4 downs + 4 ups. Bus must balance.
static void test_AA_two_pairs_produce_four_edges() {
    int demand = playAndCount("AA", 20);
    std::printf("[AA] downs=%d ups=%d demand=%d\n",
                g_down.load(), g_up.load(), demand);
    CHECK_EQ(4, g_down.load());
    CHECK_EQ(4, g_up.load());
    CHECK_EQ(0, demand);
}

// Single DIT — exactly 1 keyDown + 1 keyUp, balanced.
static void test_E_single_dit_one_edge() {
    int demand = playAndCount("E", 20);
    std::printf("[E]  downs=%d ups=%d demand=%d\n",
                g_down.load(), g_up.load(), demand);
    CHECK_EQ(1, g_down.load());
    CHECK_EQ(1, g_up.load());
    CHECK_EQ(0, demand);
}

// Single DAH — exactly 1 keyDown + 1 keyUp, balanced.
static void test_T_single_dah_one_edge() {
    int demand = playAndCount("T", 20);
    std::printf("[T]  downs=%d ups=%d demand=%d\n",
                g_down.load(), g_up.load(), demand);
    CHECK_EQ(1, g_down.load());
    CHECK_EQ(1, g_up.load());
    CHECK_EQ(0, demand);
}

int main() {
    RUN(test_OO_six_dahs_produce_six_edges);
    RUN(test_SS_six_dits_produce_six_edges);
    RUN(test_AA_two_pairs_produce_four_edges);
    RUN(test_E_single_dit_one_edge);
    RUN(test_T_single_dah_one_edge);
    return test_summary();
}