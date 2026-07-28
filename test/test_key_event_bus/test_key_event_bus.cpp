#include "test_framework.h"
#include "key_event_bus.h"

#include <atomic>

// --- global counters and sink callbacks ---
// KeyEventBus::subscribe takes void(*)() (raw function pointers), so
// the multi-sink tests use plain functions bound to per-sink globals
// and re-arm the counters in each test.

static std::atomic<int> g_downA{0};
static std::atomic<int> g_upA{0};
static std::atomic<int> g_downB{0};
static std::atomic<int> g_upB{0};
static std::atomic<int> g_downC{0};
static std::atomic<int> g_upC{0};

static void onDownA() { g_downA.fetch_add(1, std::memory_order_relaxed); }
static void onUpA()   { g_upA.fetch_add(1, std::memory_order_relaxed); }
static void onDownB() { g_downB.fetch_add(1, std::memory_order_relaxed); }
static void onUpB()   { g_upB.fetch_add(1, std::memory_order_relaxed); }
static void onDownC() { g_downC.fetch_add(1, std::memory_order_relaxed); }
static void onUpC()   { g_upC.fetch_add(1, std::memory_order_relaxed); }

static void resetAll() {
    KeyEventBus::resetForTest();
    g_downA.store(0);
    g_upA.store(0);
    g_downB.store(0);
    g_upB.store(0);
    g_downC.store(0);
    g_upC.store(0);
}

// --- reference-counted demand ---

static void test_balanced_keydown_keyup_fires_sinks() {
    resetAll();
    auto id = KeyEventBus::subscribe(onDownA, onUpA);
    CHECK(id != 0);

    KeyEventBus::keyDown();
    CHECK_EQ(g_downA.load(), 1);
    CHECK_EQ(g_upA.load(), 0);
    CHECK_EQ(KeyEventBus::demand(), 1);

    KeyEventBus::keyUp();
    CHECK_EQ(g_downA.load(), 1);
    CHECK_EQ(g_upA.load(), 1);
    CHECK_EQ(KeyEventBus::demand(), 0);
}

static void test_extra_keyup_below_zero_is_noop() {
    resetAll();
    KeyEventBus::subscribe(onDownA, onUpA);

    KeyEventBus::keyUp();
    KeyEventBus::keyUp();
    KeyEventBus::keyUp();
    CHECK_EQ(g_upA.load(), 0);
    CHECK_EQ(KeyEventBus::demand(), 0);
}

static void test_overlapping_demand_requires_matching_ups() {
    resetAll();
    KeyEventBus::subscribe(onDownA, onUpA);

    KeyEventBus::keyDown();  // 0->1, fires onDownA
    KeyEventBus::keyDown();  // 1->2, no event
    KeyEventBus::keyDown();  // 2->3, no event
    CHECK_EQ(g_downA.load(), 1);
    CHECK_EQ(g_upA.load(), 0);
    CHECK_EQ(KeyEventBus::demand(), 3);

    KeyEventBus::keyUp();    // 3->2, no event
    CHECK_EQ(g_upA.load(), 0);
    KeyEventBus::keyUp();    // 2->1, no event
    CHECK_EQ(g_upA.load(), 0);
    KeyEventBus::keyUp();    // 1->0, fires onUpA
    CHECK_EQ(g_upA.load(), 1);
    CHECK_EQ(KeyEventBus::demand(), 0);
}

static void test_force_all_up_drains_outstanding_demand() {
    resetAll();
    KeyEventBus::subscribe(onDownA, onUpA);

    KeyEventBus::keyDown();
    KeyEventBus::keyDown();
    KeyEventBus::keyDown();
    CHECK_EQ(g_downA.load(), 1);
    CHECK_EQ(g_upA.load(), 0);
    CHECK_EQ(KeyEventBus::demand(), 3);

    KeyEventBus::forceAllUp();
    CHECK_EQ(g_upA.load(), 1);
    CHECK_EQ(KeyEventBus::demand(), 0);

    // After forceAllUp, a fresh keyDown should fire onDownA again.
    KeyEventBus::keyDown();
    CHECK_EQ(g_downA.load(), 2);
    CHECK_EQ(KeyEventBus::demand(), 1);

    // And a real keyUp should fire onUpA again.
    KeyEventBus::keyUp();
    CHECK_EQ(g_upA.load(), 2);
    CHECK_EQ(KeyEventBus::demand(), 0);
}

static void test_force_all_up_when_idle_is_noop() {
    resetAll();
    KeyEventBus::subscribe(onDownA, onUpA);
    KeyEventBus::forceAllUp();
    CHECK_EQ(g_downA.load(), 0);
    CHECK_EQ(g_upA.load(), 0);
    CHECK_EQ(KeyEventBus::demand(), 0);
}

// --- multiple sinks ---

static void test_two_sinks_both_fire_on_edge() {
    resetAll();
    KeyEventBus::subscribe(onDownA, onUpA);
    KeyEventBus::subscribe(onDownB, onUpB);

    KeyEventBus::keyDown();
    CHECK_EQ(g_downA.load(), 1);
    CHECK_EQ(g_downB.load(), 1);

    KeyEventBus::keyUp();
    CHECK_EQ(g_upA.load(), 1);
    CHECK_EQ(g_upB.load(), 1);
}

// --- subscribe / unsubscribe ---

static void test_unsubscribe_stops_further_callbacks() {
    resetAll();
    auto id = KeyEventBus::subscribe(onDownA, onUpA);

    KeyEventBus::keyDown();
    KeyEventBus::keyUp();
    CHECK_EQ(g_downA.load(), 1);
    CHECK_EQ(g_upA.load(), 1);

    KeyEventBus::unsubscribe(id);

    KeyEventBus::keyDown();
    KeyEventBus::keyUp();
    // No additional callbacks after unsubscribe.
    CHECK_EQ(g_downA.load(), 1);
    CHECK_EQ(g_upA.load(), 1);
}

