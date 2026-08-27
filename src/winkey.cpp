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
#include "morse_decoder.h"
#include "tx_buffer.h"
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
    if (!gen) return;
    MorseModel::instance().setMode(KeyerMode::ENCODER);
    // Defensive: only start a fresh playback if the generator is
    // idle. The bridge's poll() should already have checked
    // canAcceptText() and skipped us if we are mid-word, but if
    // anything still reaches us while busy, ignore it — the text
    // stays in the bridge buffer and the next idle poll will drain
    // the accumulated chunk. Restarting playText() per character
    // produces an audible click and choppy audio. See
    // docs/winkey.md "Text playback and the audio click bug".
    if (!gen->isPlaying()) {
        // isContinuation is true for chunks after the first in a
        // chunked host-driven session (the bridge tracks that via
        // _isContinuation in its poll()/resetParams() — see
        // winkey_bridge.h). For the FIRST chunk in a session it
        // is false, so the generator does NOT prepend a leading
        // CHAR_SPACE (which would otherwise shift the decoded
        // text by one position on every fresh playback). This is
        // the same value the Winkey::playLocalMemoryText direct
        // path uses (default false), so the two paths agree on
        // what counts as a "fresh" playback.
        gen->playText(text, _bridge.isContinuation());
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

// Forward each decoded paddle character (or word-space) to the WK
// bridge so RUMlogNG / N1MM log the operator's manual keying. Hooked in
// Winkey::begin(). Mirrors K3NG's `winkey_paddle_echo_buffer` decode
// path at k3ng_keyer.ino:11623.
void cbDecodedChar(char c, void* /*ctx*/) {
    _bridge.emitDecodedChar(c);
}

// Returns true if the audio player can accept a fresh sendText()
// chunk. poll() consults this so back-to-back text bytes accumulate
// in the bridge buffer and play as one phrase instead of restarting
// the player on every char.
bool cbCanAcceptText(void* /*ctx*/) {
    MorseGenerator* gen = AudioEngine::morseGen();
    return gen && !gen->isPlaying();
}

// ─── TX-buffer side-band callbacks (A1Keyer vendor extension) ──────────────
//
// Wired to the bridge when the host enters LOAD mode (ADMIN_TX_BUFFER_LOAD)
// and dispatches bytes/control codes to MorseModel::_txBuffer — the same
// buffer the web UI's /api/tx/* endpoints drive. See docs/tx_buffer.md § 5.

void cbTxBufferFeed(char c, void* /*ctx*/) {
    MorseModel::instance().appendTxChar(c);
}

void cbTxBufferBackspace(void* /*ctx*/) {
    MorseModel::instance().backspaceTx();
}

void cbTxBufferClear(void* /*ctx*/) {
    // Cancels any in-flight session and wipes the buffer.
    MorseModel::instance().clearTx();
}

void cbTxBufferLoad(void* /*ctx*/) {
    // The bridge just flipped _txBufferLoadMode=true. Discard any
    // stale live bytes that arrived before the mode switch — the
    // host has explicitly opted into LOAD mode and any pending live
    // bytes represent stale intent. The MorseModel buffer is
    // preserved (the host may want to append to it).
    Log::info("[WK] TX-buffer LOAD mode entered");
}

void cbTxBufferStart(void* /*ctx*/) {
    Log::info("[WK] TX-buffer START (host-driven)");
    TxBuffer::beginSession();
}

bool cbTxBufferHasPending(void* /*ctx*/) {
    return MorseModel::instance().txHasPending();
}

}  // namespace

