#include "test_framework.h"
#include "display_model.h"
#include "text_input.h"

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

// --- radio keying (GPIO4 mirror) ---

static void test_radio_keying_default_off() {
    auto& m = MorseModel::instance();
    m.setRadioKeyingEnabled(false);
    CHECK(!m.radioKeyingEnabled());
}

static void test_radio_keying_set_on() {
    auto& m = MorseModel::instance();
    m.setRadioKeyingEnabled(true);
    CHECK(m.radioKeyingEnabled());
}

static void test_radio_keying_toggle_increments_change_counter() {
    auto& m = MorseModel::instance();
    m.setRadioKeyingEnabled(false);  // baseline
    uint32_t before = m.changeCounter();
    m.setRadioKeyingEnabled(true);
    CHECK(m.changeCounter() > before);
    uint32_t after = m.changeCounter();
    m.setRadioKeyingEnabled(false);
    CHECK(m.changeCounter() > after);
}

static void test_keying_settings_screen_round_trip() {
    auto& m = MorseModel::instance();
    m.setScreen(DisplayScreen::KEYING_SETTINGS);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::KEYING_SETTINGS);
    // Returning to the decoder screen must work.
    m.setScreen(DisplayScreen::DECODER);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::DECODER);
}

static void test_radio_keying_persists_across_screen_changes() {
    auto& m = MorseModel::instance();
    m.setRadioKeyingEnabled(true);
    m.setScreen(DisplayScreen::WPM_SETTINGS);
    m.setScreen(DisplayScreen::KEYING_SETTINGS);
    m.setScreen(DisplayScreen::DECODER);
    // The setting should be unchanged by screen navigation.
    CHECK(m.radioKeyingEnabled());
}

static void test_radio_keying_teardown_resets_to_off() {
    // Singleton test pollution guard: leave the model in a known state
    // so the next test (or the host process) doesn't inherit keying=on.
    auto& m = MorseModel::instance();
    m.setRadioKeyingEnabled(false);
    CHECK(!m.radioKeyingEnabled());
}

// =============================================================================
// Wi-Fi UI mirror tests
// =============================================================================

static void test_wifi_state_default_idle() {
    auto& m = MorseModel::instance();
    m.setWifiState(0);
    CHECK_EQ(m.wifiState(), 0);
}

static void test_wifi_state_set_and_get() {
    auto& m = MorseModel::instance();
    m.setWifiState(7);   // NetState::CONNECTED
    CHECK_EQ(m.wifiState(), 7);
}

static void test_wifi_state_same_value_is_noop() {
    auto& m = MorseModel::instance();
    m.setWifiState(3);
    uint32_t before = m.changeCounter();
    m.setWifiState(3);
    CHECK_EQ(m.changeCounter(), before);
}

static void test_wifi_ip_default_zero() {
    auto& m = MorseModel::instance();
    m.setWifiLocalIP(0);
    CHECK_EQ(m.wifiLocalIP(), 0u);
}

static void test_wifi_ip_set_and_get() {
    auto& m = MorseModel::instance();
    m.setWifiLocalIP(0xC0A80164);
    CHECK_EQ(m.wifiLocalIP(), 0xC0A80164u);
}

static void test_wifi_has_credentials_round_trip() {
    auto& m = MorseModel::instance();
    m.setWifiHasCredentials(false);
    CHECK(!m.wifiHasCredentials());
    m.setWifiHasCredentials(true);
    CHECK(m.wifiHasCredentials());
}

static void test_wifi_cred_source_default_none() {
    auto& m = MorseModel::instance();
    m.setWifiCredSource(0);
    CHECK_EQ(m.wifiCredSource(), 0);
}

static void test_wifi_cred_source_set_and_get() {
    auto& m = MorseModel::instance();
    m.setWifiCredSource(1);   // NetCredSource::NVS
    CHECK_EQ(m.wifiCredSource(), 1);
}

static void test_wifi_seconds_until_retry_set_and_get() {
    auto& m = MorseModel::instance();
    m.setWifiSecondsUntilRetry(42);
    CHECK_EQ(m.wifiSecondsUntilRetry(), 42u);
}

static void test_wifi_scan_count_clamps_negative() {
    auto& m = MorseModel::instance();
    m.setWifiScanCount(-3);
    CHECK_EQ(m.wifiScanCount(), 0);
}

static void test_wifi_scan_cursor_clamps_within_count() {
    auto& m = MorseModel::instance();
    m.setWifiScanCount(3);
    m.wifiAdjustScanCursor(+10);
    CHECK_EQ(m.wifiScanCursor(), 2);     // clamped to last
    m.wifiAdjustScanCursor(-10);
    CHECK_EQ(m.wifiScanCursor(), 0);     // clamped to first
}

static void test_wifi_scan_cursor_keeps_window_visible() {
    auto& m = MorseModel::instance();
    m.setWifiScanCount(20);
    m.wifiAdjustScanCursor(+8);
    // cursor should be 8; top should follow so cursor is visible.
    // Tied to kPageSize == 4 in network_manager.h: with delta=+8 the
    // windowing math lands top at 8 - (4 - 1) = 5.
    CHECK_EQ(m.wifiScanCursor(), 8);
    CHECK_EQ(m.wifiScanTop(), 5);

    m.wifiAdjustScanCursor(-8);
    CHECK_EQ(m.wifiScanCursor(), 0);
    CHECK_EQ(m.wifiScanTop(), 0);
}

