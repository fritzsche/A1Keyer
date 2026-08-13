#pragma once
/**
 * text_input.h — Reusable single-line text editor for keyboard-driven entry.
 *
 * A1Keyer's settings screens all use two-key incremental editing (";" and
 * "." to step a number). Entering a Wi-Fi passphrase needs real text
 * entry, and this is the first such field, so the editor is factored out
 * rather than inlined into the network screens.
 *
 * The editor is deliberately passive: it owns the edit buffer and nothing
 * else. It never touches the display, never reads the keyboard, and never
 * changes the active screen. The caller samples the keyboard once per
 * tick into a CardputerKeyState, hands it to feed(), and acts on the
 * returned Result. That split is what makes the whole thing testable on
 * the host — see test/test_text_input.
 *
 * Storage is caller-owned so the buffer can live in the display model
 * alongside the other persisted-ish state, rather than being heap
 * allocated on a device with no PSRAM.
 *
 * Portable C++17: no Arduino, no ESP-IDF, no dynamic allocation.
 */
#include <cstddef>

/**
 * One tick's worth of keyboard state, sampled by the caller.
 *
 * `printable` is the first printable ASCII character currently held, or
 * 0 if none. Modifier flags describe the same instant.
 */
struct CardputerKeyState {
    bool anyKey    = false;  ///< any key is currently down (used for wake)
    bool shift     = false;
    bool fn        = false;
    bool opt       = false;  ///< Cardputer OPT key (bottom row, col 1). Used for caps-lock toggle on the password screen.
    bool enter     = false;
    bool backspace = false;
    bool escape    = false;
    char printable = 0;      ///< first printable ASCII held, else 0
};

class TextInput {
public:
    /// What the caller should do after feed().
    ///
    /// IDLE means no state changed on this tick — caller should NOT
    /// repaint. CHANGED means the buffer or display state (e.g. reveal
    /// toggle) changed and needs a repaint. ENTER / ESC are explicit
    /// transitions that the caller must handle.
    enum class Result {
        IDLE,     ///< nothing changed this tick
        CHANGED,  ///< buffer or reveal state changed
        ENTER,    ///< user committed the value
        ESC,      ///< user abandoned the field
    };

    /**
     * Construct an editor over caller-owned storage.
     *
     * @param buf       Storage, must outlive the editor. Set to "" here.
     * @param cap       Buffer capacity INCLUDING the terminator, so the
     *                  longest accepted string is cap-1 characters.
     * @param maskChar  Glyph the renderer should substitute for each
     *                  character while reveal() is false.
     */
    TextInput(char* buf, size_t cap, char maskChar = '*');

    /// Replace the contents, e.g. to pre-fill a previously saved value.
    /// Truncates at capacity. Cursor lands at the end. Also clears key-
    /// repeat state.
    void setValue(const char* s);

    /// Empty the buffer and clear key-repeat state. Cursor lands at 0.
    void clear();

    /**
     * Tell the editor that Enter is already "in flight" — i.e. the key
     * that opened this screen is still being held when the editor first
     * sees it. The next feed() tick will not treat the held Enter as a
     * fresh commit, even though _prevEnter was just reset to false by
     * clear() or by first construction.
     */
    void primeEnterHeld();

    // ─── Buffer access ──────────────────────────────────────────────────

    const char* value()  const { return _buf; }
    size_t      length() const { return _len; }

    /// Position of the insertion cursor, 0..length(). Characters are
    /// inserted before this position and backspace deletes the character
    /// immediately before it.
    size_t      cursorPos() const { return _cursor; }

    // ─── Editing ────────────────────────────────────────────────────────

    /// Insert one character at the cursor position. Cursor advances.
    /// Silently ignored when the buffer is full or when c is not
    /// printable ASCII.
    void insert(char c);

    /// Delete the character before the cursor. No-op at position 0.
    void backspace();

    /// Delete the character at the cursor. No-op at end of buffer.
    void deleteForward();

    /// Move the cursor by `delta` (negative = left, positive = right).
    /// Clamped to [0, length()].
    void moveCursor(int delta);

    void cursorToStart() { _cursor = 0; }
    void cursorToEnd()   { _cursor = _len; }

    // ─── Reveal / masking ───────────────────────────────────────────────

    /// True when the renderer should show the text as-is rather than
    /// substituting maskChar().
    bool reveal() const { return _reveal; }
    void setReveal(bool on) { _reveal = on; }

    char maskChar() const { return _maskChar; }

    /**
     * Advance the editor by one keyboard tick.
     *
     * Only key *transitions* act, so holding a key does not repeat: the
     * editor remembers the previous tick's state internally. Feed every
     * tick while the field has focus, including ticks where no key is
     * down — that is how a release is observed.
     *
     * Precedence within one tick: Esc, then Enter, then Backspace, then
     * the printable character.
     *
     * Returns IDLE when nothing changed (caller should not repaint),
     * CHANGED when the buffer or reveal flag moved, ENTER/ESC for
     * explicit transitions.
     *
     * Shift+Space toggles reveal(). Plain Space still inserts a space,
     * since a WPA passphrase may legitimately contain one.
     */
    Result feed(const CardputerKeyState& ks);

private:
    char*  _buf;
    size_t _cap;
    size_t _len    = 0;
    size_t _cursor = 0;
    bool   _reveal = false;
    char   _maskChar;

    // Previous tick, for edge detection.
    bool _prevEnter     = false;
    bool _prevBackspace = false;
    bool _prevEscape    = false;
    bool _prevFn        = false;
    char _prevPrintable = 0;
};
