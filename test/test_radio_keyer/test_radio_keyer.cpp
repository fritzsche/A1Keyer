#include "test_framework.h"
#include "radio_keyer.h"
#include "key_event_bus.h"

// Under UNIT_TEST, radio_keyer.cpp compiles to a state-only stub:
//   - No GPIO is touched.
//   - RadioKeyer::begin() still subscribes to KeyEventBus so the
//     enabled / disabled -> GPIO state matrix can be exercised.
//
// Tests verify:
//   1. Disabled unit never reports keyed, even if the bus fires.
//   2. setEnabled(false) while keyed forces LOW and drains the bus.
//   3. Bus integration: enabled unit tracks the bus 0→1 / 1→0 edges.
//   4. Counters report transitions cleanly.

static void resetState() {
    KeyEventBus::resetForTest();
    RadioKeyer::resetForTest();
}

static void test_default_disabled_after_reset() {
    resetState();
    CHECK(!RadioKeyer::isEnabled());
    CHECK(!RadioKeyer::isKeyed());
}

static void test_disabled_unit_ignores_bus() {
    resetState();
    RadioKeyer::begin();
    // Re-arm enabling flag (reset cleared it). Stay disabled.
    CHECK(!RadioKeyer::isEnabled());

    KeyEventBus::keyDown();
    KeyEventBus::keyDown();
    KeyEventBus::keyUp();
    KeyEventBus::keyUp();

    // No transitions should have been noted.
    CHECK(!RadioKeyer::isKeyed());
    CHECK_EQ(RadioKeyer::downCount(), 0);
    CHECK_EQ(RadioKeyer::upCount(), 0);
}

static void test_set_enabled_false_while_keyed_forces_low() {
    resetState();
    RadioKeyer::begin();
    RadioKeyer::setEnabled(true);

    KeyEventBus::keyDown();
    CHECK(RadioKeyer::isKeyed());
    CHECK_EQ(RadioKeyer::downCount(), 1);

    // User toggles off mid-element.
    RadioKeyer::setEnabled(false);
    CHECK(!RadioKeyer::isEnabled());
    CHECK(!RadioKeyer::isKeyed());
    // upCount reflects the drain transition.
    CHECK_EQ(RadioKeyer::upCount(), 1);

    // The bus has been drained. A subsequent keyDown while still
    // disabled is a no-op — the sink callback early-returns without
    // arming the GPIO.
    KeyEventBus::keyDown();
    CHECK(!RadioKeyer::isKeyed());
    CHECK_EQ(RadioKeyer::downCount(), 1);

    // Release the bus demand so re-enabling + a fresh keyDown actually
    // produces a 0→1 edge.
    KeyEventBus::keyUp();

    // Re-enable, re-key.
    RadioKeyer::setEnabled(true);
    KeyEventBus::keyDown();
    CHECK(RadioKeyer::isKeyed());
    CHECK_EQ(RadioKeyer::downCount(), 2);
}

static void test_bus_integration_enabled_tracks_edges() {
    resetState();
    RadioKeyer::begin();
    RadioKeyer::setEnabled(true);

    KeyEventBus::keyDown();
    CHECK(RadioKeyer::isKeyed());
    CHECK_EQ(RadioKeyer::downCount(), 1);
    CHECK_EQ(RadioKeyer::upCount(), 0);

    KeyEventBus::keyUp();
    CHECK(!RadioKeyer::isKeyed());
    CHECK_EQ(RadioKeyer::downCount(), 1);
    CHECK_EQ(RadioKeyer::upCount(), 1);
}

static void test_bus_integration_overlapping_demand() {
    resetState();
    RadioKeyer::begin();
    RadioKeyer::setEnabled(true);

    // Two producers overlap (e.g. held K + pressed paddle).
    KeyEventBus::keyDown();   // 0->1, fires onBusDown
    KeyEventBus::keyDown();   // 1->2, no event
    CHECK(RadioKeyer::isKeyed());
    CHECK_EQ(RadioKeyer::downCount(), 1);

    KeyEventBus::keyUp();     // 2->1, no event
    CHECK(RadioKeyer::isKeyed());
    CHECK_EQ(RadioKeyer::upCount(), 0);

    KeyEventBus::keyUp();     // 1->0, fires onBusUp
    CHECK(!RadioKeyer::isKeyed());
    CHECK_EQ(RadioKeyer::upCount(), 1);
}

static void test_repeated_on_off_cycles() {
    resetState();
    RadioKeyer::begin();

    for (int i = 0; i < 5; ++i) {
        RadioKeyer::setEnabled(true);
        KeyEventBus::keyDown();
        CHECK(RadioKeyer::isKeyed());
        KeyEventBus::keyUp();
        CHECK(!RadioKeyer::isKeyed());

        RadioKeyer::setEnabled(false);
        // Any in-flight demand is forced LOW.
        KeyEventBus::keyDown();
        CHECK(!RadioKeyer::isKeyed());
        KeyEventBus::keyUp();
        CHECK(!RadioKeyer::isKeyed());
    }

    CHECK_EQ(RadioKeyer::downCount(), 5);  // only the enabled cycles
    CHECK_EQ(RadioKeyer::upCount(), 5);
}

static void test_setting_same_value_is_noop() {
    resetState();
    RadioKeyer::begin();
    RadioKeyer::setEnabled(true);
    int downBefore = RadioKeyer::downCount();
    int upBefore   = RadioKeyer::upCount();

    RadioKeyer::setEnabled(true);  // no-op
    CHECK_EQ(RadioKeyer::downCount(), downBefore);
    CHECK_EQ(RadioKeyer::upCount(), upBefore);
}

int main() {
    printf("=== test_radio_keyer ===\n");
    RUN(test_default_disabled_after_reset);
    RUN(test_disabled_unit_ignores_bus);
    RUN(test_set_enabled_false_while_keyed_forces_low);
    RUN(test_bus_integration_enabled_tracks_edges);
    RUN(test_bus_integration_overlapping_demand);
    RUN(test_repeated_on_off_cycles);
    RUN(test_setting_same_value_is_noop);
    return test_summary();
}
