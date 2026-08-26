#pragma once
/**
 * display_model.h — MorseModel singleton: observable state for the display system.
 *
 * This singleton holds all UI state read by CardputerDisplay (Core 0) and written
 * by the audio thread (Core 1) and loop() (Core 0). All members are std::atomic
 * so cross-core communication is lock-free without mutexes.
 *
 * Thread safety:
 *   - Audio thread writes: keyerPatternPercent, encoderChar
 *   - loop() writes: screen, mode, decoded text, wpm, frequency, overlay, display active
 *   - Display task reads: all members (polling, no consumer-lock needed)
 *
 * The key invariant: no mutex or critical section is needed because the std::atomic
 * operations are individually safe across cores, and the display task only reads
 * values that have atomic store visibility on the writing core.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "net_config.h"
#include "memory_store.h"
#include "MorseTable.h"

class TextInput;   // forward: passwordInput() returns TextInput*

// ─── Enum definitions ─────────────────────────────────────────────────────────

/** Display screen modes. */
enum class DisplayScreen {
    DECODER,     ///< Default: scrolling decoded morse text in the main area.
    WPM_VIEW,    ///< Overlay: large WPM readout, auto-returns after OVERLAY_TIMEOUT_MS.
    WPM_SETTINGS, ///< Settings: in-place WPM editing with ;/., confirmed with Enter.
    FREQ_VIEW,   ///< Overlay: large frequency readout, auto-returns after OVERLAY_TIMEOUT_MS.
    FREQ_SETTINGS, ///< Settings: in-place frequency editing with ;/., confirmed with Enter.
    VOLUME_VIEW,   ///< Overlay: large volume readout, auto-returns after OVERLAY_TIMEOUT_MS.
    VOLUME_SETTINGS, ///< Settings: in-place volume editing with ;/., confirmed with Enter.
    MODE_VIEW,   ///< Overlay: large mode readout, auto-returns after OVERLAY_TIMEOUT_MS.
    MODE_SETTINGS, ///< Settings: in-place mode editing with ;/., confirmed with Enter.
    KEYING_SETTINGS,  ///< Settings: in-place on/off radio-keying toggle, ;/., confirmed with Enter.
    POLARITY_SETTINGS, ///< Settings: in-place paddle-polarity toggle (Normal/Reversed), ;/., confirmed with Enter.
    WIFI_SCAN_LIST,   ///< Network config: scan list / "Scanning…" / scan failed message.
    WIFI_PASSWORD_INPUT, ///< Network config: passphrase entry (TextInput-backed).
    WIFI_NETWORK_INFO,   ///< Network config: status, SSID, IP / error, X to forget, R to retry.
    WIFI_AP_PASSWORD_INPUT, ///< Network config: AP passphrase entry (TextInput-backed, SSID = "A1Keyer").
    WIFI_NETWORK_INFO_CONFIRM, ///< Network config: Y/N confirmation overlay for "forget network".

    MEMORY_PICK,         ///< Memory keyer: M opened, awaiting digit 0-9 to choose the slot to edit.
    MEMORY_EDIT,         ///< Memory keyer: slot picked, editing CW text via TextInput (same control as the Wi-Fi password screen).

    WABUN_SETTINGS       ///< Wabun mode selection: International / Wabun Katakana / Wabun Hiragana.
};

/** Keyer operating mode. */
enum class KeyerMode {
    KEYER,    ///< Morse paddle key (Iambic B) or straight key drives the decoder.
    ENCODER   ///< Morse encoder plays back stored text as morse audio.
};

/** Physical keyer type (Iambic B paddle vs straight key). */
enum class KeyerType {
    PADDLE,   ///< Iambic B dual-paddle keyer.
    STRAIGHT  ///< Single-contact straight key.
};

// ─────────────────────────────────────────────────────────────────────────────

class MorseModel {
public:
    // ─── Singleton ─────────────────────────────────────────────────────────

    /**
     * instance — get the MorseModel singleton.
     *
     * @return Reference to the single MorseModel instance.
     *
     * Usage:
     *   auto& m = MorseModel::instance();
     *   m.setWPM(25);
     */
    static MorseModel& instance();

    // ─── Screen state ──────────────────────────────────────────────────────

