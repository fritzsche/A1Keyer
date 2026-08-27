#pragma once
/**
 * winkey.h — device-side WinKeyer facade.
 *
 * Owns the single WinkeyBridge instance, wires its callbacks to the
 * firmware (MorseModel / AudioEngine / MorseGenerator), and pumps bytes
 * between WinkeySerial (USB CDC1) and the bridge. main.cpp calls begin()
 * from setup() and poll() from loop().
 *
 * The protocol logic lives in the portable WinkeyBridge; this facade is
 * the thin ESP32/audio glue. On host unit-test builds it is a no-op stub
 * (the bridge itself is tested directly in test/test_winkey_bridge/).
 */
class WinkeyBridge;

class Winkey {
public:
    /// Bring up CDC1 and wire the bridge callbacks. Call from setup()
    /// after AudioEngine::begin() + AudioEngine::createMorseGen().
    static void begin();

    /// Drain CDC1 RX into the bridge and run bridge housekeeping. Call
    /// from loop().
    static void poll();

    /// Read-only access to the bridge singleton for status readback
    /// (used by the HTTP /state handler). Returns nullptr in UNIT_TEST
    /// builds where the bridge is not instantiated.
    static const WinkeyBridge* bridge();

    /// Push MorseModel's current WPM to the bridge (and thus to the
    /// host) when the model has changed since the last call. Drives
    /// the device→host side of the bidirectional WPM sync
    /// (K3NG single-byte speed-pot pin event `(wpm - low) | 0x80`).
    /// Called from loop() after the bridge drain so the host stays
    /// in sync with keyboard / NVS WPM changes. No-op in UNIT_TEST
    /// builds.
    static void syncWpmFromLocal();

    /// Memory-keyer entry point. Plays a stored CW phrase through
    /// sidetone AND the radio-keying bus (KeyEventBus, gated by the
    /// operator's KEYING setting inside RadioKeyer). When the
    /// WinKey bridge is open (a host logger is connected), the bytes
    /// are also fed through `feed()`+`poll()` so the host sees the
    /// standard per-byte K1EL echo — same code path as host-driven
    /// playback. No-op (early return) for null/empty text, when the
    /// generator is already busy, or in UNIT_TEST builds.
    ///
    /// See docs/memory.md and docs/winkey.md.
    static void playLocalMemoryText(const char* text);

    /// TX-buffer entry point. Replaces the memory-keyer path with
    /// the shared MorseModel::_txBuffer for callers that want to
    /// drive the TX buffer directly (currently the HTTP handler and
    /// the WinKeyBridge admin callbacks — neither calls this; both
    /// go via MorseModel directly). Provided as a public hook so
    /// future entry points (e.g. an on-device keyboard overlay) can
    /// share the same path. No-op in UNIT_TEST builds.
    static void beginTxSession();

private:
    Winkey() = delete;
};
