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
    _buffer.clear();
}

void WinkeyBridge::emit(uint8_t byte) {
    if (_out) _out(byte, _outCtx);
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
                // Text to send as CW. Accepted only after host-open.
                if (_open) appendText(byte);
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
        default:
            // Calibrate, A2D, get-values, EEPROM, baud: accepted-and-
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
            return;
        case WK_GET_POT:
            // No physical pot; report current WPM offset as 0 (top bit set
            // per WK convention). Minimal, keeps hosts happy.
            emit(0x80);
            return;
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
