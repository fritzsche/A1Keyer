/**
 * winkey_bridge.cpp — WinKeyer 2.x protocol state machine.
 *
 * Command codes and parser model verified against the K3NG reference
 * implementation (k3ng_keyer.ino, service_winkey()). Pure C++, no
 * Arduino / ESP-IDF dependency — compiles identically on device and in
 * host unit tests. See winkey_bridge.h and docs/winkey.md § 16.
 */
#include "winkey_bridge.h"
#include <cstring>

namespace {

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// WK command bytes (0x00-0x1F). Values from the K3NG service_winkey()
// switch. Only the ones we act on are named; the rest are consumed with
// the right parameter arity and ignored (stored where useful).
enum : uint8_t {
    WK_ADMIN         = 0x00,  // + admin sub-command byte
    WK_SIDETONE      = 0x01,  // 1 param: bit7 paddle-only, low nibble 1-10 freq
    WK_SPEED         = 0x02,  // 1 param: WPM (0 = use pot)
    WK_WEIGHTING     = 0x03,  // 1 param
    WK_PTT_TIMES     = 0x04,  // 2 params: lead, tail
    WK_SET_POT       = 0x05,  // 3 params
    WK_PAUSE         = 0x06,  // 1 param
    WK_GET_POT       = 0x07,  // 0 params, replies
    WK_BACKSPACE     = 0x08,  // 0 params
    WK_PINCONFIG     = 0x09,  // 1 param
    WK_CLEAR_BUF     = 0x0A,  // 0 params
    WK_KEY_IMMED     = 0x0B,  // 1 param
    WK_HSCW          = 0x0C,  // 1 param
    WK_FARNSWORTH    = 0x0D,  // 1 param
    WK_SETMODE       = 0x0E,  // 1 param
    WK_LOAD_DEFAULTS = 0x0F,  // 15 params
    WK_FIRST_EXT     = 0x10,  // 1 param
    WK_KEY_COMP      = 0x11,  // 1 param
    WK_NULL_12       = 0x12,  // 1 param (unsupported placeholder)
    WK_NULL_13       = 0x13,  // 0 params
    WK_SW_PADDLE     = 0x14,  // 1 param
    WK_REQ_STATUS    = 0x15,  // 0 params, replies status
    WK_POINTER       = 0x16,  // 1 param
    WK_RATIO         = 0x17,  // 1 param
    // 0x18-0x1F buffered commands — most take 1 param; 0x1E/0x1F take 0.
    WK_BUF_PTT       = 0x18,  // 1 param
    WK_BUF_KEY       = 0x19,  // 1 param
    WK_BUF_WAIT      = 0x1A,  // 1 param
    WK_BUF_MERGE     = 0x1B,  // 0 params (merges next two chars)
    WK_BUF_SPEED     = 0x1C,  // 1 param
    WK_BUF_HSCW      = 0x1D,  // 1 param
    WK_BUF_CANCELSPD = 0x1E,  // 0 params
    WK_BUF_NOP       = 0x1F,  // 0 params
};

// Admin sub-commands (after 0x00).
enum : uint8_t {
    ADMIN_RESET      = 0x01,
    ADMIN_HOST_OPEN  = 0x02,
    ADMIN_HOST_CLOSE = 0x03,
    ADMIN_GET_VALUES = 0x07,   // K1EL WK2 datasheet v23 § 4 — reply with all
                                // current settings; the byte value (0x07) is
                                // in the same numeric range as operating
                                // WK_GET_POT, but the 0x00 prefix routes it
                                // here instead — no collision. See
                                // docs/winkey.md § 16.7.
    ADMIN_SET_WK1    = 0x0A,
    ADMIN_SET_WK2    = 0x0B,
};

constexpr uint8_t kTextThreshold = 0x20;  // >= this byte value is text

}  // namespace

uint8_t WinkeyBridge::paramCountFor(uint8_t cmd) {
    switch (cmd) {
        case WK_PTT_TIMES:     return 2;
        case WK_SET_POT:       return 3;
        case WK_LOAD_DEFAULTS: return 15;
        // no-parameter commands act immediately:
        case WK_GET_POT: case WK_BACKSPACE: case WK_CLEAR_BUF:
        case WK_NULL_13: case WK_REQ_STATUS:
        case WK_BUF_MERGE: case WK_BUF_CANCELSPD: case WK_BUF_NOP:
            return 0;
        // everything else in 0x01-0x1F we handle takes exactly 1 param:
        default:               return 1;
    }
}