static void test_unsubscribe_invalid_token_is_noop() {
    resetAll();
    KeyEventBus::subscribe(onDownA, onUpA);
    KeyEventBus::unsubscribe(0);     // invalid
    KeyEventBus::unsubscribe(99999); // not present

    KeyEventBus::keyDown();
    KeyEventBus::keyUp();
    CHECK_EQ(g_downA.load(), 1);
    CHECK_EQ(g_upA.load(), 1);
}

static void test_unsubscribe_only_matching_id() {
    // Two sinks; unsubscribe one of them. The other must still fire.
    resetAll();
    auto idA = KeyEventBus::subscribe(onDownA, onUpA);
    auto idB = KeyEventBus::subscribe(onDownB, onUpB);
    CHECK(idA != 0);
    CHECK(idB != 0);
    CHECK(idA != idB);

    KeyEventBus::unsubscribe(idA);

    KeyEventBus::keyDown();
    KeyEventBus::keyUp();
    // idA removed: its counters should not advance.
    CHECK_EQ(g_downA.load(), 0);
    CHECK_EQ(g_upA.load(), 0);
    // idB still subscribed: its counters should advance.
    CHECK_EQ(g_downB.load(), 1);
    CHECK_EQ(g_upB.load(), 1);
}

static void test_subscribe_returns_distinct_ids() {
    resetAll();
    auto id1 = KeyEventBus::subscribe(onDownA, onUpA);
    auto id2 = KeyEventBus::subscribe(onDownA, onUpA);
    auto id3 = KeyEventBus::subscribe(onDownA, onUpA);
    CHECK(id1 != 0);
    CHECK(id2 != 0);
    CHECK(id3 != 0);
    CHECK(id1 != id2);
    CHECK(id2 != id3);
    CHECK(id1 != id3);
}

static void test_subscribe_table_full_returns_zero() {
    resetAll();
    KeyEventBus::SinkId ids[KeyEventBus::kMaxSinks];
    for (size_t i = 0; i < KeyEventBus::kMaxSinks; ++i) {
        ids[i] = KeyEventBus::subscribe(onDownA, onUpA);
        CHECK(ids[i] != 0);
    }
    auto overflow = KeyEventBus::subscribe(onDownA, onUpA);
    CHECK_EQ((int)overflow, 0);

    // Free one slot, then a new subscribe should succeed.
    KeyEventBus::unsubscribe(ids[2]);
    auto afterFree = KeyEventBus::subscribe(onDownA, onUpA);
    CHECK(afterFree != 0);
}

static void test_unsubscribe_compacts_table() {
    // After unsubscribe of a middle element, the last sink should still
    // receive transitions. This is a regression test for the
    // "remove first used regardless of id" bug.
    resetAll();
    auto idA = KeyEventBus::subscribe(onDownA, onUpA);    // sink[0]
    auto idB = KeyEventBus::subscribe(onDownB, onUpB);    // sink[1]
    auto idC = KeyEventBus::subscribe(onDownC, onUpC);    // sink[2]

    // Remove the middle sink.
    KeyEventBus::unsubscribe(idB);

    KeyEventBus::keyDown();
    KeyEventBus::keyUp();
    // A and C should both fire; B should not.
    CHECK_EQ(g_downA.load(), 1);
    CHECK_EQ(g_upA.load(), 1);
    CHECK_EQ(g_downB.load(), 0);
    CHECK_EQ(g_upB.load(), 0);
    CHECK_EQ(g_downC.load(), 1);
    CHECK_EQ(g_upC.load(), 1);

    // Unsubscribe A; C must still fire.
    KeyEventBus::unsubscribe(idA);
    KeyEventBus::keyDown();
    KeyEventBus::keyUp();
    CHECK_EQ(g_downA.load(), 1);
    CHECK_EQ(g_upA.load(), 1);
    CHECK_EQ(g_downC.load(), 2);
    CHECK_EQ(g_upC.load(), 2);
}

static void test_reset_for_test_clears_state() {
    // Add a sink, fire some events, then reset.
    KeyEventBus::subscribe(onDownA, onUpA);
    KeyEventBus::keyDown();
    KeyEventBus::keyDown();
    CHECK_EQ(KeyEventBus::demand(), 2);

    KeyEventBus::resetForTest();
    CHECK_EQ(KeyEventBus::demand(), 0);

    // After reset, the previous sink must be gone — fresh subscribe
    // gets a new id, and the prior callbacks must not fire.
    g_downA.store(0);
    g_upA.store(0);
    KeyEventBus::keyDown();
    KeyEventBus::keyUp();
    CHECK_EQ(g_downA.load(), 0);
    CHECK_EQ(g_upA.load(), 0);
}

int main() {
    printf("=== test_key_event_bus ===\n");
    RUN(test_balanced_keydown_keyup_fires_sinks);
    RUN(test_extra_keyup_below_zero_is_noop);
    RUN(test_overlapping_demand_requires_matching_ups);
    RUN(test_force_all_up_drains_outstanding_demand);
    RUN(test_force_all_up_when_idle_is_noop);
    RUN(test_two_sinks_both_fire_on_edge);
    RUN(test_unsubscribe_stops_further_callbacks);
    RUN(test_unsubscribe_invalid_token_is_noop);
    RUN(test_unsubscribe_only_matching_id);
    RUN(test_subscribe_returns_distinct_ids);
    RUN(test_subscribe_table_full_returns_zero);
    RUN(test_unsubscribe_compacts_table);
    RUN(test_reset_for_test_clears_state);
    return test_summary();
}
