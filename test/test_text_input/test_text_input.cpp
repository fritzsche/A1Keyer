#include "test_framework.h"
#include "text_input.h"

#include <cstring>

// ─── helpers ────────────────────────────────────────────────────────────────

/// A key state with a single printable character held.
static CardputerKeyState chr(char c, bool shift = false) {
    CardputerKeyState ks;
    ks.anyKey    = true;
    ks.shift     = shift;
    ks.printable = c;
    return ks;
}

/// Nothing held — needed between keystrokes so the editor sees a release.
static CardputerKeyState none() {
    return CardputerKeyState{};
}

/// Type a whole string, releasing between characters.
static void type(TextInput& ti, const char* s) {
    for (const char* p = s; *p; ++p) {
        ti.feed(chr(*p));
        ti.feed(none());
    }
}

// ─── construction ───────────────────────────────────────────────────────────

static void test_starts_empty() {
    char buf[16] = "garbage";
    TextInput ti(buf, sizeof(buf));
    CHECK_STR_EQ("", ti.value());
    CHECK_EQ(0u, ti.length());
    CHECK_EQ(0u, ti.cursorPos());
    CHECK(!ti.reveal());
    CHECK_EQ('*', ti.maskChar());
}

static void test_custom_mask_char() {
    char buf[16];
    TextInput ti(buf, sizeof(buf), '#');
    CHECK_EQ('#', ti.maskChar());
}

// ─── insert / backspace ─────────────────────────────────────────────────────

static void test_insert_appends_at_end() {
    // Cursor starts at 0; inserting moves it forward, so the natural
    // append behaviour is preserved for the empty-buffer case.
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.insert('a');
    ti.insert('b');
    ti.insert('c');
    CHECK_STR_EQ("abc", ti.value());
    CHECK_EQ(3u, ti.length());
    CHECK_EQ(3u, ti.cursorPos());
}

static void test_insert_rejects_non_printable() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.insert('a');
    ti.insert('\n');    // 0x0A
    ti.insert('\t');    // 0x09
    ti.insert(0x7F);    // DEL
    ti.insert('b');
    CHECK_STR_EQ("ab", ti.value());
}

static void test_insert_clamps_at_capacity() {
    char buf[4];        // usable = 3 chars
    TextInput ti(buf, sizeof(buf));
    type(ti, "abcdef");
    CHECK_STR_EQ("abc", ti.value());
    CHECK_EQ(3u, ti.length());
    // The buffer must still be terminated inside its bounds.
    CHECK_EQ('\0', buf[3]);
}

static void test_insert_at_middle_breaks_buffer() {
    // Core cursor test: inserting in the middle must shift the tail
    // right and leave the cursor sitting between the two halves.
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.setValue("ac");          // cursor at end (length 2)
    ti.moveCursor(-1);          // cursor now between 'a' and 'c'
    ti.insert('b');
    CHECK_STR_EQ("abc", ti.value());
    CHECK_EQ(2u, ti.cursorPos());
}

static void test_backspace_removes_before_cursor() {
    // Default cursor-at-end behaviour: backspace deletes the last char.
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "abc");
    ti.backspace();
    CHECK_STR_EQ("ab", ti.value());
    CHECK_EQ(2u, ti.cursorPos());
}

static void test_backspace_at_start_is_noop() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "abc");
    ti.cursorToStart();
    CHECK_EQ(0u, ti.cursorPos());
    ti.backspace();
    CHECK_STR_EQ("abc", ti.value());
    CHECK_EQ(0u, ti.cursorPos());
}

static void test_backspace_in_middle_splits() {
    // Cursor=2 sits between 'b' and 'c' in "abc"; backspace removes
    // the 'b', leaving "ac" and the cursor at 1 (between 'a' and 'c').
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.setValue("abc");         // cursor at 3 (end)
    ti.moveCursor(-1);          // cursor at 2 (between 'b' and 'c')
    ti.backspace();
    CHECK_STR_EQ("ac", ti.value());
    CHECK_EQ(1u, ti.cursorPos());
}

static void test_backspace_on_empty_is_noop() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.backspace();
    ti.backspace();
    CHECK_STR_EQ("", ti.value());
    CHECK_EQ(0u, ti.length());
}

