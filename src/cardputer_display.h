#pragma once
/**
 * cardputer_display.h — Cardputer display implementation.
 *
 * Hardware: M5Cardputer 240×135 LCD driven by M5.Display (Adafruit GFX).
 *
 * Font sizes (Adafruit GFX built-in 8×16 font, scaled by N):
 *   SIZE 1 →  8×16 px per character
 *   SIZE 2 → 16×32 px per character
 *   SIZE 3 → 24×48 px per character  (current MAIN_TEXT_SIZE)
 *
 * Visible character count at each size:
 *   SIZE 1 → 240 /  8 = 30 chars per line
 *   SIZE 2 → 240 / 16 = 15 chars per line
 *   SIZE 3 → 240 / 24 = 10 chars per line  ← current (spacious)
 *
 * The spacing between characters in renderScrollingText() adds one
 * extra character-cell width of gap after each rendered character.
 * At SIZE 3 this means each "slot" occupies 48px, allowing only ~5
 * characters to be visible at once. Use SIZE 2 for denser output.
 */

#include "display_interface.h"
#include <cstdint>
#ifdef BOARD_CARDPUTER
#include <M5Cardputer.h>
#endif

class CardputerDisplay : public DisplayInterface {
public:
    // ─── Display dimensions ─────────────────────────────────────────────────

    /** Pixel width of the Cardputer LCD. */
    static constexpr int SCREEN_W = 240;

    /** Pixel height of the Cardputer LCD. */
    static constexpr int SCREEN_H = 135;

    // ─── Layout ─────────────────────────────────────────────────────────────

    /** Height of the status line in pixels (top of screen). */
    static constexpr int STATUS_HEIGHT = 20;

    /**
     * Vertical offset where the main text area begins.
     * Equal to STATUS_HEIGHT. Main area spans y=STATUS_HEIGHT .. SCREEN_H-1.
     */
    static constexpr int MAIN_Y = STATUS_HEIGHT;

    // ─── Font sizes ─────────────────────────────────────────────────────────
    //
    // STATUS LINE: uses built-in bitmap font (16×32 px at scale 2)
    // MAIN TEXT:   uses Montserrat smooth font (36px or 48px native)
    //
    // Font switching: M5.Display.setFont(nullptr)  → built-in bitmap
    //                M5.Display.setFont(&fonts::lv_font_montserrat_XX) → smooth
    //
    // When Montserrat is loaded, setTextSize(1) = native XX px,
    // setTextSize(2) = 2×XX px, etc. (integer scales are crisp).
    //
    // Montserrat glyph widths (approximate, proportional):
    //   24px native: ~20px/char → 12 chars/240px
    //   36px native: ~30px/char →  8 chars/240px
    //   48px native: ~40px/char →  6 chars/240px

    /** Font size for the status line (built-in bitmap, 8×16 px). */
    static constexpr int STATUS_TEXT_SIZE = 1;

    /**
     * Smooth font size for main decoded-morse text (Montserrat 36px).
     * Used by renderScrollingText() for the scrolling decoded text area.
     * Visible chars: 240 / 30 ≈ 8 chars at 36px native size.
     */
    static constexpr int SMOOTH_FONT_SIZE = 36;

    /**
     * Text size for decoded morse display (bitmap fallback scale factor).
     * Used when SMOOTH_FONT_SIZE == 0.
     */
    static constexpr int MAIN_TEXT_SIZE = 3;

    /**
     * Font size for the currently playing encoder character (Montserrat 48px).
     * Displayed large in ENCODER mode.
     */
    static constexpr int SCROLL_SPRITE_W = SCREEN_W * 2;  // 480px
    static constexpr int SCROLL_SPRITE_H = 32;            // 32px — FreeMono24pt7b fits
    static constexpr int SCROLL_TEXT_Y  = 8;            // baseline y inside sprite

    // ─── Circular text buffer ────────────────────────────────────────────────
    // Must match MorseModel::TEXT_BUF_SIZE. Kept here for local computations.

    /** Matches MorseModel::TEXT_BUF_SIZE (200 chars). */
    static constexpr size_t TEXT_BUF_SIZE = 200;

