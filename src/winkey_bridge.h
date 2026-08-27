#pragma once
/**
 * winkey_bridge.h — K1EL WinKeyer 2.x protocol emulator (core).
 *
 * Parses the WK2 serial byte stream from a host logger (RUMlogNG, N1MM,
 * fldigi) and translates it into A1Keyer actions: set WPM, set sidetone,
 * enable output, and send text as CW. See docs/winkey.md.
 *
 * PROTOCOL MODEL (verified against the K3NG reference implementation,
 * k3ng_keyer.ino service_winkey()):
 *   - Bytes 0x00–0x1F are COMMANDS. 0x00 is the admin prefix (a second
 *     byte selects the admin sub-command). Each command consumes a fixed
 *     number of parameter bytes.
 *   - Bytes >= 0x20 are TEXT: appended to the send buffer and keyed as CW
 *     immediately. Lowercase is upper-cased; '|' (0x7C) is a half-space.
 *   - There is NO load/play framing for normal sending — the host just
 *     streams ASCII.
 *   - Echo: command/parameter bytes are NOT echoed. Each sent text
 *     character is echoed back to the host AFTER it is keyed (the device
 *     glue calls echoSentChar() when the generator consumes a char).
 *   - Host-open (0x00 0x02) replies with the version byte (0x17 = WK2
 *     rev 2.3; hosts accept >= 0x10 as "a WinKeyer").
 *
 * TRANSPORT- AND PLATFORM-AGNOSTIC BY DESIGN.
 *   Input  : the transport calls feed(byte) for each received byte.
 *   Output : the bridge emits bytes (version, status, char echo) through
 *            an OutputFn callback.
 *   Effects: WPM / sidetone / output-enable / send-text are delivered
 *            through a Callbacks struct of function pointers.
 * Nothing here includes Arduino, ESP-IDF, MorseModel, or AudioEngine, so
 * the whole protocol is unit-testable on the host with spy callbacks
 * (test/test_winkey_bridge/).
 */
#include <cstdint>
#include <cstddef>
#include "winkey_buffer.h"

class WinkeyBridge {
public:
    /// Emit one byte toward the host (version / status / char echo).
    using OutputFn = void (*)(uint8_t byte, void* ctx);

    /// Side-effect hooks into the rest of the firmware. Any may be null
    /// (accept-and-store, no effect — still valid for status readback).
    /// Values are clamped to WK ranges before the hook; the hook may
    /// clamp again to its own domain (e.g. MorseModel::setWPM → [5,50]).
    struct Callbacks {
        void (*setWpm)(int wpm, void* ctx)               = nullptr;
        void (*setSidetoneHz)(int hz, void* ctx)         = nullptr;
        void (*setOutputEnable)(bool on, void* ctx)      = nullptr;
        // Send accumulated text as CW. Called from poll() when the send
        // buffer has content AND canAcceptText() returns true. The
        // device hook hands it to MorseGenerator; if the generator is
        // already mid-playback, the hook should return without calling
        // playText() and let the buffer accumulate for the next poll.
        void (*sendText)(const char* text, void* ctx)    = nullptr;
        // Query: is the consumer ready to accept a new sendText() call?
        // poll() skips draining the buffer when this returns false, so
        // rapid back-to-back text bytes accumulate into one chunk
        // instead of restarting playback on every char. Default
        // (nullptr) is "always accept" — backwards compatible.
        bool (*canAcceptText)(void* ctx)                 = nullptr;
        void (*stopSending)(void* ctx)                   = nullptr;   // 0x0A clear
        // ─── TX-buffer side-band (A1Keyer vendor extension) ────────────────
        // When txBufferLoadMode() is true, text bytes / 0x08 / 0x0A
        // route here instead of the live WinkeyBuffer. The device
        // glue implements these against MorseModel::_txBuffer.
        // nullptr in host unit tests (LOAD mode is then a no-op sink).
        void (*txBufferFeed)(char c, void* ctx)          = nullptr;   // text byte in LOAD mode
        void (*txBufferBackspace)(void* ctx)             = nullptr;   // 0x08 in LOAD mode
        void (*txBufferClear)(void* ctx)                 = nullptr;   // 0x0A in LOAD mode
        void (*txBufferLoad)(void* ctx)                  = nullptr;   // ADMIN_TX_BUFFER_LOAD
        void (*txBufferStart)(void* ctx)                 = nullptr;   // ADMIN_TX_BUFFER_START
        // Query: does the TX buffer have unsent chars? Used to set bit
        // 4 of the WK2 status byte so hosts can poll readiness. nullptr
        // → bit 4 stays clear (live-stream-only behaviour).
        bool (*txBufferHasPending)(void* ctx)            = nullptr;
        void* ctx                                        = nullptr;
    };

