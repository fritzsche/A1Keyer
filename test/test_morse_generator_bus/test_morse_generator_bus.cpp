#include "test_framework.h"
#include "key_event_bus.h"
#include "key_envelop.h"
#include "morse_generator.h"

#include <atomic>
#include <vector>

// ─── helpers ────────────────────────────────────────────────────────────────
//
// KeyEventBus::subscribe takes void(*)() — raw, non-capturing function
// pointers. We mirror the pattern from test_key_event_bus: file-static
// counters and free functions, re-armed per test.

static std::atomic<int> g_downCount{0};
static std::atomic<int> g_upCount{0};

static void onDownSpy() { g_downCount.fetch_add(1, std::memory_order_relaxed); }
static void onUpSpy()   { g_upCount.fetch_add(1, std::memory_order_relaxed); }

static void armSpy() {
    KeyEventBus::resetForTest();
    g_downCount.store(0);
    g_upCount.store(0);
    KeyEventBus::subscribe(onDownSpy, onUpSpy);
}

static int downCount() { return g_downCount.load(std::memory_order_relaxed); }
static int upCount()   { return g_upCount.load(std::memory_order_relaxed); }

// Drain the generator by repeatedly calling fillSamplesMono until
// isPlaying() goes false. Mimics the audio task on Core 1.
static void drain(MorseGenerator& gen, int ditLenSamples) {
    std::vector<int16_t> buf(ditLenSamples, 0);
    int guard = 1024;
    while (gen.isPlaying() && --guard > 0) {
        gen.fillSamplesMono(buf.data(), buf.size(), 500.0f, 16384);
    }
}

// ─── baseline: no events until something plays ──────────────────────────────

static void test_no_events_when_idle() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    CHECK_EQ(0, downCount());
    CHECK_EQ(0, upCount());
    CHECK_EQ(0, KeyEventBus::demand());

    gen.playText("");
    CHECK_EQ(0, downCount());
    CHECK_EQ(0, upCount());
    CHECK_EQ(0, KeyEventBus::demand());
}

// ─── single-element playback ────────────────────────────────────────────────

static void test_single_dit_one_down_one_up() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    gen.playText("E");
    CHECK(gen.isPlaying());
    // playText's single advance sets up the first DIT → fires keyDown.
    CHECK_EQ(1, downCount());
    CHECK_EQ(0, upCount());
    CHECK_EQ(1, KeyEventBus::demand());

    drain(gen, env.ditLengthSamples());
    CHECK(!gen.isPlaying());
    // Natural completion fires keyUp on the exhausted branch.
    CHECK_EQ(1, downCount());
    CHECK_EQ(1, upCount());
    CHECK_EQ(0, KeyEventBus::demand());
}

static void test_single_dah_one_down_one_up() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    gen.playText("T");                       // DAH only
    drain(gen, env.ditLengthSamples());
    CHECK(!gen.isPlaying());
    CHECK_EQ(1, downCount());
    CHECK_EQ(1, upCount());
    CHECK_EQ(0, KeyEventBus::demand());
}

// ─── multi-element: no double-down across adjacent marks ────────────────────

static void test_no_double_down_across_TU() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    // "TU" → DAH, CHAR_SPACE, DIT, DIT, DAH  (encoder dumps for "TU")
//   (no ELEMENT_SPACE between the two DITs of U — the inter-element
//    gap is the envelope's trailing silence, NOT a separate element.
//    This is the same architecture that produced the bug: GPIO4 was
//    staying HIGH through the env trailing, making the radio hear a
//    single long dash instead of two short ones.)
    gen.playText("TU");

    // playText's advance set up DAH → one keyDown, bus count == 1.
    CHECK_EQ(1, downCount());
    CHECK_EQ(0, upCount());
    CHECK_EQ(1, KeyEventBus::demand());

    drain(gen, env.ditLengthSamples());
    CHECK(!gen.isPlaying());

    // Sequence of edges (playText + drain) with the keyed-boundary fix:
    //   DAH:        0→1  (playText's first advance fired down)
    //   DAH at 3d:  1→0  (keyed-boundary keyUp fires at 3*ditLen;
    //                       radio LOW for 1*ditLen env trailing silence)
    //   CHAR_SPACE:    – (silence, radio stays LOW)
    //   DIT:        0→1  (advance to DIT fires down — _wasElKeyDown was
    //                       reset to false by the keyed-boundary keyUp,
    //                       so the next mark sees a fresh rising edge)
    //   DIT at 1d:  1→0  (keyed-boundary keyUp at 1*ditLen)
    //   DIT:        0→1  (advance to next DIT fires down)
    //   DIT at 1d:  1→0  (keyed-boundary keyUp at 1*ditLen)
    //   DAH:        0→1  (advance to DAH fires down)
    //   DAH at 3d:  1→0  (keyed-boundary keyUp at 3*ditLen)
    //   exhausted:     – (radio already unkeyed, no extra up)
    // Total: 4 downs + 4 ups. Bus demand NEVER exceeded 1.
    CHECK_EQ(4, downCount());
    CHECK_EQ(4, upCount());
    CHECK_EQ(0, KeyEventBus::demand());
}