    /**
     * screen — current display screen.
     *
     * @return Current DisplayScreen enum value.
     */
    DisplayScreen screen() const;

    /**
     * setScreen — change the active display screen.
     *
     * @param s  New screen (DECODER, WPM_VIEW, or FREQ_VIEW).
     *
     * Calling setScreen increments the change counter, triggering a display
     * render on the next ~20fps poll cycle.
     */
    void setScreen(DisplayScreen s);

    // ─── Keyer mode ─────────────────────────────────────────────────────────

    /**
     * mode — current keyer mode.
     *
     * @return KEYER (paddle-driven) or ENCODER (playback-driven).
     */
    KeyerMode mode() const;

    /**
     * setMode — change the keyer mode.
     *
     * @param m  New mode (KEYER or ENCODER).
     *
     * ENCODER mode is set when the user presses P to start text playback.
     * The status line shows "ENCODER" and the main area shows the playing char.
     */
    void setMode(KeyerMode m);

    // ─── Keyer type (Paddle / Straight) ────────────────────────────────────────

    /**
     * keyerType — current physical keyer type.
     * @return PADDLE (Iambic B) or STRAIGHT (single-contact key).
     */
    KeyerType keyerType() const;

    /**
     * setKeyerType — change the physical keyer type.
     * @param t  PADDLE or STRAIGHT.
     */
    void setKeyerType(KeyerType t);

    // ─── Radio keying output ──────────────────────────────────────────────────

    /**
     * radioKeyingEnabled — whether the on-air CW keying output is active.
     *
     * When true, GPIO4 (Cardputer EXT header G4) is driven HIGH during any
     * dit/dah and LOW during spaces, mirroring the CW output of whichever
     * keyer (iambic or straight) or the held `K` keyboard key.
     *
     * @return true if radio keying is enabled; default false.
     */
    bool radioKeyingEnabled() const;

    /**
     * setRadioKeyingEnabled — toggle the on-air keying output.
     *
     * @param enabled  true to enable, false to disable (force GPIO LOW).
     *
     * Off-to-on does NOT restore stale demand from any still-held key
     * — a fresh element / key-press is required to key the transmitter.
     * Persisted to NVS by main.cpp on Enter from KEYING_SETTINGS.
     */
    void setRadioKeyingEnabled(bool enabled);

    // ─── Paddle polarity ──────────────────────────────────────────────────────

    /**
     * polarityReversed — whether the iambic paddle levers are swapped.
     *
     * When true, the physical dit lever sounds a dah and vice versa, exactly as
     * if the paddle cable had been rewired. Affects ONLY the iambic keyer — the
     * straight key is unaffected.
     *
     * @return true if polarity is reversed; default false (normal).
     */
    bool polarityReversed() const;

    /**
     * setPolarityReversed — swap or unswap the iambic paddle levers.
     *
     * Applied to the running keyer immediately (audible on the next element),
     * and persisted to NVS by main.cpp on Enter from POLARITY_SETTINGS.
     *
     * @param reversed  true to swap dit/dah, false for normal orientation.
     */
    void setPolarityReversed(bool reversed);

    // ─── WinKey / Console mode indicator ───────────────────────────────────

    /**
     * winkeyMode — true when the serial line is in WinKey mode (speaking
     * the WinKeyer protocol to a host logger), false in Console/dev mode.
     * Mirrors Console::mode() so the display task can render an indicator
     * without depending on the Console module. Toggled by the 'D' key.
     */
    bool winkeyMode() const;

    /** Set the WinKey/Console mode indicator (mirror of Console::mode()). */
    void setWinkeyMode(bool on);

    // ─── Morse table mode ───────────────────────────────────────────────────

    MorseTableMode morseTableMode() const;
    void setMorseTableMode(MorseTableMode mode);

    // ─── Decoded text (circular buffer) ────────────────────────────────────
    // The circular buffer holds TEXT_BUF_SIZE (200) characters of decoded morse.
    // Three atomic pointers manage it:
    //   _textHead  — next write position (ring advance index)
    //   _textTail  — index of the oldest valid character (read start)
    //   _textLen   — number of valid characters (0..TEXT_BUF_SIZE)
    //
    // Behaviour:
    //   - Normal (not full): indices 0..textLen-1 are valid, tail==0
    //   - Full: indices tail..tail+textLen-1 (wrapped) are valid, oldest at tail
    //   - Overflow: when len==SIZE and a new char arrives, tail advances by 1
    //                (oldest character is silently dropped)
    //
    // appendDecodedChar writes at _textHead, then advances head.
    // renderScrollingText reads starting at _textTail for length _textLen.