    WinkeyBridge() = default;

    /// Wire the output sink and effect callbacks. Call once at startup.
    void begin(OutputFn out, void* outCtx, const Callbacks& cb);

    /// Feed one received byte through the protocol state machine.
    void feed(uint8_t byte);

    /// Flush any accumulated send-text to the sendText hook. Call from
    /// loop() after feeding available bytes.
    void poll();

    /// Current WK2 status byte (3-MSB tag 110 — § 12.2).
    uint8_t statusByte() const;

    /// True once the host has issued Host-Open (§ 5). Commands other than
    /// admin are ignored until then.
    bool isOpen() const { return _open; }

    /// True once the host has completed its initial probe by asking us a
    /// question that we answer (GET_POT → 0x80, REQ_STATUS → statusByte).
    /// Until then, text bytes are silently swallowed even though `_open`
    /// is true. This suppresses host-init artefacts like RUMlogNG
    /// streaming bytes from its outgoing-CW buffer during the very first
    /// few ms after open — see docs/winkey.md § 13.9.
    bool isPrimed() const { return _primed; }

    /// WK version byte reported on host-open. 0x17 = WK2 rev 2.3.
    static constexpr uint8_t kVersion = 0x17;

    /// True when the next sendText() the bridge is about to dispatch
    /// is a CONTINUATION of a chunked playback (i.e. the previous
    /// chunk's audio just finished and the encoder does not emit a
    /// trailing inter-character silence, so MorseGenerator must
    /// prepend a CHAR_SPACE to preserve the spec's 3-unit gap). False
    /// for the FIRST chunk in a session (no previous chunk) — that
    /// chunk is a fresh, independent playback and the prepend would
    /// shift the decoded text by one position. The device-side hook
    /// in winkey.cpp reads this via isContinuation() and passes it
    /// to MorseGenerator::playText(text, isContinuation). See
    /// docs/winkey.md § 13.6 and the regression test
    /// test_back_to_back_independent_playback_no_prepend.
    bool isContinuation() const { return _isContinuation; }

    /// Reset all protocol state including `_open=false`. Used by host
    /// tests between RUN() cases; the wire-facing admin-reset path does
    /// NOT call this — it must keep the host interface open so a
    /// defensive reset from the host (N1MM / RUMlogNG / fldigi issue
    /// one as part of their init sequence) doesn't silently drop every
    /// subsequent command. See handleAdmin(ADMIN_RESET) and
    /// docs/winkey.md § 5.1.
    void resetForTest();

    /// Restore default parameter values, idle the parser, and clear
    /// the send buffer. Does NOT touch `_open` or `_wk2Mode`. Used by
    /// both the test entry point and the wire-facing admin-reset path.
    void resetParams();

    // ─── Stored parameters (exposed for tests / status readback) ─────────
    int  wpm() const        { return _wpm; }
    int  sidetoneHz() const { return _sidetoneHz; }
    bool outputEnabled() const { return _outputEnable; }
    uint8_t keyerMode() const { return _keyerMode; }

    /// A1Keyer's absolute WPM range. MorseModel clamps to [5, 50];
    /// the wire-facing clamp stays at the WK2 spec's [5, 99] for
    /// hosts that send SetSpeed values outside the audio range.
    static constexpr int kWpmMin = 5;
    static constexpr int kWpmMax = 50;

