#pragma once
/**
 * radio_keyer.h — Galvanically-isolated CW keying output.
 *
 * Drives a single GPIO high while a CW tone is being generated
 * (iambic dit, iambic dah, straight-key closure, or the keyboard `K`
 * held-to-key). The intent is to drive a PC817 optocoupler whose
 * transistor side keys a real transceiver's CW input.
 *
 * Source-of-truth is the central KeyEventBus; RadioKeyer is just a
 * sink. Subscribing / unsubscribing happens in begin() / end(). The
 * user-facing on/off setting (persisted in NVS) is handled by
 * setEnabled(); disabling forces the GPIO LOW immediately even if a
 * keyer element is mid-stream.
 *
 * Hardware:
 *   - M5Stack Cardputer ADV only (ESP32-S3, GPIO_NUM_4 on the EXT
 *     2.54-14P header).
 *   - GPIO4 is shared with AudioEngine's NS4168 amp chain which
 *     configures it as OUTPUT but never drives a level. begin() resets
 *     and re-claims the pin, then drives LOW before any sink activity
 *     is possible.
 *   - On Tab5 (BOARD_TAB5) and in host unit tests the module compiles
 *     to a state-only stub.
 */
#include "key_event_bus.h"

class RadioKeyer {
public:
    /// Reset GPIO4, configure as OUTPUT, drive LOW, subscribe to bus.
    /// Must be called from setup() AFTER AudioEngine::begin().
    static void begin();

    /// Enable / disable the on-air output. setEnabled(false) forces the
    /// GPIO LOW immediately and routes all subsequent bus transitions
    /// to a no-op until re-enabled.
    static void setEnabled(bool enabled);

    /// Whether the user setting is on. Independent of the momentary
    /// keyed state.
    static bool isEnabled();

    /// Current keyed (GPIO HIGH) state. Reflects the physical line, not
    /// just the enabled flag.
    static bool isKeyed();

    /// For tests / diagnostics: how many keyDowns the sink has seen
    /// since begin(). Reset by resetForTest().
    static int downCount();
    static int upCount();

    /// Reset internal counters and the enabled flag. Used by host unit
    /// tests between RUN() cases.
    static void resetForTest();
};