    /// Maximum decoded-text capacity. Public so external readers (e.g.
    /// the HTTP /state handler) can iterate the circular buffer.
    static constexpr size_t TEXT_BUF_SIZE = 200;

    /**
     * decodedText — raw pointer to the text buffer.
     *
     * @return Pointer to the 200-character buffer. May contain null bytes
     *         before textLen valid characters. Use textLen() for the count.
     *
     * WARNING: Do not use this pointer for iteration — use textTail() and
     * textLen() to correctly handle the circular buffer wrap case.
     */
    const char* decodedText() const;

    /**
     * decodedTextLen — number of valid characters currently in the buffer.
     *
     * @return 0 to TEXT_BUF_SIZE inclusive.
     */
    size_t decodedTextLen() const;

    /**
     * appendDecodedChar — append one decoded character to the circular buffer.
     *
     * @param c  The decoded character to append (e.g. 'A', ' ', '.').
     *
     * Algorithm:
     *   1. Write c at _textHead
     *   2. Advance _textHead = (_textHead + 1) % TEXT_BUF_SIZE
     *   3. If len < TEXT_BUF_SIZE: len++
     *      Else (buffer full): advance _textTail = (_textTail + 1) % TEXT_BUF_SIZE
     *
     * After a full cycle (200 chars), the oldest characters are evicted.
     * Each call increments the change counter to trigger a display render.
     */
    void appendDecodedChar(char c);

    /** Append with player-origin flag. */
    void appendDecodedChar(char c, bool fromPlayer);

    /**
     * clearDecodedText — reset the circular buffer.
     *
     * Resets _textHead, _textTail, and _textLen to 0 and zero-fills the buffer.
     * Increments the change counter.
     */
    void clearDecodedText();

    /**
     * textTail — index of the oldest valid character in the circular buffer.
     *
     * @return 0 to TEXT_BUF_SIZE-1 inclusive.
     *
     * Use for iteration:
     *   for (size_t i = 0; i < textLen(); ++i) {
     *       size_t idx = (textTail() + i) % TEXT_BUF_SIZE;
     *       char c = textAt(idx);
     *   }
     */
    size_t textTail() const;

    /**
     * textHead — next write position in the circular buffer.
     *
     * @return 0 to TEXT_BUF_SIZE-1 inclusive.
     *
     * This is where the next appendDecodedChar() will write.
     * Note: when the buffer is not full, valid characters are at indices 0..textLen-1,
     * and textHead() == textLen(). When full, textHead() == textTail() (equal indices).
     */
    size_t textHead() const;

    /**
     * textLen — number of valid characters in the circular buffer.
     *
     * @return 0 to TEXT_BUF_SIZE inclusive.
     */
    size_t textLen() const;

    /**
     * textAt — character at a specific buffer index.
     *
     * @param idx  Absolute buffer index (0 to TEXT_BUF_SIZE-1).
     * @return Character at that index, or '\0' if idx >= TEXT_BUF_SIZE.
     *
     * This reads the raw buffer directly. Always use (textTail() + i) % SIZE
     * for wrapped indexing.
     */
    char textAt(size_t idx) const;

    /**
     * attrAt — attribute (K=keyer, P=player) at a specific buffer index.
     *
     * @param idx  Absolute buffer index (0 to TEXT_BUF_SIZE-1).
     * @return ATTR_KEYER ('K') or ATTR_PLAYER ('P'), or '\0' if idx >= TEXT_BUF_SIZE.
     */
    char attrAt(size_t idx) const;

    // ─── WPM ─────────────────────────────────────────────────────────────────

    /**
     * wpm — current words-per-minute speed setting.
     *
     * @return WPM value in range [5, 50].
     */
    int wpm() const;

    /**
     * setWPM — set the WPM speed.
     *
     * @param wpm  Target WPM, clamped to [5, 50].
     *
     * Propagates to the IambicKeyer and MorseGenerator via
     * AudioEngine::keyer()->setWPM() and AudioEngine::morseGen()->setWPM().
     */
    void setWPM(int wpm);

