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
static std::atomic<bool> g_high{false};
static std::atomic<int> g_highEdges{0};
static std::atomic<int> g_lowEdges{0};

static void onDown() { g_down.fetch_add(1); g_highEdges.fetch_add(1); g_high.store(true); }
static void onUp()   { g_up.fetch_add(1);   g_lowEdges.fetch_add(1);  g_high.store(false); }

static void arm() {
    KeyEventBus::resetForTest();
    g_down.store(0); g_up.store(0); g_high.store(false);
    g_highEdges.store(0); g_lowEdges.store(0);
    KeyEventBus::subscribe(onDown, onUp);
}

static void dumpElements(const char* label, const std::vector<MorseEncoder::Element>& seq) {
    std::printf("[%s] %zu elements:", label, seq.size());
    for (auto& e : seq) {
        const char* t = "?";
        switch (e.type) {
            case MorseEncoder::Element::DIT:        t = "DIT";  break;
            case MorseEncoder::Element::DAH:        t = "DAH";  break;
            case MorseEncoder::Element::CHAR_SPACE: t = "CS";   break;
            case MorseEncoder::Element::WORD_SPACE: t = "WS";   break;
        }
        std::printf(" %s(u=%d,kd=%d)", t, e.units, (int)e.keyDown);
    }
    std::printf("\n");
}

// Just print what the encoder produces for the user-reported inputs.
static void test_print_encoder_output_for_OO_SS_AA() {
    MorseEncoder enc(20);
    auto oo = enc.encode("OO"); dumpElements("OO", oo);
    auto ss = enc.encode("SS"); dumpElements("SS", ss);
    auto aa = enc.encode("AA"); dumpElements("AA", aa);
    // sanity: 7 elements each (3 marks + 1 space + 3 marks)
    CHECK_EQ(7, (int)oo.size());
    CHECK_EQ(7, (int)ss.size());
    CHECK_EQ(5, (int)aa.size());  // 2 + 1 + 2
}

// Simulate a radio sink: count down/up edges from MorseGenerator. With
// the keyed-boundary fix in MorseGenerator::fillSamplesMono (mirrors
// iambic_keyer.cpp `_elementKeyedSamples`), each mark fires its own
// keyDown + keyUp at the keyed portion of its envelope — not at the
// envelope's full trailing silence boundary. So "OO" (3 DAHs × 2 chars
// = 6 DAHs) produces 6 downs + 6 ups, etc. Bus demand stays at 0.
static void runPlayback(const char* text) {
    arm();
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    gen.playText(text);
    std::vector<int16_t> buf(env.ditLengthSamples(), 0);
    int guard = 4096;
    while (gen.isPlaying() && --guard > 0) {
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    }
    std::printf("  [%s] downs=%d ups=%d demand=%d edges(high/low)=(%d/%d)\n",
                text, g_down.load(), g_up.load(), KeyEventBus::demand(),
                g_highEdges.load(), g_lowEdges.load());
}

// OO = 6 DAHs total → 6 keyDown + 6 keyUp edges.
static void test_playback_OO_produces_six_on_off_cycles() {
    runPlayback("OO");
    CHECK_EQ(6, g_down.load());
    CHECK_EQ(6, g_up.load());
    CHECK_EQ(0, KeyEventBus::demand());
}

// SS = 6 DITs total → 6 keyDown + 6 keyUp edges.
static void test_playback_SS_produces_six_on_off_cycles() {
    runPlayback("SS");
    CHECK_EQ(6, g_down.load());
    CHECK_EQ(6, g_up.load());
    CHECK_EQ(0, KeyEventBus::demand());
}

// AA = 4 marks (2 DITs + 2 DAHs) → 4 keyDown + 4 keyUp edges.
static void test_playback_AA_produces_four_on_off_cycles() {
    runPlayback("AA");
    CHECK_EQ(4, g_down.load());
    CHECK_EQ(4, g_up.load());
    CHECK_EQ(0, KeyEventBus::demand());
}

int main() {
    RUN(test_print_encoder_output_for_OO_SS_AA);
    RUN(test_playback_OO_produces_six_on_off_cycles);
    RUN(test_playback_SS_produces_six_on_off_cycles);
    RUN(test_playback_AA_produces_four_on_off_cycles);
    return test_summary();
}