// SOS = 12 marks across 3 characters. With the keyed-boundary fix
// (mirrors iambic_keyer.cpp), each mark fires its own keyDown + keyUp:
//   S first DIT:        DOWN  (advance)
//   S first DIT @ 1d:   UP    (keyed boundary)
//   S 2nd, 3rd DIT:    DOWN+UP each
//   CHAR_SPACE:         —     (silence, radio stays LOW)
//   O first DAH:        DOWN
//   O first DAH @ 3d:   UP
//   O 2nd, 3rd DAH:    DOWN+UP each
//   CHAR_SPACE:         —
//   S first DIT:        DOWN
//   ... (3 more DITs, each DOWN+UP)
//   exhausted:          —     (radio already unkeyed)
// Total: 9 DOWNS + 9 UPS. Bus demand NEVER exceeded 1.
static void test_no_double_down_across_NINE_marks() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    gen.playText("SOS");
    drain(gen, env.ditLengthSamples());

    CHECK_EQ(9, downCount());
    CHECK_EQ(9, upCount());
    CHECK_EQ(0, KeyEventBus::demand());
}

// The KEY invariant: the bus demand must stay at 1 for as long as
// the generator is mid-mark, no matter how many adjacent marks
// stack. We track the demand() each time the generator fires an
// advance — simple proxy for "the wiring leaked". SOS exercises 9
// marks back-to-back, so any drift would show up as demand != 1
// somewhere in the 9 advances.
static void test_bus_demand_never_exceeds_one_during_marks() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    gen.playText("SOS");
    drain(gen, env.ditLengthSamples());

    // Equal down and up, count separately tracked. The wiring's
    //      promise is "demand is the number of *open* marks".
    // Total marks = 9, total down = 3 (encoder never emits inter-
    // mark silence elements within a single character), so demand
    // was at most 1 the whole time. After drain() demand is 0.
    CHECK_EQ(downCount(), upCount());
    CHECK_EQ(0, KeyEventBus::demand());
}

// ─── stop() unkeys the radio ────────────────────────────────────────────────

static void test_stop_unkeys_mid_mark() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    gen.playText("E");
    CHECK_EQ(1, downCount());
    CHECK_EQ(1, KeyEventBus::demand());

    gen.stop();                              // operator aborts mid-DIT
    CHECK(!gen.isPlaying());
    CHECK_EQ(0, KeyEventBus::demand());      // stop() must fire keyUp
    CHECK_EQ(1, downCount());
    CHECK_EQ(1, upCount());
}

static void test_stop_when_already_idle_is_noop() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    // Never started, then stop() — must not fire keyUp.
    gen.stop();
    CHECK_EQ(0, upCount());
    CHECK_EQ(0, KeyEventBus::demand());

    // After a clean playback completes, stop() must not double-up.
    gen.playText("E");
    drain(gen, env.ditLengthSamples());
    int downBefore = downCount();
    int upBefore   = upCount();
    gen.stop();
    CHECK_EQ(downBefore, downCount());
    CHECK_EQ(upBefore,   upCount());
}

// ─── multiple playText() calls do not stack demands ─────────────────────────

static void test_sequential_play_text_resets_bus_to_zero_between() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    gen.playText("E");
    drain(gen, env.ditLengthSamples());
    CHECK_EQ(0, KeyEventBus::demand());

    gen.playText("T");
    drain(gen, env.ditLengthSamples());
    CHECK_EQ(0, KeyEventBus::demand());
    // Exactly one down and one up per playback — never leak.
    CHECK_EQ(2, downCount());
    CHECK_EQ(2, upCount());
}

// ─── repeated playback never leaks demand ───────────────────────────────────

static void test_repeated_play_keeps_demand_at_zero() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    for (int i = 0; i < 20; ++i) {
        const char* piece = (i % 2) ? "T" : "E";
        gen.playText(piece);
        drain(gen, env.ditLengthSamples());
        if (KeyEventBus::demand() != 0) {
            // Bail out so ctest still reports a meaningful value.
            CHECK_EQ(0, KeyEventBus::demand());
            break;
        }
    }
    CHECK_EQ(0, KeyEventBus::demand());
    CHECK_EQ(20, downCount());
    CHECK_EQ(20, upCount());
}

// ─── edge cases ────────────────────────────────────────────────────────────

static void test_play_then_immediate_stop_is_safe() {
    KeyEnvelop env(20, 0.005f, 48000);
    MorseGenerator gen(&env, 20);
    armSpy();

    gen.playText("E");
    gen.stop();                              // before any drain
    CHECK_EQ(0, KeyEventBus::demand());
    CHECK_EQ(1, downCount());                // playText advance set DIT
    CHECK_EQ(1, upCount());                  // stop() cleared it

    // A second play after the abort must work cleanly.
    gen.playText("T");
    drain(gen, env.ditLengthSamples());
    CHECK_EQ(0, KeyEventBus::demand());
    CHECK_EQ(2, downCount());
    CHECK_EQ(2, upCount());
}

int main() {
    printf("=== test_morse_generator_bus ===\n");
    RUN(test_no_events_when_idle);
    RUN(test_single_dit_one_down_one_up);
    RUN(test_single_dah_one_down_one_up);
    RUN(test_no_double_down_across_TU);
    RUN(test_no_double_down_across_NINE_marks);
    RUN(test_stop_unkeys_mid_mark);
    RUN(test_stop_when_already_idle_is_noop);
    RUN(test_sequential_play_text_resets_bus_to_zero_between);
    RUN(test_repeated_play_keeps_demand_at_zero);
    RUN(test_play_then_immediate_stop_is_safe);
    return test_summary();
}
