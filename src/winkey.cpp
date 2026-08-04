/**
 * winkey.cpp — device-side WinKeyer facade (glue).
 *
 * Device build: wires WinkeyBridge callbacks to MorseModel / AudioEngine
 * / MorseGenerator, streams the bridge's output bytes back over the shared
 * serial line (WinKey mode only), and pumps received bytes in poll(). Host
 * (UNIT_TEST) build: no-op.
 */
#include "winkey.h"

#ifndef UNIT_TEST

#include "winkey_bridge.h"
#include "winkey_serial.h"
#include "console_io.h"
#include "display_model.h"
#include "audio_engine.h"
#include "morse_generator.h"
#include "Log.h"

namespace {

WinkeyBridge _bridge;

// ─── Bridge output sink: bytes toward the host go out the serial line. ──
void wkOut(uint8_t byte, void* /*ctx*/) {
    WinkeySerial::write(byte);
}

// ─── Effect callbacks (WinkeyBridge::Callbacks) ─────────────────────────
void cbSetWpm(int wpm, void* /*ctx*/) {
    // MorseModel::setWPM clamps to [5,50] and propagates to keyer + gen.
    MorseModel::instance().setWPM(wpm);
}

void cbSetSidetoneHz(int hz, void* /*ctx*/) {
    // MorseModel clamps to [300,900]; keep the model and audio in sync.
    MorseModel::instance().setFrequency((float)hz);
    AudioEngine::setToneFrequency((float)hz);
}

void cbSetOutputEnable(bool on, void* /*ctx*/) {
    // Gated by the operator's KEYING setting inside RadioKeyer — see
    // docs/winkey.md § 16.4 rule 3. We surface the host's intent by
    // mirroring it into the model; RadioKeyer AND-s it with the operator
    // flag, so a host E1 cannot re-enable RF the operator turned off.
    MorseModel::instance().setRadioKeyingEnabled(on);
}

void cbSendText(const char* text, void* /*ctx*/) {
    MorseGenerator* gen = AudioEngine::morseGen();
    if (gen) {
        MorseModel::instance().setMode(KeyerMode::ENCODER);
        gen->playText(text);
    }
    // Echo is now handled in WinkeyBridge::appendText (per-byte,
    // K1EL-compliant — the chip echoes each text char when received, not
    // when keyed). We deliberately do not re-echo here; otherwise hosts
    // would see each character twice.
}

void cbStopSending(void* /*ctx*/) {
    MorseGenerator* gen = AudioEngine::morseGen();
    if (gen) gen->stop();
}

}  // namespace

void Winkey::begin() {
    WinkeySerial::begin();
    WinkeyBridge::Callbacks cb;
    cb.setWpm          = &cbSetWpm;
    cb.setSidetoneHz   = &cbSetSidetoneHz;
    cb.setOutputEnable = &cbSetOutputEnable;
    cb.sendText        = &cbSendText;
    cb.stopSending     = &cbStopSending;
    cb.ctx             = nullptr;
    _bridge.begin(&wkOut, nullptr, cb);
    Log::info("[WK] WinkeyBridge ready (WinKey is the default; press 'D' for Console/debug mode)");
}

void Winkey::poll() {
    // Only parse serial bytes as WinKeyer protocol while in WinKey mode.
    // In Console mode the same serial line carries the debug console /
    // terminal input, which must NOT be fed to the bridge.
    if (Console::mode() != Console::Mode::WinKey) return;

    // Drain everything the serial line has buffered this cycle, then run
    // bridge housekeeping (send-buffer playback).
    int guard = 256;  // bound work per loop() so we never starve the loop
    while (WinkeySerial::available() > 0 && guard-- > 0) {
        int b = WinkeySerial::read();
        if (b < 0) break;
        // Protocol-trace tap. One [INFO] line per byte so the wire
        // activity is greppable from the serial monitor and from the
        // HTTP /log endpoint (Log::info → Console::write → LogRing when
        // ENABLE_WIFI_DEBUG=1). Pairs with the [WK2 TX] tap in
        // Console::rawWinkeyWrite() for a full duplex trace.
        Log::info("[WK2 RX] %02X", b);
        _bridge.feed((uint8_t)b);
    }
    _bridge.poll();
}

const WinkeyBridge* Winkey::bridge() {
    return &_bridge;
}

#else  // UNIT_TEST — no-op stubs

void Winkey::begin() {}
void Winkey::poll() {}
const WinkeyBridge* Winkey::bridge() { return nullptr; }

#endif  // UNIT_TEST