    /**
     * adjustWPM — adjust WPM by a signed delta.
     *
     * @param delta  Amount to add to current WPM (negative to decrease).
     *
     * The result is clamped to [5, 50] so the caller doesn't need to clamp.
     *
     * Example:
     *   model.adjustWPM(+1);   // increase by 1 WPM
     *   model.adjustWPM(-5);   // decrease by 5 WPM
     */
    void adjustWPM(int delta);

    // ─── Side tone frequency ─────────────────────────────────────────────────

    /**
     * frequency — current side tone frequency in Hz.
     *
     * @return Frequency in range [400, 1000].
     */
    float frequency() const;

    /**
     * setFrequency — set the side tone frequency.
     *
     * @param hz  Target frequency in Hz, clamped to [300, 900].
     */
    void setFrequency(float hz);

    /**
     * adjustFrequency — adjust frequency by a signed delta.
     *
     * @param delta  Hz to add to current frequency (negative to decrease).
     *
     * Result clamped to [300, 900].
     *
     * Example:
     *   model.adjustFrequency(+10.0f);  // +10 Hz
     *   model.adjustFrequency(-50.0f);  // -50 Hz
     */
    void adjustFrequency(float delta);

    // ─── Volume ───────────────────────────────────────────────────────────────

    /**
     * volume — current volume setting as a percentage (0–100).
     *
     * @return Volume percentage. 0 = silent, 100 = max.
     */
    int volume() const;

    /**
     * setVolume — set the volume percentage.
     *
     * @param vol  Target volume 0–100, automatically clamped.
     *
     * Propagates to AudioEngine::setVolumePercent() which updates
     * the cached amplitude and codec attenuators.
     */
    void setVolume(int vol);

    /**
     * adjustVolume — adjust volume by a signed delta.
     *
     * @param delta  Amount to add to current volume (negative to decrease).
     *
     * Result clamped to [0, 100].
     */
    void adjustVolume(int delta);

    // ─── Keyer pattern percentage ────────────────────────────────────────────

    /**
     * keyerPatternPercent — DIT/(DIT+DAH) ratio as a percentage.
     *
     * Updated by the audio thread every ~200ms via setKeyerPatternPercent().
     * Used by the display to draw the pattern progress bar.
     *
     * @return 0–100 representing the proportion of DITs in recent keyer history.
     */
    int keyerPatternPercent() const;

    /**
     * setKeyerPatternPercent — update the keyer pattern percentage.
     *
     * @param pct  New percentage 0–100.
     *
     * Called from AudioEngine::fillBuffer() on the audio thread.
     * Increments change counter to trigger display update.
     */
    void setKeyerPatternPercent(int pct);

    // ─── Change notification ─────────────────────────────────────────────────

    /**
     * changeCounter — monotonic counter incremented on every state change.
     *
     * @return Current counter value.
     *
     * The display task polls this every 50ms. When current != last, a render
     * is triggered. Any setter in MorseModel increments it.
     */
    uint32_t changeCounter() const;

    /**
     * incrementChangeCounter — increment the change counter.
     *
     * Called automatically by every public setter. Also callable directly
     * to force a render (e.g. after a timer-based event).
     */
    void incrementChangeCounter();

    // ─── Overlay timeout ─────────────────────────────────────────────────────

    /**
     * overlayStartMillis — timestamp when the current overlay was opened.
     *
     * @return millis() value at the moment setOverlayStartMillis() was called.
     */
    uint32_t overlayStartMillis() const;

    /**
     * setOverlayStartMillis — record the overlay open timestamp.
     *
     * @param ms  Current value of millis() at overlay open time.
     *
     * Call with millis() when opening an overlay (WPM_VIEW or FREQ_VIEW).
     * The display task checks (millis() - overlayStartMillis()) each cycle
     * and auto-returns to DECODER after OVERLAY_TIMEOUT_MS.
     */
    void setOverlayStartMillis(uint32_t ms);

    /** Auto-dismiss overlay after this many milliseconds (10 seconds). */
    static constexpr uint32_t OVERLAY_TIMEOUT_MS = 10000;

    // ─── Screen-saver ─────────────────────────────────────────────────────────

