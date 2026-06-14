#include "test_framework.h"
#include "display_model.h"

static void test_screen_default_is_decoder() {
    auto& m = MorseModel::instance();
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::DECODER);
}

static void test_screen_transitions() {
    auto& m = MorseModel::instance();
    m.setScreen(DisplayScreen::WPM_VIEW);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::WPM_VIEW);
    m.setScreen(DisplayScreen::WPM_SETTINGS);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::WPM_SETTINGS);
    m.setScreen(DisplayScreen::FREQ_SETTINGS);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::FREQ_SETTINGS);
    m.setScreen(DisplayScreen::VOLUME_SETTINGS);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::VOLUME_SETTINGS);
    m.setScreen(DisplayScreen::DECODER);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::DECODER);
}

static void test_wpm_clamp() {
    auto& m = MorseModel::instance();
    m.setWPM(20);
    CHECK_EQ(m.wpm(), 20);
    m.setWPM(100);
    CHECK_EQ(m.wpm(), 50);
    m.setWPM(1);
    CHECK_EQ(m.wpm(), 5);
}

static void test_wpm_adjust() {
    auto& m = MorseModel::instance();
    m.setWPM(20);
    m.adjustWPM(+5);
    CHECK_EQ(m.wpm(), 25);
    m.adjustWPM(-10);
    CHECK_EQ(m.wpm(), 15);
}

static void test_frequency_clamp() {
    auto& m = MorseModel::instance();
    m.setFrequency(600.0f);
    CHECK_NEAR(m.frequency(), 600.0f, 0.1f);
    m.setFrequency(2000.0f);
    CHECK_EQ((int)m.frequency(), 900);
    m.setFrequency(100.0f);
    CHECK_EQ((int)m.frequency(), 300);
}

static void test_frequency_adjust() {
    auto& m = MorseModel::instance();
    m.setFrequency(500.0f);
    m.adjustFrequency(+50.0f);
    CHECK_EQ((int)m.frequency(), 550);
    m.adjustFrequency(-100.0f);
    CHECK_EQ((int)m.frequency(), 450);
}

static void test_volume_clamp() {
    auto& m = MorseModel::instance();
    m.setVolume(50);
    CHECK_EQ(m.volume(), 50);
    m.setVolume(200);
    CHECK_EQ(m.volume(), 100);
    m.setVolume(-10);
    CHECK_EQ(m.volume(), 0);
}

static void test_volume_adjust() {
    auto& m = MorseModel::instance();
    m.setVolume(50);
    m.adjustVolume(+10);
    CHECK_EQ(m.volume(), 60);
    m.adjustVolume(-20);
    CHECK_EQ(m.volume(), 40);
}

static void test_change_counter_increments() {
    auto& m = MorseModel::instance();
    uint32_t c0 = m.changeCounter();
    m.setScreen(DisplayScreen::WPM_VIEW);
    CHECK(m.changeCounter() > c0);
}

static void test_mode_switch() {
    auto& m = MorseModel::instance();
    m.setMode(KeyerMode::ENCODER);
    CHECK_EQ((int)m.mode(), (int)KeyerMode::ENCODER);
    m.setMode(KeyerMode::KEYER);
    CHECK_EQ((int)m.mode(), (int)KeyerMode::KEYER);
}

static void test_decoded_text_append_and_read() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();
    m.appendDecodedChar('A');
    m.appendDecodedChar('B');
    CHECK_EQ(m.decodedTextLen(), (size_t)2);
    CHECK_STR_EQ(m.decodedText(), "AB");
}

static void test_decoded_text_overflow_capped() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();
    for (int i = 0; i < 250; ++i) {
        m.appendDecodedChar('X');
    }
    // Buffer is capped; length must not grow past the internal capacity
    CHECK(m.decodedTextLen() <= 200);
}

static void test_pattern_percent() {
    auto& m = MorseModel::instance();
    m.setKeyerPatternPercent(42);
    CHECK_EQ(m.keyerPatternPercent(), 42);
}

static void test_overlay_timeout() {
    auto& m = MorseModel::instance();
    m.setOverlayStartMillis(1000);
    CHECK_EQ(m.overlayStartMillis(), 1000u);
}

static void test_display_active_default_true() {
    auto& m = MorseModel::instance();
    CHECK(m.isDisplayActive());
}

static void test_display_active_set_false() {
    auto& m = MorseModel::instance();
    m.setDisplayActive(false);
    CHECK(!m.isDisplayActive());
    CHECK(m.changeCounter() > 0);  // toggle should increment counter
}

static void test_display_active_toggle() {
    auto& m = MorseModel::instance();
    m.setDisplayActive(true);
    CHECK(m.isDisplayActive());
    m.setDisplayActive(false);
    CHECK(!m.isDisplayActive());
}

static void test_touch_updates_activity() {
    auto& m = MorseModel::instance();
    m.touch();
    CHECK(m.lastActivity() > 0);
}

static void test_encoder_char_default_zero() {
    auto& m = MorseModel::instance();
    CHECK_EQ((int)m.encoderChar(), 0);
}

static void test_encoder_char_set_and_get() {
    auto& m = MorseModel::instance();
    m.setEncoderChar('A');
    CHECK_EQ((int)m.encoderChar(), 'A');
}

// =============================================================================
// Player color tracking tests
// =============================================================================