    /// Update the bridge's WPM from a local source (keyboard / NVS) and
    /// emit a single-byte speed-pot-change notification to the host so
    /// K3NG-compatible loggers (RUMlogNG, N1MM, fldigi) mirror the
    /// change in their UI. The byte is encoded as
    /// `(wpm - _potWpmLow) | 0x80` — the documented K1EL WK2 / K3NG
    /// idiom for "speed pot changed" (top bit set, low 7 bits = the
    /// offset from the pot's low WPM). No-op before host-open.
    /// Idempotent — when the polled value matches the last value the
    /// host sent us (tracked in `_lastPushedWpm`), the emit is
    /// suppressed so the host never sees its own `0x02 N` echoed back.
    /// See docs/winkey.md § 14.1, § 16.7.
    void setWpmFromLocal(int wpm);

    /// Current speed-pot-byte encoding of the bridge WPM. Returns
    /// `(wpm - _potWpmLow) | 0x80` clamped so the low 7 bits stay in
    /// [0, 127]. This is the byte emitted by both the local push
    /// (`setWpmFromLocal`) and by the host's `0x07` GET_POT query.
    /// See docs/winkey.md § 14.1, § 16.7.
    uint8_t speedPotValue() const;

    /// Emit a decoded paddle character (or word-space) toward the host
    /// so K3NG-compatible loggers (RUMlogNG, N1MM) can mirror the
    /// operator's manual keying in their CW log. Mirrors K3NG's
    /// `winkey_port_write(convert_cw_number_to_ascii(...), 0)` at
    /// `k3ng_keyer.ino:11627`. No-op before host-open or before the
    /// host has primed the bridge (so the first decoder output during
    /// a stream-of-text open is not double-echoed). Lowercase is
    /// upper-cased to match the WK text-byte convention. The character
    /// is delivered as a single ASCII byte via the standard output
    /// sink. See docs/winkey.md § 16.8.
    void emitDecodedChar(char c);

    // ─── TX-buffer extensions (A1Keyer-specific) ─────────────────────────────
    //
    // Three new admin sub-commands expose a TX-buffer mode on the WK2
    // interface. The shared buffer is MorseModel::_txBuffer (the same
    // buffer the web UI's /api/tx/* endpoints drive). Live-stream mode
    // is the default and is unchanged — hosts that don't know about
    // these commands see no difference. See docs/tx_buffer.md § 5 and
    // docs/winkey.md § 17.

    /// True when text bytes (≥ 0x20) accumulate into MorseModel::_txBuffer
    /// instead of the live WinkeyBuffer (which keys immediately). 0x08
    /// (backspace) and 0x0A (clear) target the TX buffer in this mode.
    bool txBufferLoadMode() const { return _txBufferLoadMode; }

    /// Manually set the load mode. Called from the admin sub-command
    /// handlers. Idempotent. The default constructor leaves it false.
    /// `resetForTest()` / `resetParams()` also clear it.
    void setTxBufferLoadMode(bool on) { _txBufferLoadMode = on; }

private:
    // Parser state: are we mid-command awaiting parameter byte(s)?
    enum class Parse : uint8_t {
        IDLE,          // expecting a command byte or text
        ADMIN,         // saw 0x00, expecting the admin sub-command
        NEED_PARAMS,   // collecting _paramsNeeded bytes for _pendingCmd
    };

    void dispatchCommand(uint8_t cmd);      // a command byte 0x00-0x1F
    void applyCommand(uint8_t cmd, const uint8_t* params, uint8_t n);
    void handleAdmin(uint8_t sub);
    void appendText(uint8_t byte);          // a text byte >= 0x20
    void emit(uint8_t byte);                // via _out

    static uint8_t paramCountFor(uint8_t cmd);   // 0 for no-param commands
    static int     sidetoneIndexToHz(uint8_t byte);

    // Wiring
    OutputFn  _out    = nullptr;
    void*     _outCtx = nullptr;
    Callbacks _cb{};

    // Protocol state
    bool    _open        = false;
    bool    _wk2Mode     = false;
    bool    _primed      = false;   // set true after first GET_POT/REQ_STATUS reply
    Parse   _parse       = Parse::IDLE;
    uint8_t _pendingCmd  = 0;
    uint8_t _params[3]   = {0, 0, 0};    // max param count we handle is 3 (pot)
    uint8_t _paramGot    = 0;
    uint8_t _paramsNeeded = 0;

