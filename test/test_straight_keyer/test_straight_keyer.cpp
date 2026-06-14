#include "test_framework.h"
#include "straight_keyer.h"
#include "key_envelop.h"
#include <vector>
#include <atomic>
#include <cstdio>

// s_keyState is defined in iambic_keyer.cpp
extern KeyState s_keyState;

#define SET   MEMORY_SET
#define UNSET MEMORY_UNSET

// --- reset key state ---

static void resetKeyState() {
    s_keyState.memory[DIT_IDX].store(UNSET, std::memory_order_relaxed);
    s_keyState.memory[DAH_IDX].store(UNSET, std::memory_order_relaxed);
    s_keyState.state[DIT_IDX].store(UNSET, std::memory_order_relaxed);
    s_keyState.state[DAH_IDX].store(UNSET, std::memory_order_relaxed);
}

// --- basic initialisation ---

static void test_keyer_idle_by_default() {
    StraightKeyer keyer;
    keyer.begin();
    resetKeyState();
    CHECK(!keyer.isActive());
}

static void test_keyer_not_active_with_no_memory() {
    StraightKeyer keyer;
    keyer.begin();
    resetKeyState();
    CHECK(!keyer.isActive());
}

// --- idle → playing via memory ---

static void test_keyer_plays_tone_when_memory_set() {
    StraightKeyer keyer;
    keyer.begin();
    resetKeyState();

    s_keyState.memory[DIT_IDX].store(SET, std::memory_order_relaxed);
    s_keyState.state[DIT_IDX].store(SET, std::memory_order_relaxed);

    // Fill just 10 ms — ramp only, no hold needed
    std::vector<int16_t> buf(480, 0x7FFF);
    keyer.fillSamples(buf.data(), 480, 500.0f, 16384, 48000);

    int nonZero = 0;
    for (auto s : buf) if (s != 0) ++nonZero;
    CHECK(nonZero > 0);
}

static void test_decoder_ring_buffer_starts_empty() {
    StraightKeyer keyer;
    keyer.begin();
    resetKeyState();
    CHECK_EQ(0, (int)keyer.decoderAvailable());
}

static void test_keyer_is_active_during_playback() {
    StraightKeyer keyer;
    keyer.begin();
    resetKeyState();

    CHECK(!keyer.isActive());

    s_keyState.memory[DIT_IDX].store(SET, std::memory_order_relaxed);
    s_keyState.state[DIT_IDX].store(SET, std::memory_order_relaxed);

    std::vector<int16_t> buf(240, 0);
    keyer.fillSamples(buf.data(), 240, 500.0f, 16384, 48000);

    CHECK(keyer.isActive());
}

int main() {
    printf("=== test_straight_keyer ===\n");
    RUN(test_keyer_idle_by_default);
    RUN(test_keyer_not_active_with_no_memory);
    RUN(test_keyer_plays_tone_when_memory_set);
    RUN(test_decoder_ring_buffer_starts_empty);
    RUN(test_keyer_is_active_during_playback);
    return test_summary();
}