static void test_delete_forward_removes_at_cursor() {
    // Forward-delete (DEL) erases the char AT the cursor; cursor does
    // not move. Distinct from backspace which moves the cursor left.
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.setValue("abc");
    ti.cursorToStart();
    ti.deleteForward();
    CHECK_STR_EQ("bc", ti.value());
    CHECK_EQ(0u, ti.cursorPos());
}

static void test_delete_forward_at_end_is_noop() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "abc");
    ti.deleteForward();
    CHECK_STR_EQ("abc", ti.value());
}

// ─── cursor movement ────────────────────────────────────────────────────────

static void test_move_cursor_clamps_left() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "abc");
    // Cursor is at 3; -10 must clamp to 0.
    ti.moveCursor(-10);
    CHECK_EQ(0u, ti.cursorPos());
}

static void test_move_cursor_clamps_right() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "abc");
    // Cursor is at 3; +10 must clamp to 3 (== length).
    ti.moveCursor(+10);
    CHECK_EQ(3u, ti.cursorPos());
}

static void test_cursor_to_start_and_end() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "hello");
    ti.cursorToStart();
    CHECK_EQ(0u, ti.cursorPos());
    ti.cursorToEnd();
    CHECK_EQ(5u, ti.cursorPos());
}

// ─── setValue / clear ───────────────────────────────────────────────────────

static void test_set_value_prefills() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.setValue("hunter2");
    CHECK_STR_EQ("hunter2", ti.value());
    CHECK_EQ(7u, ti.length());
    // Editing continues from the end of the pre-filled value.
    ti.insert('!');
    CHECK_STR_EQ("hunter2!", ti.value());
    CHECK_EQ(8u, ti.cursorPos());
}

static void test_set_value_truncates() {
    char buf[4];
    TextInput ti(buf, sizeof(buf));
    ti.setValue("abcdef");
    CHECK_STR_EQ("abc", ti.value());
}

static void test_set_value_null_clears() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.setValue("abc");
    ti.setValue(nullptr);
    CHECK_STR_EQ("", ti.value());
    CHECK_EQ(0u, ti.length());
    CHECK_EQ(0u, ti.cursorPos());
}

static void test_clear_empties() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "abc");
    ti.clear();
    CHECK_STR_EQ("", ti.value());
    CHECK_EQ(0u, ti.cursorPos());
}

// ─── feed: printable characters ─────────────────────────────────────────────

static void test_feed_types_characters() {
    char buf[32];
    TextInput ti(buf, sizeof(buf));
    type(ti, "s3cret");
    CHECK_STR_EQ("s3cret", ti.value());
}

static void test_feed_returns_changed_for_typing() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    CHECK(ti.feed(chr('a')) == TextInput::Result::CHANGED);
}

static void test_feed_returns_idle_when_idle() {
    // This is the flicker-fix contract: feed() with an unchanged tick
    // must report IDLE, not CHANGED, so the caller can skip the repaint.
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "ab");
    CHECK(ti.feed(none()) == TextInput::Result::IDLE);
    CHECK(ti.feed(none()) == TextInput::Result::IDLE);
}

static void test_feed_returns_idle_when_key_held() {
    // Holding a printable key must not produce repeated CHANGED — only
    // the first edge does. A repaint-only renderer relies on this.
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    CHECK(ti.feed(chr('a')) == TextInput::Result::CHANGED);
    CHECK(ti.feed(chr('a')) == TextInput::Result::IDLE);
    CHECK(ti.feed(chr('a')) == TextInput::Result::IDLE);
}

// A held key must produce exactly one character, not one per tick. This
// is the whole reason feed() does edge detection internally.
static void test_held_key_does_not_repeat() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    for (int i = 0; i < 10; ++i) ti.feed(chr('a'));
    CHECK_STR_EQ("a", ti.value());
}

static void test_release_allows_same_key_again() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.feed(chr('a'));
    ti.feed(chr('a'));   // still held — ignored
    ti.feed(none());     // released
    ti.feed(chr('a'));   // pressed again — accepted
    CHECK_STR_EQ("aa", ti.value());
}

// Rolling from one key straight onto another, without a gap, is normal
// fast typing and must not drop the second character.
static void test_rollover_between_two_keys() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.feed(chr('a'));
    ti.feed(chr('b'));   // no release in between
    CHECK_STR_EQ("ab", ti.value());
}