    // Parameters (defaults per WK2 datasheet Table 3, § 6.3)
    int     _wpm         = 20;
    int     _sidetoneHz  = 600;
    bool    _outputEnable = true;
    uint8_t _keyerMode   = 1;            // 1 = iambic B

    // Last value pushed from local (MorseModel → bridge → host).
    // -1 forces the next setWpmFromLocal() call to emit
    // unconditionally once _open is true. Cleared by resetParams().
    // See docs/winkey.md § 16.7 (feedback suppression).
    int     _lastPushedWpm = -1;

    // Speed pot range configured by the host via `0x05 <low> <range> <scale>`
    // (WK2 § 8.1). The low 7 bits of the speed-pot byte are
    // `(wpm - _potWpmLow)`. Default matches A1Keyer's effective WPM
    // range [5, 50] so the bridge is usable without host configuration;
    // a real K1EL Winkeyer's physical pot sets these via the A2D
    // channels. See docs/winkey.md § 14.1.
    int     _potWpmLow  = kWpmMin;     // 5
    int     _potWpmHigh = kWpmMax;     // 50
    // Store-only params (status readback; no audio effect yet — § 16.6)
    uint8_t _weight = 50, _farnsworth = 0, _pttTail = 5, _pttLead = 5;
    uint8_t _hangTime = 0, _ratio = 50;

    // CW send buffer (text >= 0x20 accumulates here, drains in poll()).
    WinkeyBuffer _buffer;

    // TX-buffer mode flag (A1Keyer vendor extension). When true, text
    // bytes / backspace / clear are routed to MorseModel::_txBuffer
    // (the shared web UI TX buffer) instead of _buffer. Switched by
    // ADMIN_TX_BUFFER_LOAD, exited by ADMIN_TX_BUFFER_START. Cleared
    // by resetParams() and resetForTest(). See docs/tx_buffer.md § 5.
    bool _txBufferLoadMode = false;

    // Chunked-playback continuation tracker. Set false at session
    // boundaries (resetParams, WK_CLEAR_BUF, busy→idle edge with
    // fresh bytes waiting); set true after every cbSendText()
    // call (so the next chunk in the SAME session is flagged as a
    // continuation). The device hook in winkey.cpp reads
    // isContinuation() and passes it to MorseGenerator::playText()
    // to gate the boundary prepend — see playText()'s docs for
    // the rationale.
    bool _isContinuation = false;

    // Previous tick's consumer-busy state. Used by poll() to edge
    // detect the busy→idle transition of the MorseGenerator. When
    // the audio just finished (busy→idle) AND fresh bytes are
    // waiting in the buffer, the next cbSendText() is a fresh
    // playback, not a continuation — _isContinuation must be
    // cleared so MorseGenerator skips the boundary prepend.
    bool _prevConsumerBusy = false;

    // Latched "the consumer's audio just transitioned from busy to
    // idle" flag. Set by poll() when it observes a busy→idle edge
    // while the buffer is empty (audio just wound down, no fresh
    // bytes yet). Read+cleared by the NEXT poll() that finds the
    // buffer non-empty — at that point the bytes that arrived are
    // a fresh, independent playback, and _isContinuation must be
    // cleared before cbSendText() fires so MorseGenerator skips
    // the boundary prepend.
    bool _sawAudioEndEdge = false;

    // Previous tick's buffer-occupancy state. Used by poll() to
    // disambiguate the chunked-stream case ("bytes accumulated in
    // the bridge buffer while audio was busy; audio finished;
    // accumulated bytes drain as a continuation") from the
    // one-char-at-a-time case ("audio finished naturally; bytes
    // arrive one-by-one after; each byte is its own fresh
    // playback, not a continuation"). On the busy→idle edge with
    // the buffer currently non-empty, we set _isContinuation=true
    // ONLY if the buffer was already non-empty during the busy
    // phase (i.e. bytes were accumulating). If the buffer was
    // empty when the audio finished, the bytes that subsequently
    // arrive are a fresh playback. See winkey_bridge.cpp poll().
    bool _prevBufferNonEmpty = false;
};