// WK2 sidetone command (0x01): low nibble 1-10 selects a preset. The WK2
// presets are 4000/N Hz for N=1..10 → ~4000,2000,1333,1000,800,666,571,
// 500,444,400. We map to the nearest sensible sidetone; A1Keyer clamps
// to its own [300,900] range in the hook.
int WinkeyBridge::sidetoneIndexToHz(uint8_t byte) {
    static const int kHz[11] = {
        600,          // index 0 → default
        4000, 2000, 1333, 1000, 800, 667, 571, 500, 444, 400
    };
    int idx = byte & 0x0F;
    if (idx < 1 || idx > 10) return 600;
    return kHz[idx];
}

// K1EL WK2 / K3NG speed-pot byte convention: the high bit (0x80) is
// the "speed pot changed" indicator; the low 7 bits are the offset
// from the configured pot-low WPM. K3NG's two emitters both use this
// encoding (k3ng_cw_keyer/k3ng_keyer/k3ng_keyer.ino:5730 for the live
// pin-event and :11788 for the GET_POT reply). Hosts (RUMlogNG, N1MM,
// hamlib) decode via `wpm = low + (byte & 0x7F)`. See docs/winkey.md
// § 14.1, § 16.7.
uint8_t WinkeyBridge::speedPotValue() const {
    int offset = _wpm - _potWpmLow;
    if (offset < 0)   offset = 0;
    if (offset > 127) offset = 127;   // 0x80 | 127 = 0xFF (max byte)
    return (uint8_t)(0x80 | offset);
}

// Push a local WPM change (keyboard, NVS restore, etc.) into the
// bridge's mirror and out to the host as a single speed-pot byte
// (`(wpm - low) | 0x80`). K3NG-compatible loggers (RUMlogNG, N1MM,
// fldigi) parse this and update their UI to match the operator's
// choice. Idempotent: when the polled value matches `_lastPushedWpm`
// (which the WK_SPEED path stamps on every host-initiated change),
// we skip the emit to avoid echoing the host's own command back. See
// docs/winkey.md § 16.7.
void WinkeyBridge::setWpmFromLocal(int wpm) {
    int clamped = clampi(wpm, 5, 99);
    if (clamped == _wpm && clamped == _lastPushedWpm) return;
    _wpm = clamped;
    _lastPushedWpm = clamped;
    if (_open) emit(speedPotValue());
}

void WinkeyBridge::begin(OutputFn out, void* outCtx, const Callbacks& cb) {
    _out = out;
    _outCtx = outCtx;
    _cb = cb;
    resetForTest();
}

void WinkeyBridge::resetForTest() {
    _open = false;
    _wk2Mode = false;
    resetParams();
}

void WinkeyBridge::resetParams() {
    _parse = Parse::IDLE;
    _pendingCmd = 0;
    _paramGot = 0;
    _paramsNeeded = 0;
    _wpm = 20;
    _sidetoneHz = 600;
    _outputEnable = true;
    _keyerMode = 1;
    _weight = 50; _farnsworth = 0; _pttTail = 5; _pttLead = 5;
    _hangTime = 0; _ratio = 50;
    // Default pot range matches A1Keyer's effective WPM range; the
    // host reconfigures this via `0x05` Set Pot. See docs/winkey.md
    // § 14.1.
    _potWpmLow  = kWpmMin;
    _potWpmHigh = kWpmMax;
    // Clear the "last pushed" tracker so a subsequent local change
    // (or the host's first probe after reset) re-emits the current
    // value via setWpmFromLocal(). See docs/winkey.md § 16.7.
    _lastPushedWpm = -1;
    _buffer.clear();
    // Reset the "primed" gate so any text bytes that arrive between
    // a host-open (or soft reset) and the host's first probe query
    // are swallowed. WK2-compliant hosts always probe via GET_POT or
    // REQ_STATUS, so legitimate text arrives AFTER _primed flips true.
    // Defends against RUMlogNG sending stray bytes from its outgoing
    // CW buffer before the host has even confirmed we are alive —
    // see docs/winkey.md § 13.9.
    _primed = false;
    // After a reset (test or wire-facing admin reset) the next
    // sendText() must look like a fresh, independent playback: the
    // previous session's audio is gone and the encoder doesn't emit
    // a trailing CHAR_SPACE, so MorseGenerator would prepend an
    // unwanted gap if we left _isContinuation=true here.
    _isContinuation = false;
    _prevConsumerBusy    = false;
    _sawAudioEndEdge     = false;
    _prevBufferNonEmpty  = false;
}