    /**
     * isDisplayActive — whether the display is in active (non-screensaver) mode.
     *
     * @return true = backlight on, rendering at ~20fps. false = backlight off.
     */
    bool isDisplayActive() const;

    /**
     * setDisplayActive — enable or disable the display (screen-saver control).
     *
     * @param on  true to activate display, false to activate screen-saver.
     *
     * When set to false, the display task will turn off the backlight.
     * When set to true, the display task will wake the display and render.
     */
    void setDisplayActive(bool on);

    /**
     * lastActivity — timestamp of the last user activity (keyboard or paddle).
     *
     * @return millis() at last touch() call.
     */
    uint32_t lastActivity() const;

    /**
     * touch — record a user activity event (reset screen-saver timer).
     *
     * Called from requestRender() when a keyboard key is pressed.
     * Also called from DisplayTask::wakeFromScreensaver() when a paddle fires.
     */
    void touch();

    /** Inactivity timeout before screen-saver activates (5 minutes). */
    static constexpr uint32_t DISPLAY_TIMEOUT_MS = 300000;  // 5 min

    // ─── Encoder character display ───────────────────────────────────────────

    /**
     * encoderChar — the character currently being played by the encoder.
     *
     * @return The playing character, or '\0' if idle.
     *
     * Updated by the audio thread during text playback (MorseGenerator).
     * Read by CardputerDisplay::updateMainText() in ENCODER mode to show
     * the current character large and prominently.
     */
    char encoderChar() const;

    /**
     * setEncoderChar — update the currently playing encoder character.
     *
     * @param c  The character being played (e.g. 'A'), or '\0' if between chars.
     *
     * Called from AudioEngine::fillBuffer() every time the MorseGenerator
     * advances to a new element (dit/dah/space).
     */
    void setEncoderChar(char c);

    /** @return true if the last appended decoded character came from the player. */
    bool lastCharFromPlayer() const;

    /** Set whether the last appended decoded char came from the player. */
    void setLastCharFromPlayer(bool v);

    /** Index in circular buffer where player chars begin (0 if none). */
    size_t playerTail() const { return _playerTail.load(std::memory_order_relaxed); }

    /** Index of the first player character in the circular buffer. */
    size_t playerHead() const { return _playerHead.load(std::memory_order_relaxed); }

    /** Reset player head so next player char starts a new session (SIZE_MAX = none). */
    void resetPlayerHead();

    // ─── Wi-Fi screens ───────────────────────────────────────────────────────
    //
    // Atomic mirrors of NetworkManager state for the display task. NetworkManager
    // is the authority; these are populated once per loop() by main.cpp via the
    // setter pair below. The display never reaches into NetworkManager directly
    // so the model owns the observation contract.
    //
    // The error string and connected SSID come straight from NetworkManager in
    // the renderer — they are short-lived strings that change atomically with
    // the state and don't need to be mirrored here.

    /// Current state, as an int so this header stays free of NetworkManager.
    /// Use NetState from network_manager.h for the meaning.
    int wifiState() const;
    void setWifiState(int s);

    /// Local IP, big-endian / network byte order (so `>>24` yields the
    /// first dotted-decimal octet). 0 when not connected.
    uint32_t wifiLocalIP() const;
    void setWifiLocalIP(uint32_t ip);

    /// True when stored credentials are available.
    bool wifiHasCredentials() const;
    void setWifiHasCredentials(bool v);

    /// Where the active credentials came from (NetCredSource int value).
    int wifiCredSource() const;
    void setWifiCredSource(int s);

    /// Current radio role (NetMode int value). 0=STA, 1=AP.
    int wifiMode() const;
    void setWifiMode(int m);

    /// Number of stations currently associated with our AP. Only
    /// meaningful when mode()==ACCESS_POINT.
    int wifiApStations() const;
    void setWifiApStations(int n);

    /// Maximum simultaneous AP clients (kApMaxStations, mirrored for
    /// the renderer).
    int wifiApMaxStations() const;
    void setWifiApMaxStations(int n);

    /// Whether the Y/N "Forget?" overlay is currently drawn on top of
    /// the network-info screen. main.cpp drives this from the X-key
    /// handler in STA mode; the renderer reads it to draw the
    /// overlay; the X-key handler reads it to decide whether Y/N
    /// dismiss it or to enter it.
    bool wifiNetConfirmForget() const;
    void setWifiNetConfirmForget(bool v);