    // ─── Colours (RGB565 16-bit) ────────────────────────────────────────────
    // RGB565: R in bits[15:11], G in bits[10:5], B in bits[4:0]
    // Use https://rgb565.com to convert.

    static constexpr uint16_t COLOR_BG     = 0x0000;  // black
    static constexpr uint16_t COLOR_FG     = 0xFFFF;  // white
    static constexpr uint16_t COLOR_ACCENT = 0x07E0;  // bright
    static constexpr uint16_t COLOR_WARN   = 0xF800;  // red

    // ─── Construction ───────────────────────────────────────────────────────

    CardputerDisplay();

    // ─── DisplayInterface implementation ───────────────────────────────────

    /**
     * init — initialise the display hardware.
     *
     * Called once by DisplayTask::begin() before the first render.
     * Clears the screen to COLOR_BG.
     */
    void init() override;

    /**
     * clear — clear the entire screen to the background colour.
     *
     * Fills the full display area with COLOR_BG (black).
     */
    void clear() override;

    /**
     * render — full display render (status line + main area or overlay).
     *
     * Reads current MorseModel state and dispatches to the appropriate
     * screen renderer:
     *   DECODER  → updateStatusLine() + updateMainText()
     *   WPM_VIEW → updateStatusLine() + showWPMView()
     *   FREQ_VIEW → updateStatusLine() + showFreqView()
     *
     * Checks isDisplayActive() first — if the screen is in power-save
     * mode (backlight off), skips rendering without clearing.
     */
    void render() override;

    /**
     * updateStatusLine — top 20px status bar.
     *
     * Displays four items across the top 20px:
     *   1. Mode label left-aligned: "KEYER" or "ENCODER"
     *   2. Pattern progress bar: filled rectangle showing DIT/(DIT+DAH) ratio
     *   3. Pattern percent text right of bar
     *   4. WPM and frequency on the second row right-aligned
     *
     * @param model  MorseModel reference providing current values.
     *
     * Example output:
     *   KEYER  ████████░░░░ 60%       20W 500F
     */
    void updateStatusLine(MorseModel& model) override;

    /**
     * updateMainText — main decoded-morse text area (y=20–134).
     *
     * KEYER mode:
     *   Renders the scrolling decoded morse text at MAIN_TEXT_SIZE.
     *   Auto-scrolls when content overflows the visible width.
     *
     * ENCODER mode:
     *   Renders the current character at (ENCODER_TEXT_SIZE + 1) scale
     *   in accent colour, with the decoded morse history beneath it at
     *   STATUS_TEXT_SIZE in dimmed colour.
     *
     * @param model  MorseModel reference.
     */
    void updateMainText(MorseModel& model) override;

    /**
     * showWPMView — WPM overlay (auto-dismisses after 10s).
     *
     * Content:
     *   - "WPM" label top-left (2× scale)
     *   - Current WPM value centred large (5× scale, e.g. "20")
     *   - Hint row at bottom: "S+W: +/-1    ENTER: back"
     *
     * @param model  MorseModel reference.
     */
    void showWPMView(MorseModel& model) override;

    /**
     * showFreqView — Frequency overlay (auto-dismisses after 10s).
     *
     * Content:
     *   - "FREQ" label top-left (2× scale)
     *   - Frequency value centred large (5× scale, e.g. "500")
     *   - "Hz" suffix beside value (2× scale)
     *   - Hint row at bottom: "S+W: +/-10Hz  ENTER: back"
     *
     * @param model  MorseModel reference.
     */
    void showFreqView(MorseModel& model) override;

    /**
     * showWPMSettingsView — WPM settings (in-place editing, no auto-dismiss).
     *
     * Shows: "WPM" label (2x), large current value (3x), hint row.
     * Adjusted with Fn+; (up) / Fn+. (down). Confirmed with Enter.
     * 10s inactivity timeout is handled by DisplayTask via OVERLAY_TIMEOUT_MS.
     *
     * @param model  MorseModel reference.
     */
    void showWPMSettingsView(MorseModel& model) override;

