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
        // buffer has content. The device hook hands it to MorseGenerator.
        void (*sendText)(const char* text, void* ctx)    = nullptr;
        void (*stopSending)(void* ctx)                   = nullptr;   // 0x0A clear
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

    /// WK version byte reported on host-open. 0x17 = WK2 rev 2.3.
    static constexpr uint8_t kVersion = 0x17;

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
    // Store-only params (status readback; no audio effect yet — § 16.6)
    uint8_t _weight = 50, _farnsworth = 0, _pttTail = 5, _pttLead = 5;
    uint8_t _hangTime = 0, _ratio = 50;

    // CW send buffer (text >= 0x20 accumulates here, drains in poll()).
    WinkeyBuffer _buffer;
};
