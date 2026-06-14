/**
 * cardputer_display.cpp - Cardputer display implementation.
 *
 * Renders MorseModel state on the Cardputer's 240x135 LCD.
 *
 * Layout (top to bottom):
 *   y=0-19   : Status line (mode, pattern bar, WPM, frequency)
 *   y=20-134 : Main area (decoded morse text or overlay)
 *
 * Scrolling:
 *   Direct rendering with per-char pixel measurement (FreeMono24pt7b).
 *   Auto-scroll: 1 char per render tick when content overflows.
 *   Per-char attributes (K=keyer, P=player) drive color changes.
 */

#include "cardputer_display.h"
#ifdef BOARD_CARDPUTER
#include <M5Cardputer.h>
#include <string>

CardputerDisplay::CardputerDisplay() = default;

void CardputerDisplay::init() {
    clear();
    M5.Display.setFont(&fonts::FreeMono24pt7b);
}

void CardputerDisplay::clear() {
    M5.Display.clear(COLOR_BG);
}

void CardputerDisplay::render() {
    auto& model = MorseModel::instance();
    if (!model.isDisplayActive()) return;
    Serial.printf("[CD] render: screen=%d wpm=%d freq=%d mode=%d\n",
        (int)model.screen(), model.wpm(),
        (int)model.frequency(), (int)model.mode());
    clear();

    switch (model.screen()) {
        case DisplayScreen::WPM_VIEW:
            updateStatusLine(model);
            showWPMView(model);
            break;
        case DisplayScreen::FREQ_VIEW:
            updateStatusLine(model);
            showFreqView(model);
            break;
        case DisplayScreen::WPM_SETTINGS:
            updateStatusLine(model);
            showWPMSettingsView(model);
            break;
        case DisplayScreen::FREQ_SETTINGS:
            updateStatusLine(model);
            showFreqSettingsView(model);
            break;
        case DisplayScreen::VOLUME_VIEW:
            updateStatusLine(model);
            showVolumeView(model);
            break;
        case DisplayScreen::VOLUME_SETTINGS:
            updateStatusLine(model);
            showVolumeSettingsView(model);
            break;
        case DisplayScreen::MODE_VIEW:
            updateStatusLine(model);
            showModeView(model);
            break;
        case DisplayScreen::MODE_SETTINGS:
            updateStatusLine(model);
            showModeSettingsView(model);
            break;
        case DisplayScreen::DECODER:
        default:
            updateStatusLine(model);
            updateMainText(model);
            break;
    }
}

void CardputerDisplay::updateStatusLine(MorseModel& model) {
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(STATUS_TEXT_SIZE);
    M5.Display.setTextColor(COLOR_FG);

    // Left: keyer type label (Paddle or Straight) or ENCODER
    const char* modeStr;
    if (model.mode() == KeyerMode::ENCODER) {
        modeStr = "ENCODER";
    } else {
        modeStr = (model.keyerType() == KeyerType::STRAIGHT) ? "Straight" : "Paddle";
    }
    M5.Display.setCursor(0, 3);
    M5.Display.print(modeStr);

    // Middle: compact WPM / FREQ / VOL display
    M5.Display.setCursor(65, 3);
    M5.Display.printf("%dW %dHz %d%%",
        model.wpm(),
        (int)model.frequency(),
        model.volume());

    // Right: battery level
    int bat = M5Cardputer.Power.getBatteryLevel();
    bool charging = M5Cardputer.Power.isCharging();
    M5.Display.setCursor(195, 3);
    M5.Display.setTextColor(charging ? COLOR_ACCENT : COLOR_FG);
    M5.Display.printf("%d%%", bat);
    M5.Display.setTextColor(COLOR_FG);

    // Small battery bar below percentage
    uint16_t barColor = (bat <= 20) ? COLOR_WARN : COLOR_ACCENT;
    M5.Display.fillRect(195, 12, (bat * 40) / 100, 4, barColor);
    // Background for remainder of bar
    int batBarW = (bat * 40) / 100;
    M5.Display.fillRect(195 + batBarW, 12, 40 - batBarW, 4, 0x2104);
}

void CardputerDisplay::updateMainText(MorseModel& model) {
    M5.Display.setFont(&fonts::FreeMono24pt7b);
    const char* text = model.decodedText();
    size_t len = model.decodedTextLen();
    renderScrollingText(text, len, 0);
}

void CardputerDisplay::showWPMView(MorseModel& model) {
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 10);
    M5.Display.print("WPM");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(40, MAIN_Y + 24);
    M5.Display.printf("%d", model.wpm());

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 100);
    M5.Display.print("S+W: +/-1    ENTER: back");
}