    /**
     * showFreqSettingsView — Frequency settings (in-place editing, no auto-dismiss).
     *
     * Shows: "FREQ" label (2x), large current value (3x), hint row.
     * Adjusted with Fn+; (up) / Fn+. (down). Confirmed with Enter.
     * 10s inactivity timeout is handled by DisplayTask via OVERLAY_TIMEOUT_MS.
     *
     * @param model  MorseModel reference.
     */
    void showFreqSettingsView(MorseModel& model) override;

    /**
     * showVolumeView — Volume overlay (auto-dismisses after 10s).
     *
     * Content:
     *   - "VOL" label top-left (2× scale)
     *   - Current volume value centred large (3x scale, e.g. "50%")
     *   - Hint row at bottom: "S+W: +/-1    ENTER: back"
     *
     * @param model  MorseModel reference.
     */
    void showVolumeView(MorseModel& model) override;

    /**
     * showVolumeSettingsView — Volume settings (in-place editing, no auto-dismiss).
     *
     * Shows: "VOL" label (2x), large current value (3x), hint row.
     * Adjusted with Fn+; (up) / Fn+. (down). Confirmed with Enter.
     * 10s inactivity timeout is handled by DisplayTask via OVERLAY_TIMEOUT_MS.
     *
     * @param model  MorseModel reference.
     */
    void showVolumeSettingsView(MorseModel& model) override;

    /**
     * showModeView — Mode overlay (auto-dismisses after 10s).
     *
     * Content:
     *   - "MODE" label top-left (2× scale)
     *   - Current mode value centred large (3x scale, e.g. "Paddle")
     *   - Hint row at bottom: "S+W: +/-1    ENTER: back"
     *
     * @param model  MorseModel reference.
     */
    void showModeView(MorseModel& model) override;

    /**
     * showModeSettingsView — Mode settings (in-place editing, no auto-dismiss).
     *
     * Shows: "MODE" label (2x), large current value (3x), hint row.
     * Adjusted with Fn+; (Paddle) / Fn+. (Straight). Confirmed with Enter.
     * 10s inactivity timeout is handled by DisplayTask via OVERLAY_TIMEOUT_MS.
     *
     * @param model  MorseModel reference.
     */
    void showModeSettingsView(MorseModel& model) override;

    // ─── Screen-saver control ────────────────────────────────────────────────

    /**
     * powerOff — activate screen-saver (backlight off).
     *
     * Sets display brightness to 0 and clears the screen.
     * Called by DisplayTask when inactivity timeout (5 min) expires.
     *
     * The M5.Display.setBrightness(0) turns off the backlight only;
     * display content RAM is preserved and restored on wake.
     */
    void powerOff() override;

    /**
     * powerOn — deactivate screen-saver (backlight on).
     *
     * Calls M5.Display.wakeup() and sets brightness to 255.
     * Called by DisplayTask when wakeFromScreensaver() is triggered.
     */
    void powerOn() override;

private:
    /**
     * renderScrollingText — render decoded text with automatic left-scroll.
     *
     * Uses a sprite-based double-buffer approach: text is drawn to an
     * offscreen sprite (wide enough to hold the full circular buffer),
     * then pushed to the LCD with pushSprite() in one DMA transfer.
     * This eliminates flicker and supports proper negative x-coordinate
     * clipping for proportional fonts.
     *
     * The scroll offset tracks how many oldest characters are hidden.
     * Color changes (keyer white ↔ player green) are driven by per-character
     * attributes stored in the circular buffer — no index arithmetic needed.
     *
     * @param text       Ignored — always reads via MorseModel accessors.
     * @param textLen    Total number of valid characters in buffer.
     * @param maxVisible Reserved for future use (currently always auto).
     */
    void renderScrollingText(const char* text, size_t textLen, size_t maxVisible = 0);

    /** Tracks the scroll offset (number of oldest chars to skip). */
    int _scrollOffset = 0;

    /** Offscreen sprite for flicker-free scrolling text (PSRAM-backed). */
#ifdef BOARD_CARDPUTER
    M5Canvas _scrollSprite{&M5.Display};
#else
    void* _scrollSprite = nullptr;
#endif

    /** True after createSprite() succeeds. */
    bool _spriteReady = false;

    /**
     * ensureSpriteReady — create the scroll sprite if not already created.
     * @return true if sprite is ready, false if creation failed.
     */
    bool ensureSpriteReady();

};