void WinkeyBridge::emit(uint8_t byte) {
    if (_out) _out(byte, _outCtx);
}

void WinkeyBridge::emitDecodedChar(char c) {
    // No host connected → no echo. Mirrors K3NG's
    //   if (winkey_host_open) { ... winkey_port_write(...); }
    // at k3ng_keyer.ino:11623.
    if (!_open) return;

    // Not yet primed → the bridge is still in the RUMlogNG-init
    // window where any byte we send risks being interpreted as an
    // out-of-order reply to the host's first probe (see the
    // RUMlogNG-init-does-not-play-text test). Sit on the byte
    // silently until the host has asked us a question that we
    // answered (GET_POT / REQ_STATUS).
    if (!_primed) return;

    if (c == ' ') {
        // Word-space: emit a single space byte. Mirrors K3NG's
        // winkey_port_write(' ', 0) at k3ng_keyer.ino:11637.
        emit((uint8_t)' ');
        return;
    }

    // Lowercase → uppercase to match the WK text-byte convention
    // (the host receives ASCII characters; uppercase keeps the
    // log readable regardless of the operator's paddle habit).
    if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
    emit((uint8_t)c);
}

void WinkeyBridge::feed(uint8_t byte) {
    switch (_parse) {
        case Parse::ADMIN:
            _parse = Parse::IDLE;
            handleAdmin(byte);
            return;

        case Parse::NEED_PARAMS:
            // Store up to 3 params; swallow (ignore) any beyond that so
            // large commands like Load-Defaults (15) consume cleanly
            // without overflowing _params.
            if (_paramGot < sizeof(_params)) _params[_paramGot] = byte;
            ++_paramGot;
            if (_paramGot >= _paramsNeeded) {
                _parse = Parse::IDLE;
                uint8_t stored = _paramGot < (uint8_t)sizeof(_params)
                                 ? _paramGot : (uint8_t)sizeof(_params);
                applyCommand(_pendingCmd, _params, stored);
            }
            return;

        case Parse::IDLE:
        default:
            if (byte == WK_ADMIN) {
                _parse = Parse::ADMIN;
                return;
            }
            if (byte >= kTextThreshold) {
                // Text to send as CW. Accepted only after host-open AND
                // once the host has probed us with GET_POT or REQ_STATUS
                // (i.e. _primed). Without the primed gate, RUMlogNG's
                // outgoing-CW buffer is streamed at open time and the
                // first character is keyed as CW before the user has
                // typed anything — see docs/winkey.md § 13.9.
                if (_open && _primed) appendText(byte);
                return;
            }
            // Command byte 0x01-0x1F. Commands other than admin require
            // host-open (admin is handled above via 0x00).
            if (!_open) return;
            dispatchCommand(byte);
            return;
    }
}

void WinkeyBridge::dispatchCommand(uint8_t cmd) {
    uint8_t n = paramCountFor(cmd);
    if (n == 0) {
        applyCommand(cmd, nullptr, 0);
        return;
    }
    _pendingCmd = cmd;
    _paramsNeeded = n;      // true count; may exceed the 3-slot store
    _paramGot = 0;
    _parse = Parse::NEED_PARAMS;
}

void WinkeyBridge::appendText(uint8_t byte) {
    // Uppercase lowercase letters (WK behaviour). '|' (0x7C) is a
    // half-space; pass it through — the generator treats it as text.
    if (byte >= 'a' && byte <= 'z') byte = (uint8_t)(byte - 32);
    _buffer.push((char)byte);
    // Echo the byte back to the host IMMEDIATELY, not later when the
    // buffer is drained for keying. This matches the K1EL WK2 chip:
    // hosts use the echo to track which characters are in the send
    // buffer (a backspaced char still gets echoed when it was first
    // received, then BS is silent — see test_backspace_silent_echo).
    emit(byte);
}

