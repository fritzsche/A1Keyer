#pragma once
/**
 * ui_layout.h — Per-device UI geometry for Morse Trainer.
 *
 * Adjust button positions and sizes to fit the target display.
 * Tab5:  320×480 px (landscape-capable, capacitive touch)
 * Cardputer: 240×135 px (portrait, no touch — uses keyboard/buttons)
 */

#if defined(BOARD_TAB5)
    // ── Tab5 layout ─────────────────────────────────────────────────────────
    // Play button: centered horizontally, near the bottom
    static constexpr int PLAY_BTN_X = 40;
    static constexpr int PLAY_BTN_Y = 300;
    static constexpr int PLAY_BTN_W = 240;
    static constexpr int PLAY_BTN_H = 80;

    // Volume buttons: row above the play button
    static constexpr int VOL_BTN_Y  = 100;
    static constexpr int VOL_BTN_H  = 60;
    static constexpr int VOL_BTN_W  = 120;
    static constexpr int VOL_DOWN_X = 0;
    static constexpr int VOL_UP_X   = 200;

    // Title area: top of screen
    static constexpr int TITLE_Y    = 0;
    static constexpr int TEXT_SIZE  = 3;

#elif defined(BOARD_CARDPUTER)
    // ── Cardputer layout ─────────────────────────────────────────────────────
    // Compact: 240×135 display, no touch — UI is for future serial/button input.
    // All three buttons in a row at the bottom, stacked vertically.
    static constexpr int PLAY_BTN_X = 20;
    static constexpr int PLAY_BTN_Y = 90;
    static constexpr int PLAY_BTN_W = 200;
    static constexpr int PLAY_BTN_H = 35;

    static constexpr int VOL_BTN_Y  = 50;
    static constexpr int VOL_BTN_H  = 30;
    static constexpr int VOL_BTN_W  = 80;
    static constexpr int VOL_DOWN_X = 0;
    static constexpr int VOL_UP_X   = 160;   // leave gap in middle

    static constexpr int TITLE_Y    = 0;
    static constexpr int TEXT_SIZE  = 1;     // smaller font for 240px width

#else
    #error "Unknown BOARD_xxx — define BOARD_TAB5 or BOARD_CARDPUTER in platformio.ini build_flags"
#endif