static void test_player_head_set_on_first_player_char() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();

    // First player char → _playerHead should be set
    m.appendDecodedChar('A', true);
    CHECK(m.playerHead() != SIZE_MAX);
    CHECK_EQ(m.playerHead(), (size_t)0);

    // Second player char → _playerHead should NOT move
    size_t firstHead = m.playerHead();
    m.appendDecodedChar('B', true);
    CHECK_EQ(m.playerHead(), firstHead);  // unchanged
}

static void test_player_head_reset_on_keyer_char() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();

    // Append some player chars
    m.appendDecodedChar('A', true);
    m.appendDecodedChar('B', true);
    CHECK(m.playerHead() != SIZE_MAX);

    // Keyer char resets _playerHead to SIZE_MAX
    m.appendDecodedChar('X', false);
    CHECK_EQ(m.playerHead(), (size_t)SIZE_MAX);
}

static void test_reset_player_head_method() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();

    m.appendDecodedChar('A', true);
    m.appendDecodedChar('B', true);
    CHECK(m.playerHead() != SIZE_MAX);

    m.resetPlayerHead();
    CHECK_EQ(m.playerHead(), (size_t)SIZE_MAX);
}

static void test_clear_resets_player_head() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();

    m.appendDecodedChar('A', true);
    CHECK(m.playerHead() != SIZE_MAX);

    m.clearDecodedText();
    CHECK_EQ(m.playerHead(), (size_t)SIZE_MAX);
}

static void test_player_tail_moves_to_last() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();

    m.appendDecodedChar('A', true);
    CHECK_EQ(m.playerTail(), (size_t)0);

    m.appendDecodedChar('B', true);
    CHECK_EQ(m.playerTail(), (size_t)1);

    m.appendDecodedChar('C', true);
    CHECK_EQ(m.playerTail(), (size_t)2);
}

static void test_mixed_player_then_keyer() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();

    // Player section: "AB"
    m.appendDecodedChar('A', true);   // playerHead=0, playerTail=0
    m.appendDecodedChar('B', true);   // playerHead stays 0, playerTail=1

    // Keyer char 'X' arrives — resets playerHead to SIZE_MAX
    m.appendDecodedChar('X', false);   // playerHead reset, playerTail=2 (X's position)

    // playerHead is reset on keyer input (color split goes all-white)
    CHECK_EQ(m.playerHead(), (size_t)SIZE_MAX);
    // playerTail still points to last player char (B at index 1) — not updated by keyer
    CHECK_EQ(m.playerTail(), (size_t)1);
}

static void test_player_section_fully_scrolled_off() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();

    // Player "AB", then keyer "X", then scroll
    m.appendDecodedChar('A', true);
    m.appendDecodedChar('B', true);
    m.appendDecodedChar('X', false);  // playerHead reset to SIZE_MAX
    // Buffer: [A][B][X], textHead=3, textTail=0, textLen=3

    // playerHead=SIZE_MAX means all-white (no player section)
    CHECK_EQ(m.playerHead(), (size_t)SIZE_MAX);

    // playerTail points to X (index 2)
    CHECK_EQ(m.playerTail(), (size_t)2);
}

static void test_overflow_evicts_oldest_player_head() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();

    // Fill 200 chars to wrap buffer
    for (int i = 0; i < 200; ++i) {
        m.appendDecodedChar('A', true);  // all player
    }
    // At this point: playerHead=0, playerTail=199, head=0 (wrapped)

    // Add one more — oldest char 'A' at index 0 gets evicted
    m.appendDecodedChar('B', true);  // playerHead moves to 1, playerTail=200%200=0

    // playerHead should have advanced past index 0 (oldest was dropped)
    CHECK(m.playerHead() >= (size_t)1);
}

static void test_last_char_from_player_flag() {
    auto& m = MorseModel::instance();
    m.clearDecodedText();

    CHECK(!m.lastCharFromPlayer());

    m.appendDecodedChar('A', true);
    CHECK(m.lastCharFromPlayer());

    m.appendDecodedChar('X', false);
    CHECK(!m.lastCharFromPlayer());
}

int main() {
    printf("=== display_model ===\n");
    RUN(test_screen_default_is_decoder);
    RUN(test_screen_transitions);
    RUN(test_wpm_clamp);
    RUN(test_wpm_adjust);
    RUN(test_frequency_clamp);
    RUN(test_frequency_adjust);
    RUN(test_volume_clamp);
    RUN(test_volume_adjust);
    RUN(test_change_counter_increments);
    RUN(test_mode_switch);
    RUN(test_decoded_text_append_and_read);
    RUN(test_decoded_text_overflow_capped);
    RUN(test_pattern_percent);
    RUN(test_overlay_timeout);
    RUN(test_display_active_default_true);
    RUN(test_display_active_set_false);
    RUN(test_display_active_toggle);
    RUN(test_touch_updates_activity);
    RUN(test_encoder_char_default_zero);
    RUN(test_encoder_char_set_and_get);
    RUN(test_player_head_set_on_first_player_char);
    RUN(test_player_head_reset_on_keyer_char);
    RUN(test_reset_player_head_method);
    RUN(test_clear_resets_player_head);
    RUN(test_player_tail_moves_to_last);
    RUN(test_mixed_player_then_keyer);
    RUN(test_last_char_from_player_flag);
    return test_summary();
}