void WinkeyBridge::handleAdmin(uint8_t sub) {
    switch (sub) {
        case ADMIN_HOST_OPEN:
            _open = true;
            emit(kVersion);          // version byte (§ 5.1)
            return;
        case ADMIN_SET_WK2:
            _wk2Mode = true;
            return;
        case ADMIN_SET_WK1:
            _wk2Mode = false;
            return;
        case ADMIN_RESET:
            // Restore parameter defaults but keep the host interface
            // open (K3NG / hamlib friendly behaviour). Real-world
            // hosts (N1MM, RUMlogNG, fldigi, WriteLog) routinely issue
            // a defensive reset as part of their init sequence — see
            // docs/winkey.md § 5.1 — and then immediately send GetPot,
            // ReqStatus, or text. The K1EL WK2 datasheet technically
            // requires the host to re-open after reset, but every
            // shipping WK2 emulator (K3NG, hamlib winkey.c) chooses to
            // stay open because in practice no logger actually
            // re-opens. Without this change, RUMlogNG sees the version
            // byte on open, then "Interface is not available" once
            // every command after its reset is silently dropped.
            resetParams();
            return;
        case ADMIN_HOST_CLOSE:
            _open = false;
            return;
        case ADMIN_GET_VALUES: {
            // K1EL WK2 datasheet v23 Table 14: emit a 14-byte reply
            // with every parameter the host may want at init. We only
            // have reliable values for the WPM byte (offset 1); the
            // remaining 13 bytes are zero-filled, which any K3NG-aware
            // host accepts gracefully. Hosts (RUMlogNG, N1MM) typically
            // issue this once at open as part of their init probe,
            // then follow up with `0x02 N` for any subsequent change.
            // See docs/winkey.md § 16.7.
            uint8_t reply[14] = {0};
            reply[1] = (uint8_t)_wpm;
            for (uint8_t b : reply) emit(b);
            return;
        }
        default:
            // Calibrate, A2D, other get-values, EEPROM, baud: accepted-and-
            // ignored in the core subset (§ 16.6). Note: some of these
            // carry parameters we do not consume here; the core targets
            // the commands real loggers actually send on open.
            return;
    }
}

void WinkeyBridge::applyCommand(uint8_t cmd, const uint8_t* p, uint8_t n) {
    switch (cmd) {
        case WK_SPEED: {
            if (n >= 1 && p[0] != 0) {         // 0 = "use pot"; ignore
                _wpm = clampi(p[0], 5, 99);
                // Stamp the host's own value so the next syncWpmFromLocal()
                // (which sees MorseModel's mirrored WPM) is a no-op —
                // feedback suppression for the host→keyer path.
                _lastPushedWpm = _wpm;
                if (_cb.setWpm) _cb.setWpm(_wpm, _cb.ctx);
            }
            return;
        }
        case WK_SIDETONE: {
            if (n >= 1) {
                _sidetoneHz = sidetoneIndexToHz(p[0]);
                if (_cb.setSidetoneHz) _cb.setSidetoneHz(_sidetoneHz, _cb.ctx);
            }
            return;
        }
        case WK_PAUSE: {
            // 0x06 param: 1 = pause/hold sending, 0 = resume. We surface
            // as output-enable intent for now (§ 16.6 defers PTT timing).
            return;
        }
        case WK_BACKSPACE:
            _buffer.backspace();
            return;
        case WK_CLEAR_BUF:
            _buffer.clear();
            // CLEAR_BUF is the host-driven "drop everything, we're
            // done with this session" signal. The next bytes that
            // arrive are a fresh, independent playback, not a
            // continuation — reset _isContinuation so the next
            // cbSendText() (when the host re-feeds text after a
            // fresh buffer) tells MorseGenerator to skip the
            // boundary prepend.
            _isContinuation = false;
            if (_cb.stopSending) _cb.stopSending(_cb.ctx);
            return;
        case WK_KEY_IMMED: {
            // 0x0B param: 1 = key down (tune), 0 = key up. Treat as an
            // output-enable pulse intent; A1Keyer's RadioKeyer gates it.
            if (n >= 1 && _cb.setOutputEnable)
                _cb.setOutputEnable(p[0] != 0, _cb.ctx);
            return;
        }
        case WK_REQ_STATUS:
            emit(statusByte());
            _primed = true;
            return;
        case WK_GET_POT:
            // K1EL WK2 / K3NG convention: reply with the same single
            // byte the live pin-event uses — `(wpm - low) | 0x80`. The
            // 0x80 bit is the documented "speed pot valid" indicator;
            // the low 7 bits carry the offset from the pot-low WPM.
            // k3ng_cw_keyer/k3ng_keyer/k3ng_keyer.ino:11786-11795.
            emit(speedPotValue());
            _primed = true;
            return;
        case WK_SET_POT: {
            // WK2 § 8.1: 0x05 <low> <range> <scale>. p[0] is the pot-low
            // WPM, p[1] is the range (so max WPM = low + range), and p[2]
            // is the ADC full-scale (0 = 1022, 127 = 511, 255 = 1031,
            // per K3NG k3ng_keyer.ino:10865-10880). We have no physical
            // pot so the scale byte is informational only — we store
            // the WPM range and use it for the (wpm - low) | 0x80
            // encoding in `speedPotValue()` and `setWpmFromLocal()`.
            if (n >= 2) {
                _potWpmLow  = clampi(p[0], 5, 99);
                _potWpmHigh = clampi(_potWpmLow + p[1], _potWpmLow, 99);
            }
            return;
        }
        case WK_SETMODE:      if (n >= 1) _keyerMode  = p[0]; return;
        case WK_WEIGHTING:    if (n >= 1) _weight     = p[0]; return;
        case WK_FARNSWORTH:   if (n >= 1) _farnsworth = p[0]; return;
        case WK_RATIO:        if (n >= 1) _ratio      = p[0]; return;
        case WK_PTT_TIMES:    if (n >= 2) { _pttLead = p[0]; _pttTail = p[1]; } return;
        // Buffered / extension / config commands: accepted, no effect yet.
        default:
            return;
    }
}