void CardputerDisplay::showFreqView(MorseModel& model) {
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 10);
    M5.Display.print("FREQ");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(10, MAIN_Y + 24);
    M5.Display.printf("%d", (int)model.frequency());

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 100);
    M5.Display.print("S+W: +/-10Hz  ENTER: back");
}

void CardputerDisplay::showWPMSettingsView(MorseModel& model) {
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 4);
    M5.Display.print("WPM");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COLOR_ACCENT);
    M5.Display.setCursor(0, MAIN_Y + 28);
    M5.Display.printf("%d", model.wpm());

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 95);
    M5.Display.print(";/.: -/+   ENTER: confirm");
}

void CardputerDisplay::showFreqSettingsView(MorseModel& model) {
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 4);
    M5.Display.print("FREQ");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COLOR_ACCENT);
    M5.Display.setCursor(0, MAIN_Y + 28);
    M5.Display.printf("%d", (int)model.frequency());

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 95);
    M5.Display.print(";/.: -/+Hz  ENTER: confirm");
}

void CardputerDisplay::showVolumeView(MorseModel& model) {
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 10);
    M5.Display.print("VOL");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(40, MAIN_Y + 24);
    M5.Display.printf("%d%%", model.volume());

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 100);
    M5.Display.print("S+W: +/-1    ENTER: back");
}

void CardputerDisplay::showVolumeSettingsView(MorseModel& model) {
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 4);
    M5.Display.print("VOL");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COLOR_ACCENT);
    M5.Display.setCursor(0, MAIN_Y + 28);
    M5.Display.printf("%d%%", model.volume());

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 95);
    M5.Display.print(";/.: -/+   ENTER: confirm");
}

void CardputerDisplay::showModeView(MorseModel& model) {
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 10);
    M5.Display.print("MODE");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(0, MAIN_Y + 24);
    M5.Display.print(model.keyerType() == KeyerType::STRAIGHT ? "Straight" : "Paddle");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 100);
    M5.Display.print("ENTER: back");
}

void CardputerDisplay::showModeSettingsView(MorseModel& model) {
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 4);
    M5.Display.print("MODE");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COLOR_ACCENT);
    M5.Display.setCursor(0, MAIN_Y + 28);
    M5.Display.print(model.keyerType() == KeyerType::STRAIGHT ? "Straight" : "Paddle");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 95);
    M5.Display.print(";/.: change   ENTER: confirm");
}

void CardputerDisplay::renderScrollingText(const char* text, size_t textLen, size_t maxVisible) {
    (void)text;
    (void)maxVisible;

    auto& m = MorseModel::instance();
    size_t head = m.textHead();
    size_t len = m.decodedTextLen();

    if (len == 0) {
        _scrollOffset = 0;
        M5.Display.setFont(&fonts::FreeMono24pt7b);
        M5.Display.setTextColor(0x7384);
        M5.Display.setCursor(0, MAIN_Y + 45);
        M5.Display.print("waiting...");
        M5.Display.setFont(nullptr);
        return;
    }

    M5.Display.setFont(&fonts::FreeMono24pt7b);
    int charW = M5.Display.textWidth("M");
    int maxChars = (SCREEN_W - 2) / charW;
    M5.Display.setFont(nullptr);

    size_t start;
    if (len <= (size_t)maxChars) {
        start = 0;
    } else {
        start = (head + TEXT_BUF_SIZE - maxChars) % TEXT_BUF_SIZE;
    }

    std::string dbg;
    dbg.reserve(maxChars);
    for (int i = 0; i < maxChars && i < (int)len; ++i) {
        size_t idx = (start + i) % TEXT_BUF_SIZE;
        char raw = m.textAt(idx);
        dbg.push_back(raw == ' ' ? '_' : raw);
    }
    Serial.printf("[CD] renderScrollingText: len=%zu head=%zu start=%zu maxChars=%d -> \"%s\"\n",
        len, head, start, maxChars, dbg.c_str());

    M5.Display.setFont(&fonts::FreeMono24pt7b);
    M5.Display.setCursor(0, MAIN_Y + 45);

    char lastAttr = 0;
    for (int i = 0; i < maxChars && i < (int)len; ++i) {
        size_t idx = (start + i) % TEXT_BUF_SIZE;
        char raw = m.textAt(idx);
        char attr = m.attrAt(idx);

        if (attr != lastAttr) {
            M5.Display.setTextColor((attr == MorseModel::ATTR_PLAYER) ? COLOR_ACCENT : COLOR_FG);
            lastAttr = attr;
        }
        M5.Display.print(raw == ' ' ? '_' : raw);
    }

    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setFont(nullptr);
}

void CardputerDisplay::powerOff() {
    M5.Display.setBrightness(0);
    M5.Display.clear();
}

void CardputerDisplay::powerOn() {
    M5.Display.wakeup();
    M5.Display.setBrightness(255);
}

#endif  // BOARD_CARDPUTER