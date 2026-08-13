#include "text_input.h"

namespace {
/// Printable ASCII, i.e. what a passphrase or SSID may contain.
inline bool isPrintable(char c) {
    return c >= 0x20 && c < 0x7F;
}
}  // namespace

TextInput::TextInput(char* buf, size_t cap, char maskChar)
    : _buf(buf), _cap(cap), _maskChar(maskChar) {
    if (_buf && _cap > 0) _buf[0] = '\0';
}

void TextInput::setValue(const char* s) {
    _len = 0;
    if (_buf && _cap > 0) _buf[0] = '\0';
    if (s) {
        while (*s && _len + 1 < _cap) {
            _buf[_len++] = *s++;
        }
        _buf[_len] = '\0';
    }
    _cursor = _len;   // editing resumes at the end of the new value
    _prevEnter = _prevBackspace = _prevEscape = false;
    _prevPrintable = 0;
}

void TextInput::clear() {
    setValue(nullptr);
    _cursor = 0;      // explicit: an empty buffer has cursor at 0
}

void TextInput::primeEnterHeld() {
    // Pretend the previous tick already saw Enter held. The next feed()
    // will see enterEdge = ks.enter && !_prevEnter = true && !true = false.
    _prevEnter = true;
}

void TextInput::insert(char c) {
    if (!isPrintable(c)) return;
    // Buffer is "full" when one more char plus the terminator won't fit.
    if (_len + 1 >= _cap) return;
    // Shift the tail right by one, then drop the new char in the gap
    // and walk the cursor over it. The shift runs from the back so the
    // tail-end index never collides with what we're writing.
    for (size_t i = _len; i > _cursor; --i) {
        _buf[i] = _buf[i - 1];
    }
    _buf[_cursor] = c;
    ++_len;
    _buf[_len] = '\0';
    ++_cursor;
}

void TextInput::backspace() {
    if (_cursor == 0) return;
    // Pull the cursor back, then drag everything to its right down one
    // slot. _len shrinks so the unused slot at the end is "garbage" but
    // the next insert / setValue will overwrite it, and the terminator
    // is rewritten here to keep the buffer always valid.
    --_cursor;
    for (size_t i = _cursor; i + 1 < _len; ++i) {
        _buf[i] = _buf[i + 1];
    }
    --_len;
    _buf[_len] = '\0';
}

void TextInput::deleteForward() {
    if (_cursor >= _len) return;
    for (size_t i = _cursor; i + 1 < _len; ++i) {
        _buf[i] = _buf[i + 1];
    }
    --_len;
    _buf[_len] = '\0';
}

void TextInput::moveCursor(int delta) {
    if (delta == 0) return;
    if (delta < 0) {
        const size_t step = (size_t)(-delta);
        _cursor = (step > _cursor) ? 0 : (_cursor - step);
    } else {
        const size_t step = (size_t)delta;
        const size_t room = _len - _cursor;
        _cursor = (step > room) ? _len : (_cursor + step);
    }
}

TextInput::Result TextInput::feed(const CardputerKeyState& ks) {
    // Edge detection against the previous tick — a held key must not
    // repeat. Compute all edges first, then update the saved state, so
    // every early return still records this tick.
    const bool escEdge   = ks.escape    && !_prevEscape;
    const bool enterEdge = ks.enter     && !_prevEnter;
    const bool bsEdge    = ks.backspace && !_prevBackspace;
    const bool fnEdge    = ks.fn        && !_prevFn;
    const bool charEdge  = ks.printable != 0 && ks.printable != _prevPrintable;

    _prevEscape    = ks.escape;
    _prevEnter     = ks.enter;
    _prevBackspace = ks.backspace;
    _prevFn        = ks.fn;
    _prevPrintable = ks.printable;

    if (escEdge)   return Result::ESC;
    if (enterEdge) return Result::ENTER;

    if (bsEdge) {
        backspace();
        return Result::CHANGED;
    }

    if (fnEdge) {
        // FN (bottom-left key on the Cardputer) is the discoverable
        // single-key reveal toggle. SHIFT+SPACE remains supported as
        // an alternate gesture but is shown nowhere in the password
        // screen hint, so users rarely find it.
        _reveal = !_reveal;
        return Result::CHANGED;
    }

    if (charEdge) {
        // Shift+Space is the legacy reveal toggle. Plain Space still
        // types a space, because a WPA passphrase may contain one.
        if (ks.printable == ' ' && ks.shift) {
            _reveal = !_reveal;
            return Result::CHANGED;
        }
        insert(ks.printable);
        return Result::CHANGED;
    }

    // No key edge on this tick — buffer and reveal state are unchanged,
    // so the caller should NOT trigger a repaint.
    return Result::IDLE;
}
