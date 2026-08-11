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
    if (_buffer.empty()) return;
    // If the consumer is still playing the previous chunk, do NOT drain.
    // The text stays in _buffer and the next poll() will drain+send it
    // as one larger chunk once the consumer is ready. This stops the
    // per-character audio restart (and the audible click at every
    // playText() boundary) that we get when hosts stream text faster
    // than MorseGenerator can play it. See docs/winkey.md "Text
    // playback and the audio click bug".
    if (_cb.canAcceptText && !_cb.canAcceptText(_cb.ctx)) return;
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
}

uint8_t WinkeyBridge::statusByte() const {
    // WK2 status byte: 3-MSB tag 110 (0xC0). Bits per § 12.2.
    uint8_t s = 0xC0;
    if (_buffer.xoff())  s |= 0x11;   // WAIT (bit4) + XOFF (bit0)
    if (!_buffer.empty()) s |= 0x04;  // BUSY (bit2) — chars pending
    return s;
}