    /// Seconds until the next automatic retry (0 when none is pending).
    uint32_t wifiSecondsUntilRetry() const;
    void setWifiSecondsUntilRetry(uint32_t s);

    /// Cursor row in the scan list. 0..scanCount()-1.
    int wifiScanCursor() const;
    void setWifiScanCursor(int idx);

    /// Number of scan results currently visible (mirrored from the
    /// NetworkManager by main.cpp once per loop()).
    int wifiScanCount() const;
    void setWifiScanCount(int n);

    /// Top row of the visible scan list window. Adjusted to keep the
    /// cursor in [top, top+kPageSize-1].
    int wifiScanTop() const;
    void setWifiScanTop(int top);

    /// Move the scan-list cursor by `delta` and adjust `top` so the new
    /// cursor stays in the visible window. Clamps at the list bounds.
    void wifiAdjustScanCursor(int delta);

    /// Single-line passphrase editor backing the password-entry screen.
    /// Lazily constructed; the buffer lives inside the model so it
    /// survives across visits to the screen. The renderer reads it via
    /// value()/length()/reveal(); main.cpp feeds it.
    TextInput* passwordInput();

    /// Clear the password buffer (e.g. when leaving the screen).
    void wifiClearPassword();

    /// Reset all Wi-Fi UI mirrors to a known idle state (called on
    /// enter to DECODER so a re-entrant scan starts fresh).
    void wifiResetUIState();

    // ─── Memory-keyer state ─────────────────────────────────────────────────
    //
    // Ten CW memory slots, addressed by the digits '0'..'9'. The buffers
    // live inside the model so they survive across visits to MEMORY_PICK
    // and MEMORY_EDIT, mirroring the password-buffer layout used by the
    // Wi-Fi password screen.

    /// Number of memory slots, addressed by keyboard digits '0'..'9'.
    static constexpr uint8_t kMemSlots = ::kMemSlots;

    /// Maximum length of one slot's CW text (includes the terminator).
    /// See memory_store.h for the source of truth.
    static constexpr size_t kMemLen = ::kMemLen;

    /**
     * memoryInput — single-line editor for the currently selected slot.
     *
     * @return The lazily-constructed TextInput. Buffer is caller-owned
     *         (lives at _memory[slot]) so the model needs to be told
     *         which slot to back the editor on — call setMemoryEditingSlot
     *         BEFORE first use. The renderer reads the same buffer via
     *         getMemory(). see text_input.h.
     */
    TextInput* memoryInput();

    /// Clear the memory editor's buffer. Safe to call before construction.
    void memoryClearEditor();

    /// Slot the editor is currently bound to: -1 = none (idle), 0..9 = editing.
    int memoryEditingSlot() const;

    /// Set the slot the editor is bound to. The renderer and playback
    /// paths read this to know which row of _memory to use.
    void setMemoryEditingSlot(int slot);

    /// Slot the picker screen has currently armed: -1 = none, 0..9 = armed.
    /// Used by the picker digit handler so a stray ESC clears the state.
    int memoryPickSlot() const;

    /// Set the picker armed slot.
    void setMemoryPickSlot(int slot);

    /// Pointer to the stored text for a slot, never nullptr (an unset
    /// slot returns the empty string). The buffer is owned by the model
    /// and is the SAME buffer the editor writes into during MEMORY_EDIT,
    /// so committing via TextInput::setValue followed by reading via
    /// getMemory works as expected without copying.
    const char* getMemory(uint8_t slot) const;

    /// Copy `text` into the row at `slot`, truncating if necessary and
    /// always NUL-terminating. Does NOT persist to NVS — that happens
    /// in setup() and on Enter from MEMORY_EDIT.
    void setMemory(uint8_t slot, const char* text);

    /// Convenience: copy the entire bank in/out of the model's rows.
    /// Used by setup() (load) and on Enter (save).
    void copyMemoryBank(const MemoryBank& bank);

    // ─── Private members ──────────────────────────────────────────────────────

private:
    MorseModel() = default;  // singleton: construction only via instance()

    // Display screen
    std::atomic<DisplayScreen> _screen{DisplayScreen::DECODER};
    std::atomic<uint32_t> _overlayStart{0};
    std::atomic<KeyerMode> _mode{KeyerMode::KEYER};
    std::atomic<KeyerType> _keyerType{KeyerType::PADDLE};

