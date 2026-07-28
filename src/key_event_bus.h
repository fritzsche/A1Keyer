#pragma once
/**
 * key_event_bus.h — Central dispatcher for CW key-down / key-up events.
 *
 * Every CW source (iambic paddle, straight key, MorseGenerator player,
 * future Winkey bridge) calls keyDown() / keyUp() on this bus. Every CW
 * sink (RadioKeyer for the on-air output, future MIDI keyer, future
 * decoder-logger, ...) subscribes via subscribe() and receives
 * transitions of the aggregate demand.
 *
 * Concurrency: audio task (Core 1) and loop() (Core 0) can call keyDown /
 * keyUp concurrently. The internal reference count is std::atomic<int>;
 * callbacks are invoked while the count is in a stable state, so sinks
 * do not need their own locking.
 *
 * Reference counting: multiple producers (e.g. paddle iambic + held K
 * keyboard key) can overlap. The aggregate is "down" from the first
 * keyDown until the last matching keyUp. Sinks only see 0→1 and 1→0
 * transitions.
 *
 * This module has no hardware dependencies — it is portable between
 * firmware and host unit tests.
 */
#include <atomic>
#include <cstdint>

class KeyEventBus {
public:
    /// Opaque subscription handle returned by subscribe().
    using SinkId = uint32_t;

    /// Callback signatures.
    using DownFn = void (*)();
    using UpFn   = void (*)();

    /// Maximum number of concurrent subscribers. The current design
    /// expects only a handful (RadioKeyer + future sinks).
    static constexpr size_t kMaxSinks = 8;

    /// Producer entry: a CW source has started emitting a tone.
    /// Increments the reference count; on 0→1 fires every sink's onDown.
    static void keyDown();

    /// Producer entry: a CW source has stopped emitting a tone.
    /// Decrements the reference count (saturating at 0); on 1→0 fires
    /// every sink's onUp. Idempotent: an extra keyUp() with demand==0
    /// is a no-op.
    static void keyUp();

    /// Force every sink to its "up" state immediately, regardless of
    /// outstanding demand. Used by RadioKeyer when the user toggles
    /// keying off mid-element so the transmitter is unkeyed at once.
    static void forceAllUp();

    /// Subscribe a sink. The sink's onDown fires on 0→1; onUp fires on
    /// 1→0 or on forceAllUp(). Returns an opaque token for unsubscribe.
    /// Returns 0 (invalid token) if the sink table is full.
    static SinkId subscribe(DownFn onDown, UpFn onUp);

    /// Remove a previously registered sink. No-op for an invalid token.
    static void unsubscribe(SinkId id);

    /// Number of outstanding keyDown() calls not yet matched by keyUp().
    /// Exposed for tests / diagnostics. Always >= 0.
    static int demand();

    /// Reset the bus to a clean state. Used by host unit tests between
    /// RUN() cases so sinks from a previous test do not leak.
    static void resetForTest();
};