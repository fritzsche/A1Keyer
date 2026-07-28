/**
 * radio_keyer.cpp — CW keying output driver (see radio_keyer.h).
 *
 * Subscribes to KeyEventBus. On 0→1 fires gpio_set_level(HIGH); on
 * 1→0 fires gpio_set_level(LOW). When the user setting is off the
 * physical line stays LOW regardless of bus transitions.
 *
 * Hardware-specific (ESP-IDF gpio_* + driver/gpio.h) paths are gated
 * by `#ifndef UNIT_TEST`. Under UNIT_TEST the module is a pure state
 * machine so the host tests can verify the sink's behaviour without
 * touching GPIO. On Tab5 / non-Cardputer boards the methods are
 * safe no-ops.
 */

#include "radio_keyer.h"
#include "Log.h"

#include <atomic>

#if !defined(UNIT_TEST) && defined(BOARD_CARDPUTER)
#include <driver/gpio.h>

static constexpr gpio_num_t PIN_RADIO_KEY = GPIO_NUM_4;
#endif

namespace {
std::atomic<bool> _enabled{false};
std::atomic<bool> _keyed{false};
std::atomic<int>  _downCount{0};
std::atomic<int>  _upCount{0};
KeyEventBus::SinkId _sinkId{0};

inline void applyLevel(bool high) {
#if !defined(UNIT_TEST) && defined(BOARD_CARDPUTER)
    gpio_set_level(PIN_RADIO_KEY, high ? 1 : 0);
#endif
    _keyed.store(high, std::memory_order_release);
}

// Sink callbacks wired into KeyEventBus. Both no-op when disabled so
// the GPIO cannot be keyed accidentally.
void onBusDown() {
    if (!_enabled.load(std::memory_order_acquire)) return;
    _downCount.fetch_add(1, std::memory_order_relaxed);
    if (!_keyed.load(std::memory_order_acquire)) {
        applyLevel(true);
    }
}

void onBusUp() {
    if (!_keyed.load(std::memory_order_acquire)) return;
    _upCount.fetch_add(1, std::memory_order_relaxed);
    applyLevel(false);
}
}  // namespace

void RadioKeyer::begin() {
#if !defined(UNIT_TEST) && defined(BOARD_CARDPUTER)
    gpio_reset_pin(PIN_RADIO_KEY);
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_RADIO_KEY),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    applyLevel(false);  // drive LOW before any sink activity
#else
    applyLevel(false);
#endif

    // Subscribe unconditionally so setEnabled(true) later has a live
    // sink to deliver transitions to.
    if (_sinkId == 0) {
        _sinkId = KeyEventBus::subscribe(onBusDown, onBusUp);
        if (_sinkId == 0) {
            Log::error("RadioKeyer: KeyEventBus sink table full");
        }
    }
    Log::info("RadioKeyer: begin complete (GPIO4 -> LOW, sink=%u)",
              (unsigned)_sinkId);
}

void RadioKeyer::setEnabled(bool enabled) {
    bool prev = _enabled.exchange(enabled, std::memory_order_acq_rel);
    if (prev == enabled) return;

    if (!enabled) {
        // User turned it off — force the GPIO LOW immediately even if
        // a producer is mid-element. drain the bus so any subsequent
        // transitions don't snap the line back HIGH later.
        KeyEventBus::forceAllUp();
        applyLevel(false);
    }
    // When enabling, do NOT restore stale demand from before — the
    // user must press a fresh key / element for the line to go HIGH.
    Log::info("RadioKeyer: enabled=%d", enabled ? 1 : 0);
}

bool RadioKeyer::isEnabled() {
    return _enabled.load(std::memory_order_acquire);
}

bool RadioKeyer::isKeyed() {
    return _keyed.load(std::memory_order_acquire);
}

int RadioKeyer::downCount() { return _downCount.load(std::memory_order_relaxed); }
int RadioKeyer::upCount()   { return _upCount.load(std::memory_order_relaxed); }

void RadioKeyer::resetForTest() {
    _enabled.store(false, std::memory_order_release);
    _keyed.store(false, std::memory_order_release);
    _downCount.store(0, std::memory_order_relaxed);
    _upCount.store(0, std::memory_order_relaxed);
    _sinkId = 0;
}