static void test_wifi_scan_empty_list_resets_to_zero() {
    auto& m = MorseModel::instance();
    m.setWifiScanCount(0);
    m.setWifiScanCursor(0);
    m.setWifiScanTop(0);
    m.wifiAdjustScanCursor(+3);
    CHECK_EQ(m.wifiScanCursor(), 0);
    CHECK_EQ(m.wifiScanTop(), 0);
}

static void test_password_input_lazily_constructed() {
    auto& m = MorseModel::instance();
    TextInput* ti = m.passwordInput();
    CHECK_NOT_NULL(ti);
    CHECK_EQ((size_t)0, ti->length());

    // Subsequent calls return the same instance.
    CHECK_EQ((void*)ti, (void*)m.passwordInput());
}

static void test_password_input_buffer_is_callers_storage() {
    auto& m = MorseModel::instance();
    TextInput* ti = m.passwordInput();
    ti->clear();
    ti->insert('A');
    ti->insert('B');
    CHECK_EQ((size_t)2, ti->length());
    CHECK_STR_EQ("AB", ti->value());
}

static void test_wifi_clear_password_empties_buffer() {
    auto& m = MorseModel::instance();
    TextInput* ti = m.passwordInput();
    ti->clear();
    ti->insert('x');
    ti->insert('y');
    CHECK_EQ((size_t)2, ti->length());
    m.wifiClearPassword();
    CHECK_EQ((size_t)0, ti->length());
}

static void test_wifi_reset_ui_state_zeros_scan_and_password() {
    auto& m = MorseModel::instance();
    m.setWifiScanCount(5);
    m.setWifiScanCursor(3);
    m.setWifiScanTop(2);
    m.passwordInput()->clear();
    m.passwordInput()->insert('Z');

    m.wifiResetUIState();
    CHECK_EQ(m.wifiScanCursor(), 0);
    CHECK_EQ(m.wifiScanTop(), 0);
    CHECK_EQ((size_t)0, m.passwordInput()->length());
}

static void test_wifi_screens_round_trip() {
    auto& m = MorseModel::instance();
    m.setScreen(DisplayScreen::WIFI_SCAN_LIST);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::WIFI_SCAN_LIST);
    m.setScreen(DisplayScreen::WIFI_PASSWORD_INPUT);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::WIFI_PASSWORD_INPUT);
    m.setScreen(DisplayScreen::WIFI_NETWORK_INFO);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::WIFI_NETWORK_INFO);
    m.setScreen(DisplayScreen::DECODER);
    CHECK_EQ((int)m.screen(), (int)DisplayScreen::DECODER);
}

static void test_wifi_teardown_to_defaults() {
    auto& m = MorseModel::instance();
    m.wifiResetUIState();
    m.setWifiState(0);
    m.setWifiLocalIP(0);
    m.setWifiHasCredentials(false);
    m.setWifiCredSource(0);
    m.setWifiSecondsUntilRetry(0);
    m.setWifiScanCount(0);
    CHECK_EQ(m.wifiState(), 0);
    CHECK_EQ(m.wifiLocalIP(), 0u);
    CHECK(!m.wifiHasCredentials());
    CHECK_EQ(m.wifiCredSource(), 0);
    CHECK_EQ(m.wifiSecondsUntilRetry(), 0u);
    CHECK_EQ(m.wifiScanCount(), 0);
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
    RUN(test_radio_keying_default_off);
    RUN(test_radio_keying_set_on);
    RUN(test_radio_keying_toggle_increments_change_counter);
    RUN(test_keying_settings_screen_round_trip);
    RUN(test_radio_keying_persists_across_screen_changes);
    RUN(test_radio_keying_teardown_resets_to_off);

    RUN(test_wifi_state_default_idle);
    RUN(test_wifi_state_set_and_get);
    RUN(test_wifi_state_same_value_is_noop);
    RUN(test_wifi_ip_default_zero);
    RUN(test_wifi_ip_set_and_get);
    RUN(test_wifi_has_credentials_round_trip);
    RUN(test_wifi_cred_source_default_none);
    RUN(test_wifi_cred_source_set_and_get);
    RUN(test_wifi_seconds_until_retry_set_and_get);
    RUN(test_wifi_scan_count_clamps_negative);
    RUN(test_wifi_scan_cursor_clamps_within_count);
    RUN(test_wifi_scan_cursor_keeps_window_visible);
    RUN(test_wifi_scan_empty_list_resets_to_zero);
    RUN(test_password_input_lazily_constructed);
    RUN(test_password_input_buffer_is_callers_storage);
    RUN(test_wifi_clear_password_empties_buffer);
    RUN(test_wifi_reset_ui_state_zeros_scan_and_password);
    RUN(test_wifi_screens_round_trip);
    RUN(test_wifi_teardown_to_defaults);
    return test_summary();
}