void Winkey::begin() {
    WinkeySerial::begin();
    WinkeyBridge::Callbacks cb;
    cb.setWpm              = &cbSetWpm;
    cb.setSidetoneHz       = &cbSetSidetoneHz;
    cb.setOutputEnable     = &cbSetOutputEnable;
    cb.sendText            = &cbSendText;
    cb.canAcceptText       = &cbCanAcceptText;
    cb.stopSending         = &cbStopSending;
    cb.txBufferFeed        = &cbTxBufferFeed;
    cb.txBufferBackspace   = &cbTxBufferBackspace;
    cb.txBufferClear       = &cbTxBufferClear;
    cb.txBufferLoad        = &cbTxBufferLoad;
    cb.txBufferStart       = &cbTxBufferStart;
    cb.txBufferHasPending  = &cbTxBufferHasPending;
    cb.ctx                 = nullptr;
    _bridge.begin(&wkOut, nullptr, cb);
    // Forward MorseDecoder's decoded characters back to the host so
    // RUMlogNG / N1MM log the operator's paddle keying. Mirrors K3NG's
    // `winkey_paddle_echo_buffer` decode path at k3ng_keyer.ino:11623.
    MorseDecoder::setDecodedCharHook(&cbDecodedChar, nullptr);
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
    // Sync MorseModel → bridge → host (K3NG speed-pot pin event).
    // Cheap: a single atomic counter load + compare, then maybe a
    // 1-byte emit `(wpm - low) | 0x80`. Drives the device→host half
    // of the bidirectional WPM sync — see docs/winkey.md § 16.7.
    // Runs every loop() so keyboard / NVS WPM changes propagate
    // within one tick.
    syncWpmFromLocal();
}

void Winkey::syncWpmFromLocal() {
    // Polled state machine — only acts when MorseModel's changeCounter
    // has advanced since the last call. Cheap to call every loop().
    // The bridge's setWpmFromLocal() short-circuits when the polled
    // WPM already matches the last value the host sent us, so we do
    // NOT echo the host's own 0x02 N back at it.
    static uint32_t lastCounter = UINT32_MAX;
    auto& model = MorseModel::instance();
    uint32_t cur = model.changeCounter();
    if (cur == lastCounter) return;
    lastCounter = cur;
    _bridge.setWpmFromLocal(model.wpm());
}

// Memory-keyer entry point. See winkey.h for the contract.
void Winkey::playLocalMemoryText(const char* text) {
    // Empty / unset slot is a no-op — pressing digit N on an empty
    // memory must not produce a click, just silence.
    if (!text || text[0] == '\0') return;

    MorseGenerator* gen = AudioEngine::morseGen();
    if (!gen) return;
    // Mirrors the P-key busy gate at main.cpp:500-505: if a previous
    // playback is still running, drop the new request rather than
    // restart mid-element (which would produce an audible click and
    // a confusing on-air glitch). The user can hit the digit again
    // after the current playback completes.
    if (gen->isPlaying()) return;

    MorseModel::instance().setMode(KeyerMode::ENCODER);

    if (_bridge.isOpen()) {
        // Host is attached — route through the bridge so the connected
        // logger sees the per-byte K1EL echo. Each feed() uppercases
        // and echoes immediately; poll() drains the FIFO through
        // cbSendText → MorseGenerator::playText as one chunk. This is
        // literally the same code path host-driven playback uses, so
        // memory playback is indistinguishable to the logger from
        // playback it initiated itself.
        for (const char* p = text; *p; ++p) {
            _bridge.feed(static_cast<uint8_t>(*p));
        }
        _bridge.poll();
    } else {
        // No host attached — play directly. Radio keying happens via
        // the new MorseGenerator → KeyEventBus wiring (Stage 2); the
        // bridge is bypassed so we don't spam the serial console with
        // protocol bytes while the operator is local-only.
        gen->playText(text);
    }
}

const WinkeyBridge* Winkey::bridge() {
    return &_bridge;
}

// TX-buffer session entry. Both the HTTP /api/tx/start handler and the
// WinKeyBridge cbTxBufferStart callback eventually call MorseModel::
// startTx() directly — this method is a public hook reserved for future
// callers (e.g. an on-device keyboard overlay). Keeping the API on
// Winkey mirrors the playLocalMemoryText shape.
void Winkey::beginTxSession() {
    MorseModel::instance().startTx();
}

#else  // UNIT_TEST — no-op stubs

void Winkey::begin() {}
void Winkey::poll() {}
void Winkey::syncWpmFromLocal() {}
void Winkey::playLocalMemoryText(const char*) {}
void Winkey::beginTxSession() {}
const WinkeyBridge* Winkey::bridge() { return nullptr; }

#endif  // UNIT_TEST