    // Decoded text — circular buffer (200 chars)
    // TEXT_BUF_SIZE is declared in the public section above so external
    // readers (e.g. the HTTP /state handler) can iterate the ring.
    char _textBuf[TEXT_BUF_SIZE]{0};

    // Per-character attributes (K=keyer, P=player) — parallel to _textBuf
public:
    static constexpr char ATTR_KEYER  = 'K';
    static constexpr char ATTR_PLAYER = 'P';
private:
    char _textAttr[TEXT_BUF_SIZE]{0};
    std::atomic<size_t> _textHead{0};   // next write position
    std::atomic<size_t> _textLen{0};    // valid content length
    std::atomic<size_t> _textTail{0};   // oldest valid index

    // Parameters
    std::atomic<int> _wpm{20};
    std::atomic<float> _frequency{600.0f};
    std::atomic<int> _volume{50};

    // Radio keying output (default OFF for safety — must be user-enabled)
    std::atomic<bool> _radioKeyingEnabled{false};

    // Iambic paddle polarity (default false = Normal orientation)
    std::atomic<bool> _polarityReversed{false};

    // WinKey/Console mode indicator (default true = WinKey mode so the
    // device starts speaking the WK2 protocol to a host logger on cold
    // boot; operator toggles to Console/debug with the 'D' key).
    std::atomic<bool> _winkeyMode{true};

    // Morse table mode: 0=International, 1=Wabun Katakana, 2=Wabun Hiragana
    std::atomic<MorseTableMode> _morseTableMode{MorseTableMode::INTERNATIONAL};

    // Keyer pattern percentage — written by audio thread
    std::atomic<int> _keyerPct{0};

    // Screen-saver state
    std::atomic<bool> _displayActive{true};
    std::atomic<uint32_t> _lastActivity{0};

    // Encoder character (written by audio thread during playback)
    std::atomic<char> _encoderChar{0};

    // Player character origin flag — set true when last appended char came from player
    std::atomic<bool> _lastCharFromPlayer{false};

    // Index in circular buffer where player chars begin (for color rendering)
    std::atomic<size_t> _playerTail{0};

    // Index of the first player character in the circular buffer (SIZE_MAX if none)
    std::atomic<size_t> _playerHead{SIZE_MAX};

    // ─── Wi-Fi UI mirrors ────────────────────────────────────────────────────
    std::atomic<int>     _wifiState{0};
    std::atomic<uint32_t> _wifiLocalIP{0};
    std::atomic<bool>    _wifiHasCredentials{false};
    std::atomic<int>     _wifiCredSource{0};
    std::atomic<int>     _wifiMode{0};
    std::atomic<int>     _wifiApStations{0};
    std::atomic<int>     _wifiApMaxStations{4};
    std::atomic<bool>    _wifiNetConfirmForget{false};
    std::atomic<uint32_t> _wifiSecondsUntilRetry{0};
    std::atomic<int>     _wifiScanCursor{0};
    std::atomic<int>     _wifiScanTop{0};
    std::atomic<int>     _wifiScanCount{0};

    char       _passwordBuf[kPassBufLen] = {0};
    TextInput* _passwordInput = nullptr;   ///< lazy; constructed in passwordInput()

    // ─── Memory-keyer storage ────────────────────────────────────────────────
    char       _memory[kMemSlots][kMemLen] = {{0}};
    // Editor buffer is independent of _memory[slot] — the slot is only
    // updated when the operator presses Enter (commit) inside MEMORY_EDIT.
    // Pressing ESC discards whatever is in the editor, leaving the
    // persisted text untouched. Mirrors the password buffer pattern:
    // edits live in their own storage and are written to the model on
    // commit, so ESC is a true cancel.
    char       _memoryEditorBuf[kMemLen] = {0};
    TextInput* _memoryInput   = nullptr;   ///< lazy; constructed in memoryInput(), bound to _memoryEditorBuf
    std::atomic<int> _memoryEditingSlot{-1};
    std::atomic<int> _memoryPickSlot{-1};

    // Increment on ANY state change — display task re-renders when this changes
    std::atomic<uint32_t> _changeCounter{0};
};