void WinkeyBridge::poll() {
    // Query the consumer's audio state up front — drives the
    // `_isContinuation` lifecycle below. RUMlogNG / N1MM chunk their
    // outgoing-CW stream into multiple bridge chunks when bytes arrive
    // faster than audio can drain; in that case the audio is still
    // busy (canAcceptText=false) when the next chunk's bytes are
    // accumulated, and the next cbSendText must keep isContinuation
    // true so MorseGenerator prepends the boundary CHAR_SPACE the
    // encoder omits at chunk tail.
    const bool consumerBusy =
        _cb.canAcceptText && !_cb.canAcceptText(_cb.ctx);
    // Edge-detect: was the consumer busy on the previous poll()?
    // On the EDGE busy→idle AND the buffer is non-empty right now,
    // those new bytes are NOT a continuation of the just-finished
    // chunk — they are a fresh, independent playback the user (or
    // host) initiated after the previous audio wound down. Reset
    // _isContinuation so cbSendText() below calls playText(text,
    // false) and MorseGenerator skips the prepend.
    //
    // The chunked-stream case ("audio still busy when bytes arrive,
    // audio finishes, accumulated buffer drains as chunk 2") does
    // NOT observe this edge: the buffer has been non-empty the
    // whole time, so we never enter the `_buffer.empty()` branch
    // while the audio was busy. Only the "audio wound down, then
    // user tapped web UI to start a new playback" case fits.
    const bool consumerWentIdleNow = _prevConsumerBusy && !consumerBusy;

    if (_buffer.empty()) {
        // Track the busy/idle edge for the NEXT poll() (one-tick
        // delay). If we observed the busy→idle edge this tick but
        // the buffer is empty (no waiting bytes yet), it's a
        // benign edge (audio just finished, no fresh playback has
        // arrived yet).
        if (consumerWentIdleNow) {
            _sawAudioEndEdge = true;
            // The flag's MEANING is "the next cbSendText() is a
            // continuation". As soon as the previous chunk's
            // audio has fully wound down, that meaning is stale —
            // any subsequent cbSendText() is by definition a new
            // playback. Clear _isContinuation NOW so callers
            // (the isContinuation() accessor queried by tests
            // and the device hook) see the correct state
            // immediately, instead of having to wait for a fresh
            // playback to drain before the latch clears. The
            // chunked-stream case ("audio busy, bytes arrive
            // during audio, audio finishes, accumulated buffer
            // drains as continuation") is still handled below in
            // the non-empty-buffer branch — see `_sawAudioEndEdge`
            // and the `_prevBufferNonEmpty` discussion there.
            _isContinuation = false;
        }
        _prevConsumerBusy = consumerBusy;
        _prevBufferNonEmpty = false;   // we are in the empty branch
        return;
    }
    // Buffer has pending text this tick. The pending bytes are
    // either:
    //   (a) chunk 2 of the same chunked host session — preceded by
    //       "audio busy, bytes arrive during audio, audio
    //       finishes, accumulated bytes drain as continuation".
    //       This case never saw the empty-buffer branch fire
    //       during audio playback (buffer had bytes the whole
    //       time), so `_sawAudioEndEdge` is false here, and the
    //       busy→idle edge fires AT THIS poll() in the
    //       consumerWentIdleNow variable computed above. AND the
    //       buffer was non-empty during the previous poll
    //       (audio was busy AND buffer had bytes waiting).
    //   (b) a fresh independent playback the user (or host)
    //       started after the previous audio wound down —
    //       preceded by audio finished (busy→idle), buffer sat
    //       EMPTY, user fed bytes. Two flavours:
    //         (b1) bytes arrive on the SAME poll() that sees the
    //              busy→idle edge (consumerWentIdleNow is true,
    //              _prevBufferNonEmpty is false)
    //         (b2) bytes arrive LATER, after the audio had been
    //              idle for ≥1 poll. _sawAudioEndEdge was set by
    //              the empty-buffer branch in the idle poll.
    //
    // For case (a), we RESTORE _isContinuation = true so
    // MorseGenerator prepends the boundary CHAR_SPACE the encoder
    // omits at chunk tail (the user's "UR 5NN TU sounds like X"
    // regression). For both flavours of (b), _isContinuation
    // must stay false — these bytes are a fresh playback.
    //
    // The disambiguator is `_prevBufferNonEmpty`: in case (a) the
    // buffer was non-empty during the busy phase; in case (b1)
    // the buffer was empty when audio went idle; in case (b2)
    // the audio wound down ≥1 poll ago (so by definition the
    // buffer was empty then too — anything in the buffer now
    // arrived after).
    if (consumerWentIdleNow) {
        if (_prevBufferNonEmpty) {
            // Case (a) — audio just finished AND bytes were
            // accumulating in the buffer during the busy phase.
            // Restore continuation so cbSendText() prepends the
            // boundary gap.
            _isContinuation = true;
        } else {
            // Case (b1) — audio just finished but the buffer was
            // empty (no bytes accumulated during the busy phase).
            // Bytes arrived simultaneously with — or just after —
            // the audio finishing. Fresh playback.
            _isContinuation = false;
        }
        _sawAudioEndEdge = false;
    } else if (_sawAudioEndEdge) {
        // Case (b2) — audio finished on a previous poll()'s
        // empty-buffer branch (cleared _isContinuation there).
        // Bytes now arriving are a fresh playback — keep
        // _isContinuation false. Consume the latched edge so we
        // don't re-process it next tick.
        _sawAudioEndEdge = false;
    }
    _prevConsumerBusy = consumerBusy;
    _prevBufferNonEmpty = !_buffer.empty();

    // If the consumer is still playing the previous chunk, do NOT drain.
    // The text stays in _buffer and the next poll() will drain+send it
    // as one larger chunk once the consumer is ready. This stops the
    // per-character audio restart (and the audible click at every
    // playText() boundary) that we get when hosts stream text faster
    // than MorseGenerator can play it. See docs/winkey.md "Text
    // playback and the audio click bug".
    if (consumerBusy) return;
    // Drain accumulated text to the send hook as one chunk. The device
    // hook hands it to MorseGenerator (async audio + keying); host tests
    // record the string.
    char chunk[WinkeyBuffer::kCapacity + 1];
    size_t nch = 0;
    while (!_buffer.empty() && nch < WinkeyBuffer::kCapacity) {
        chunk[nch++] = _buffer.pop();
    }
    chunk[nch] = '\0';
    if (nch > 0 && _cb.sendText) _cb.sendText(chunk, _cb.ctx);
    // From here on, the next sendText() that arrives WHILE this
    // audio is still playing is a continuation of THIS chunk — its
    // audio is still in flight when the next chunk arrives, so the
    // encoder will not emit a trailing CHAR_SPACE and
    // MorseGenerator must prepend one to preserve the gap. The flag
    // is reset back to false on the busy→idle edge above when a
    // new playback is detected.
    _isContinuation = true;
}

uint8_t WinkeyBridge::statusByte() const {
    // WK2 status byte: 3-MSB tag 110 (0xC0). Bits per § 12.2.
    uint8_t s = 0xC0;
    if (_buffer.xoff())  s |= 0x11;   // WAIT (bit4) + XOFF (bit0)
    if (!_buffer.empty()) s |= 0x04;  // BUSY (bit2) — chars pending
    return s;
}