static void test_space_is_accepted_as_a_character() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    // WPA passphrases may contain spaces.
    ti.feed(chr('a'));
    ti.feed(chr(' '));
    ti.feed(none());
    ti.feed(chr('b'));
    CHECK_STR_EQ("a b", ti.value());
}

// ─── feed: backspace ────────────────────────────────────────────────────────

static void test_feed_backspace() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "abc");
    CardputerKeyState bs;
    bs.anyKey = true;
    bs.backspace = true;
    CHECK(ti.feed(bs) == TextInput::Result::CHANGED);
    CHECK_STR_EQ("ab", ti.value());
}

static void test_held_backspace_does_not_repeat() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "abcd");
    CardputerKeyState bs;
    bs.anyKey = true;
    bs.backspace = true;
    for (int i = 0; i < 10; ++i) ti.feed(bs);
    CHECK_STR_EQ("abc", ti.value());
}

// ─── feed: enter / escape ───────────────────────────────────────────────────

static void test_feed_enter_returns_enter() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "pw");
    CardputerKeyState ks;
    ks.anyKey = true;
    ks.enter  = true;
    CHECK(ti.feed(ks) == TextInput::Result::ENTER);
    // Enter must not mutate the value.
    CHECK_STR_EQ("pw", ti.value());
}

static void test_feed_escape_returns_esc() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "pw");
    CardputerKeyState ks;
    ks.anyKey  = true;
    ks.escape  = true;
    CHECK(ti.feed(ks) == TextInput::Result::ESC);
    CHECK_STR_EQ("pw", ti.value());
}

// Holding Enter must not re-fire; otherwise one keypress would commit and
// then immediately re-trigger on the next screen.
static void test_held_enter_fires_once() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    CardputerKeyState ks;
    ks.anyKey = true;
    ks.enter  = true;
    CHECK(ti.feed(ks) == TextInput::Result::ENTER);
    CHECK(ti.feed(ks) == TextInput::Result::IDLE);
    CHECK(ti.feed(ks) == TextInput::Result::IDLE);
}

// primeEnterHeld: when the user pressed Enter on the previous screen and
// is still holding it as we transition into the editor, the very first
// tick must NOT commit an empty value. Without this guard, the wifi scan
// list's Enter would commit a blank password on the next loop tick.
static void test_prime_enter_held_suppresses_first_tick() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));

    // Simulate the scan-list path: clear() (which resets _prevEnter to
    // false), then primeEnterHeld() to swallow the held key.
    ti.clear();
    ti.primeEnterHeld();

    CardputerKeyState ks;
    ks.anyKey = true;
    ks.enter  = true;       // user is still holding Enter
    CHECK(ti.feed(ks) == TextInput::Result::IDLE);   // not ENTER
    CHECK_STR_EQ("", ti.value());

    // Once Enter is released and pressed again, normal commit semantics
    // are restored — the editor hasn't lost its mind.
    ti.feed(none());
    CHECK(ti.feed(ks) == TextInput::Result::ENTER);
}

static void test_escape_takes_precedence_over_enter() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    CardputerKeyState ks;
    ks.anyKey = true;
    ks.enter  = true;
    ks.escape = true;
    CHECK(ti.feed(ks) == TextInput::Result::ESC);
}

// ─── feed: reveal toggle ────────────────────────────────────────────────────

static void test_shift_space_toggles_reveal() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    CHECK(!ti.reveal());
    CHECK(ti.feed(chr(' ', /*shift=*/true)) == TextInput::Result::CHANGED);
    CHECK(ti.reveal());
    ti.feed(none());
    CHECK(ti.feed(chr(' ', /*shift=*/true)) == TextInput::Result::CHANGED);
    CHECK(!ti.reveal());
}

static void test_shift_space_does_not_insert() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    type(ti, "ab");
    ti.feed(chr(' ', /*shift=*/true));
    CHECK_STR_EQ("ab", ti.value());
}

static void test_plain_space_does_not_toggle_reveal() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.feed(chr(' ', /*shift=*/false));
    CHECK(!ti.reveal());
    CHECK_STR_EQ(" ", ti.value());
}

static void test_set_reveal_directly() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.setReveal(true);
    CHECK(ti.reveal());
    ti.setReveal(false);
    CHECK(!ti.reveal());
}

