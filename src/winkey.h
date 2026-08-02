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
class Winkey {
public:
    /// Bring up CDC1 and wire the bridge callbacks. Call from setup()
    /// after AudioEngine::begin() + AudioEngine::createMorseGen().
    static void begin();

    /// Drain CDC1 RX into the bridge and run bridge housekeeping. Call
    /// from loop().
    static void poll();

private:
    Winkey() = delete;
};
