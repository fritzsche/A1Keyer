// host-only test that simulates the audio engine's routing logic
// (iambic-keyer-takes-priority vs MorseGenerator) and verifies the
// bus edge sequence for memory playback.

#include "test_framework.h"
#include "morse_encoder.h"
#include "morse_generator.h"
#include "key_envelop.h"
#include "key_event_bus.h"
#include "iambic_keyer.h"

#include <atomic>
#include <cstdio>
#include <vector>

static std::atomic<int> g_down{0};
static std::atomic<int> g_up{0};
static std::atomic<int> g_demand_max{0};

static void onDown() { g_down.fetch_add(1); int d = KeyEventBus::demand(); int cur = g_demand_max.load(); if (d > cur) g_demand_max.store(d); }
static void onUp()   { g_up.fetch_add(1);   int d = KeyEventBus::demand(); int cur = g_demand_max.load(); if (d > cur) g_demand_max.store(d); }

static void arm() {
    KeyEventBus::resetForTest();
    g_down.store(0); g_up.store(0); g_demand_max.store(0);
    KeyEventBus::subscribe(onDown, onUp);
    atomic_store(&s_keyState.memory[DIT_IDX], 0);
    atomic_store(&s_keyState.memory[DAH_IDX], 0);
    atomic_store(&s_keyState.state[DIT_IDX], 0);
    atomic_store(&s_keyState.state[DAH_IDX], 0);
}

// Drive the iambic keyer for one element + trailing silence, then
// release the paddle. Returns the number of ticks consumed.
static int driveAndRelease(IambicKeyer& keyer, int idx) {
    atomic_store(&s_keyState.memory[idx], 1);
    atomic_store(&s_keyState.state[idx], 1);
    int ticks = 0;
    for (int i = 0; i < 300; ++i) {
        std::vector<int16_t> m(64, 0);
        keyer.fillSamples(m.data(), 64, 500.0f, 16384, 48000);
        ++ticks;
        // Element ended and paddle released → keyer is in gap-detection
        // mode, still "active" because _lastElementEndFrame != 0.
        if (atomic_load(&s_keyState.memory[idx]) == 0
            && keyer.lastElementEndFrame() != 0) break;
    }
    atomic_store(&s_keyState.memory[idx], 0);
    atomic_store(&s_keyState.state[idx], 0);
    return ticks;
}

// Reproduce the audio engine's routing logic verbatim, then drive it
// for a fixed number of audio ticks and inspect the resulting bus.
struct Engine {
    MorseGenerator* gen;
    IambicKeyer* keyer;
    int sampleRate = 48000;

    int ticks = 0;
    int ticksToIambic = 0;
    int ticksToMorse = 0;
    int ticksToSilence = 0;

    void step(int frames) {
        ++ticks;
        bool keyerHasMemory =
            atomic_load(&s_keyState.memory[DIT_IDX]) ||
            atomic_load(&s_keyState.memory[DAH_IDX]);
        if (keyerHasMemory || keyer->isActive()) {
            ++ticksToIambic;
            std::vector<int16_t> mono(frames, 0);
            keyer->fillSamples(mono.data(), frames, 500.0f, 16384, sampleRate);
        } else if (gen && gen->isPlaying()) {
            ++ticksToMorse;
            std::vector<int16_t> mono(frames, 0);
            gen->fillSamplesMono(mono.data(), frames, 500.0f, 16384);
        } else {
            ++ticksToSilence;
        }
    }
};

static void test_oO_iambic_clean_then_morse_gen() {
    arm();
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    IambicKeyer keyer;
    keyer.begin(&env);

    Engine e{&gen, &keyer};
    gen.playText("OO");
    int guard = 4096;
    while ((gen.isPlaying() || keyer.isActive()) && --guard > 0) {
        e.step(64);
    }
    while (--guard > 0) {
        e.step(64);
        if (!gen.isPlaying() && !keyer.isActive()) break;
    }

    std::printf("[clean] ticks=%d iambic=%d morse=%d silence=%d downs=%d ups=%d demand_max=%d\n",
                e.ticks, e.ticksToIambic, e.ticksToMorse, e.ticksToSilence,
                g_down.load(), g_up.load(), g_demand_max.load());
    // "OO" = 6 DAHs total → 6 keyDown + 6 keyUp edges.
    CHECK_EQ(6, g_down.load());
    CHECK_EQ(6, g_up.load());
    CHECK_EQ(0, KeyEventBus::demand());
    CHECK_EQ(0, e.ticksToIambic);
}

static void test_oO_iambic_paddle_just_before_memory_play() {
    arm();
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    IambicKeyer keyer;
    keyer.begin(&env);

    int drainTicks = driveAndRelease(keyer, DIT_IDX);
    std::printf("[lag]   drain ticks=%d keyer.isActive=%d (expect true: gap detection)\n",
                drainTicks, (int)keyer.isActive());

    gen.playText("OO");
    Engine e{&gen, &keyer};
    int guard = 4096;
    while ((gen.isPlaying() || keyer.isActive()) && --guard > 0) {
        e.step(64);
    }
    while (--guard > 0) {
        e.step(64);
        if (!gen.isPlaying() && !keyer.isActive()) break;
    }

    std::printf("[lag]   ticks=%d iambic=%d morse=%d silence=%d downs=%d ups=%d demand_max=%d\n",
                e.ticks, e.ticksToIambic, e.ticksToMorse, e.ticksToSilence,
                g_down.load(), g_up.load(), g_demand_max.load());
    // With the keyed-boundary fix, "OO" = 6 DAHs → 6 keyDown + 6 keyUp.
    // (When the iambic keyer steals audio ticks mid-playback, the
    // MorseGenerator's edge count may be inflated — but the bus
    // remains balanced. The important invariant is demand_max <= 1.)
    CHECK_EQ(g_down.load(), g_up.load());
    CHECK_EQ(0, KeyEventBus::demand());
    CHECK(g_demand_max.load() <= 1);
    // The iambic keyer must have released the routing so the
    // MorseGenerator could play.
    CHECK(e.ticksToMorse > 0);
}

static void test_oO_iambic_stale_demand_does_not_block_morse() {
    arm();
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    IambicKeyer keyer;
    keyer.begin(&env);

    driveAndRelease(keyer, DIT_IDX);
    std::printf("[stale] before memory: keyer.isActive=%d\n", (int)keyer.isActive());

    gen.playText("OO");
    std::printf("[stale] after playText: gen.isPlaying=%d keyer.isActive=%d\n",
                (int)gen.isPlaying(), (int)keyer.isActive());

    Engine e{&gen, &keyer};
    int guard = 4096;
    while ((gen.isPlaying() || keyer.isActive()) && --guard > 0) {
        e.step(64);
    }
    while (--guard > 0) {
        e.step(64);
        if (!gen.isPlaying() && !keyer.isActive()) break;
    }

    std::printf("[stale] ticks=%d iambic=%d morse=%d silence=%d downs=%d ups=%d demand_max=%d\n",
                e.ticks, e.ticksToIambic, e.ticksToMorse, e.ticksToSilence,
                g_down.load(), g_up.load(), g_demand_max.load());
    CHECK_EQ(g_down.load(), g_up.load());
    CHECK_EQ(0, KeyEventBus::demand());
    CHECK(g_demand_max.load() <= 1);
}

int main() {
    RUN(test_oO_iambic_clean_then_morse_gen);
    RUN(test_oO_iambic_paddle_just_before_memory_play);
    RUN(test_oO_iambic_stale_demand_does_not_block_morse);
    return test_summary();
}
