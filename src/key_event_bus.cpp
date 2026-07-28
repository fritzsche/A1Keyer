/**
 * key_event_bus.cpp — Reference-counted key-down / key-up event dispatcher.
 *
 * See key_event_bus.h for the public API and concurrency contract.
 *
 * Implementation notes:
 *   - _demand is the reference count. Producers call keyDown() (++) and
 *     keyUp() (--, saturating at 0). The increment / decrement uses
 *     acq_rel so the resulting callback dispatch is ordered with
 *     respect to the producer's other state.
 *   - On 0→1 we fire every registered sink's onDown. On 1→0 we fire
 *     every onUp. While the count is stable (n→n with n>0 and n>1)
 *     no callbacks fire; sinks only see edges.
 *   - forceAllUp() resets the count to 0 and fires every onUp so any
 *     in-flight producer's stale demand is "drained" from the sinks'
 *     point of view.
 *   - _sinks[] is a small fixed table (kMaxSinks = 8). unsubscribe()
 *     compacts the table by moving the last entry into the freed slot.
 *     Subscribe / unsubscribe are not expected to run on the real-time
 *     audio path (RadioKeyer::begin() runs once at setup).
 */

#include "key_event_bus.h"

namespace {

struct SinkEntry {
    bool used = false;
    KeyEventBus::SinkId id = 0;
    KeyEventBus::DownFn onDown = nullptr;
    KeyEventBus::UpFn   onUp   = nullptr;
};

std::atomic<int> _demand{0};
SinkEntry _sinks[KeyEventBus::kMaxSinks];

// SinkId allocator — starts at 1 so 0 is a distinguishable "invalid" token.
std::atomic<uint32_t> _nextId{1};

inline void fireAllDown() {
    for (size_t i = 0; i < KeyEventBus::kMaxSinks; ++i) {
        if (_sinks[i].used && _sinks[i].onDown) {
            _sinks[i].onDown();
        }
    }
}

inline void fireAllUp() {
    for (size_t i = 0; i < KeyEventBus::kMaxSinks; ++i) {
        if (_sinks[i].used && _sinks[i].onUp) {
            _sinks[i].onUp();
        }
    }
}

}  // namespace

void KeyEventBus::keyDown() {
    // Pre-increment fetch so we know if this is the 0→1 edge.
    int prev = _demand.fetch_add(1, std::memory_order_acq_rel);
    if (prev == 0) {
        fireAllDown();
    }
}

void KeyEventBus::keyUp() {
    // CAS loop to decrement without underflow. We only want to fire the
    // up-edge when the count actually transitions 1→0.
    int prev = _demand.load(std::memory_order_acquire);
    while (prev > 0) {
        if (_demand.compare_exchange_weak(
                prev, prev - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (prev == 1) {
                fireAllUp();
            }
            return;
        }
        // prev was reloaded by compare_exchange_weak on failure.
    }
    // demand already 0 — extra keyUp is a no-op.
}

void KeyEventBus::forceAllUp() {
    int prev = _demand.exchange(0, std::memory_order_acq_rel);
    if (prev > 0) {
        fireAllUp();
    }
}

KeyEventBus::SinkId KeyEventBus::subscribe(DownFn onDown, UpFn onUp) {
    for (size_t i = 0; i < kMaxSinks; ++i) {
        if (!_sinks[i].used) {
            SinkId id = _nextId.fetch_add(1, std::memory_order_relaxed);
            _sinks[i].used   = true;
            _sinks[i].id     = id;
            _sinks[i].onDown = onDown;
            _sinks[i].onUp   = onUp;
            return id;
        }
    }
    return 0;  // table full
}

void KeyEventBus::unsubscribe(SinkId id) {
    if (id == 0) return;
    int removeIdx = -1;
    for (size_t i = 0; i < kMaxSinks; ++i) {
        if (_sinks[i].used && _sinks[i].id == id) {
            removeIdx = (int)i;
            break;
        }
    }
    if (removeIdx < 0) return;

    // Compact: move last used entry into the freed slot.
    int lastUsed = removeIdx;
    for (size_t j = (int)kMaxSinks - 1; j > removeIdx; --j) {
        if (_sinks[j].used) { lastUsed = j; break; }
    }
    if (lastUsed != removeIdx) {
        _sinks[removeIdx] = _sinks[lastUsed];
    }
    _sinks[lastUsed].used   = false;
    _sinks[lastUsed].id     = 0;
    _sinks[lastUsed].onDown = nullptr;
    _sinks[lastUsed].onUp   = nullptr;
}

int KeyEventBus::demand() {
    return _demand.load(std::memory_order_acquire);
}

void KeyEventBus::resetForTest() {
    _demand.store(0, std::memory_order_release);
    for (size_t i = 0; i < kMaxSinks; ++i) {
        _sinks[i].used   = false;
        _sinks[i].id     = 0;
        _sinks[i].onDown = nullptr;
        _sinks[i].onUp   = nullptr;
    }
    _nextId.store(1, std::memory_order_relaxed);
}