// ─── realistic sequence ─────────────────────────────────────────────────────

static void test_typical_password_entry() {
    char buf[64];
    TextInput ti(buf, sizeof(buf));

    type(ti, "myWiFiPas");

    // Typo: one character too many, correct it.
    ti.feed(chr('z'));
    ti.feed(none());
    CHECK_STR_EQ("myWiFiPasz", ti.value());

    CardputerKeyState bs;
    bs.anyKey = true;
    bs.backspace = true;
    ti.feed(bs);
    ti.feed(none());
    CHECK_STR_EQ("myWiFiPas", ti.value());

    // Peek at what was typed.
    ti.feed(chr(' ', true));
    ti.feed(none());
    CHECK(ti.reveal());

    type(ti, "s");
    CHECK_STR_EQ("myWiFiPass", ti.value());

    CardputerKeyState en;
    en.anyKey = true;
    en.enter  = true;
    CHECK(ti.feed(en) == TextInput::Result::ENTER);
    CHECK_STR_EQ("myWiFiPass", ti.value());
}

// A 63-character passphrase is the WPA maximum and must fit exactly.
static void test_max_length_wpa_passphrase() {
    char buf[64];       // 63 usable + terminator
    TextInput ti(buf, sizeof(buf));
    for (int i = 0; i < 70; ++i) {
        ti.feed(chr('a' + (i % 26)));
        ti.feed(none());
    }
    CHECK_EQ(63u, ti.length());
    CHECK_EQ('\0', buf[63]);
}

// Mid-buffer edit: insert "X" between "ab" and "cd" using cursor moves.
static void test_mid_buffer_edit() {
    char buf[16];
    TextInput ti(buf, sizeof(buf));
    ti.setValue("abcd");     // cursor at 4
    ti.moveCursor(-2);       // cursor at 2, between 'b' and 'c'
    ti.insert('X');
    CHECK_STR_EQ("abXcd", ti.value());
    CHECK_EQ(3u, ti.cursorPos());
    // Backspace at this position removes the 'X' and leaves the cursor
    // back between 'b' and 'c'.
    ti.backspace();
    CHECK_STR_EQ("abcd", ti.value());
    CHECK_EQ(2u, ti.cursorPos());
}

int main() {
    RUN(test_starts_empty);
    RUN(test_custom_mask_char);

    RUN(test_insert_appends_at_end);
    RUN(test_insert_rejects_non_printable);
    RUN(test_insert_clamps_at_capacity);
    RUN(test_insert_at_middle_breaks_buffer);
    RUN(test_backspace_removes_before_cursor);
    RUN(test_backspace_at_start_is_noop);
    RUN(test_backspace_in_middle_splits);
    RUN(test_backspace_on_empty_is_noop);
    RUN(test_delete_forward_removes_at_cursor);
    RUN(test_delete_forward_at_end_is_noop);

    RUN(test_move_cursor_clamps_left);
    RUN(test_move_cursor_clamps_right);
    RUN(test_cursor_to_start_and_end);

    RUN(test_set_value_prefills);
    RUN(test_set_value_truncates);
    RUN(test_set_value_null_clears);
    RUN(test_clear_empties);

    RUN(test_feed_types_characters);
    RUN(test_feed_returns_changed_for_typing);
    RUN(test_feed_returns_idle_when_idle);
    RUN(test_feed_returns_idle_when_key_held);
    RUN(test_held_key_does_not_repeat);
    RUN(test_release_allows_same_key_again);
    RUN(test_rollover_between_two_keys);
    RUN(test_space_is_accepted_as_a_character);

    RUN(test_feed_backspace);
    RUN(test_held_backspace_does_not_repeat);

    RUN(test_feed_enter_returns_enter);
    RUN(test_feed_escape_returns_esc);
    RUN(test_held_enter_fires_once);
    RUN(test_prime_enter_held_suppresses_first_tick);
    RUN(test_escape_takes_precedence_over_enter);

    RUN(test_shift_space_toggles_reveal);
    RUN(test_shift_space_does_not_insert);
    RUN(test_plain_space_does_not_toggle_reveal);
    RUN(test_set_reveal_directly);

    RUN(test_typical_password_entry);
    RUN(test_max_length_wpa_passphrase);
    RUN(test_mid_buffer_edit);
    return test_summary();
}
