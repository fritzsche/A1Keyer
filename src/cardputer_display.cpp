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
#include "Log.h"
#include "network_manager.h"
#include "text_input.h"
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
    Log::debug("[CD] render: screen=%d wpm=%d freq=%d mode=%d",
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
        case DisplayScreen::KEYING_SETTINGS:
            updateStatusLine(model);
            showKeyingSettingsView(model);
            break;
        case DisplayScreen::WIFI_SCAN_LIST:
            updateStatusLine(model);
            showWifiScanList(model);
            break;
        case DisplayScreen::WIFI_PASSWORD_INPUT:
            updateStatusLine(model);
            showWifiPasswordInput(model);
            break;
        case DisplayScreen::WIFI_NETWORK_INFO:
            updateStatusLine(model);
            showWifiNetworkInfo(model);
            break;
        case DisplayScreen::MEMORY_PICK:
            updateStatusLine(model);
            showMemoryPick(model);
            break;
        case DisplayScreen::MEMORY_EDIT:
            updateStatusLine(model);
            showMemoryEdit(model);
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

    // Mode indicator: shows "WK" while the serial line is speaking the
    // WinKeyer protocol to a host logger (RUMlogNG), "DBG" while it is
    // carrying free-form debug logs. Drawn in the accent colour so it
    // stands out from the surrounding WPM / freq / vol readout.
    M5.Display.setTextColor(COLOR_ACCENT);
    M5.Display.setCursor(52, 3);
    M5.Display.print(model.winkeyMode() ? "WK" : "DBG");
    M5.Display.setTextColor(COLOR_FG);

    // Middle: compact WPM / FREQ / VOL display
    M5.Display.setCursor(65, 3);
    M5.Display.printf("%dW %dHz %d%%",
        model.wpm(),
        (int)model.frequency(),
        model.volume());

    // Right: battery level. Cache the PMIC reads for 5 s so a typing
    // session on the password screen — which triggers a full repaint on
    // every keystroke — does not also trigger two I2C reads per
    // keystroke. The PMIC shares the internal I2C bus with the TCA8418
    // keyboard controller, and a busy PMIC read at the wrong moment can
    // delay the key release the keyboard handler is waiting for, making
    // the input feel frozen for several seconds.
    static int     cachedBat       = -1;
    static bool    cachedCharging  = false;
    static uint32_t lastBatPollMs  = 0;
    const uint32_t nowMs = millis();
    if (cachedBat < 0 || (int32_t)(nowMs - lastBatPollMs) >= 5000) {
        cachedBat      = M5Cardputer.Power.getBatteryLevel();
        cachedCharging = M5Cardputer.Power.isCharging();
        lastBatPollMs  = nowMs;
    }
    const int  bat      = cachedBat;
    const bool charging = cachedCharging;

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

void CardputerDisplay::showKeyingSettingsView(MorseModel& model) {
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 4);
    M5.Display.print("KEYING");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(3);
    // Use warning colour when ON so the user notices that GPIO4 will be
    // driving a real transmitter.
    M5.Display.setTextColor(model.radioKeyingEnabled() ? COLOR_WARN : COLOR_ACCENT);
    M5.Display.setCursor(0, MAIN_Y + 28);
    M5.Display.print(model.radioKeyingEnabled() ? "On" : "Off");

    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 95);
    M5.Display.print(";: On   .: Off   ENTER: confirm");
}

// ─── Wi-Fi screens ───────────────────────────────────────────────────────────
//
// These renderers read live state directly from NetworkManager. The model
// carries only the cursor position and the password editor — everything
// else is owned by the manager and is safe to read from the display core
// because all writes happen on the loop core.

namespace {

/// Convert the model-side int mirror of NetState to the enum.
NetState mirrorState(int v) {
    if (v < 0 || v > (int)NetState::DISCONNECTED) return NetState::IDLE;
    return static_cast<NetState>((uint8_t)v);
}

const char* stateLabel(NetState s) {
    switch (s) {
        case NetState::IDLE:            return "ready";
        case NetState::SCANNING:        return "scanning";
        case NetState::SCAN_DONE:       return "choose";
        case NetState::SCAN_FAILED:     return "scan failed";
        case NetState::CONNECTING:      return "connecting";
        case NetState::CONNECTED:       return "connected";
        case NetState::CONNECT_FAILED:  return "connect failed";
        case NetState::DISCONNECTED:    return "disconnected";
    }
    return "?";
}

/// SSID clipped to a width that fits a 240px row (cursor + lock + RSSI
/// + margins). The whole row, including the trailing RSSI digit, has to
/// land before the scrollbar thumb at x=232.
void printSsidClipped(const char* ssid, int x, int y, int maxChars) {
    if (!ssid) return;
    const int n = (int)std::char_traits<char>::length(ssid);
    if (n <= maxChars) {
        M5.Display.print(ssid);
    } else {
        for (int i = 0; i < maxChars - 1; ++i) M5.Display.print(ssid[i]);
        M5.Display.print('>');
    }
}

}  // namespace

void CardputerDisplay::showWifiScanList(MorseModel& model) {
    const NetState st = mirrorState(model.wifiState());

    // Title
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 4);
    M5.Display.print("WiFi");

    // State line right of the title
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COLOR_ACCENT);
    M5.Display.setCursor(50, MAIN_Y + 8);
    M5.Display.print(stateLabel(st));

    // Row counter top-right
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(195, MAIN_Y + 8);
    const int n = model.wifiScanCount();
    M5.Display.printf("%d/%d", model.wifiScanCursor() + (n > 0 ? 1 : 0), n);

    if (st == NetState::SCANNING) {
        // Animated dots — never blocks the keyer
        M5.Display.setFont(nullptr);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(COLOR_FG);
        M5.Display.setCursor(0, MAIN_Y + 50);
        M5.Display.print("Scanning networks");
        const int phase = (int)(millis() / 250) % 4;
        for (int i = 0; i < phase; ++i) M5.Display.print('.');
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0x7384);
        M5.Display.setCursor(0, MAIN_Y + 95);
        M5.Display.print("ESC: cancel");
        return;
    }

    if (st == NetState::SCAN_FAILED || (st == NetState::SCAN_DONE && n == 0)) {
        M5.Display.setFont(nullptr);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(COLOR_WARN);
        M5.Display.setCursor(0, MAIN_Y + 36);
        const char* msg = WifiMgr::lastErrorMessage();
        M5.Display.print(msg[0] ? msg : "scan failed");
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0x7384);
        M5.Display.setCursor(0, MAIN_Y + 95);
        M5.Display.print("ESC: back   R: retry");
        return;
    }

    // Four-row windowed list. Built-in bitmap font at scale 2 is
    // ~12×20 px per glyph, the smallest size still readable at arm's
    // length on the 240×135 LCD. 4 rows × 20 px = 80 px fits between
    // title (y=24..40) and the hint row (y=130).
    //
    // Coordinate convention: setCursor(x, y) on the default bitmap font
    // places the TOP-LEFT of the glyph at (x, y) — text grows downward
    // from there. So `y` is the top of the row's glyph and the
    // highlight rectangle must start at (y - 2) to give a 2 px top
    // margin and end at (y - 2 + kRowH) for a 2 px bottom margin
    // around the 16 px glyph.
    constexpr int kRowH    = 20;
    constexpr int kFirstY  = MAIN_Y + 28;   // y = 48 — first row TOP
    constexpr int kListX   = 16;
    constexpr int kLockX   = 196;           // 1 char lock glyph
    constexpr int kRssiX   = 208;           // up to 3 digits (-99..0)
    constexpr int kMaxSsid = 11;            // 11 chars × 12 px = 132 px

    const int top    = model.wifiScanTop();
    const int cursor = model.wifiScanCursor();
    M5.Display.setTextSize(2);
    for (int row = 0; row < WifiMgr::kPageSize; ++row) {
        const int idx = top + row;
        if (idx >= n) break;
        const NetScanEntry* e = WifiMgr::scanEntry(idx);
        if (!e) break;

        const int y = kFirstY + row * kRowH;     // top of the glyph row
        // Cursor block (full-row highlight on the selected row).
        // Glyph extends from y to y+16; the highlight band sits from
        // (y - 2) to (y - 2 + kRowH) so the glyph is vertically
        // centered inside the row.
        if (idx == cursor) {
            M5.Display.fillRect(0, y - 2, SCREEN_W, kRowH, COLOR_ACCENT);
            M5.Display.setTextColor(COLOR_BG);
        } else {
            M5.Display.setTextColor(COLOR_FG);
        }
        M5.Display.setCursor(kListX, y);
        printSsidClipped(e->ssid, kListX, y, kMaxSsid);

        // Lock or "o" for open
        M5.Display.setCursor(kLockX, y);
        M5.Display.print(e->open ? 'o' : '#');
        // RSSI digit (capped at -9 so a single digit always fits)
        M5.Display.setCursor(kRssiX, y);
        const int rssi = e->rssi;
        M5.Display.printf("%d", rssi > -9 ? rssi : (rssi / 10));
    }

    // Scrollbar on the right edge when there are more rows than fit.
    // The track is aligned to the same vertical extent as the
    // highlight bands: starts 2 px above the first row and spans
    // kPageSize * kRowH.
    if (n > WifiMgr::kPageSize) {
        const int trackX  = SCREEN_W - 2;
        const int trackY  = kFirstY - 2;
        const int trackH  = WifiMgr::kPageSize * kRowH;
        M5.Display.drawFastVLine(trackX, trackY, trackH, 0x7384);
        const int thumbH  = std::max(4, trackH * WifiMgr::kPageSize / n);
        const int thumbY  = trackY + (trackH - thumbH) * top / (n - WifiMgr::kPageSize);
        M5.Display.fillRect(trackX, thumbY, 2, thumbH, COLOR_FG);
    }

    // Hint row
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 110);
    M5.Display.print(";/.: move   ENTER: connect   ESC: back");
}

void CardputerDisplay::showWifiPasswordInput(MorseModel& model) {
    TextInput* ti = model.passwordInput();

    // SSID of the AP being joined — index 0 of the scan list is fine; the
    // model owns the cursor so we can pull the exact entry by index.
    const int idx = model.wifiScanCursor();
    const NetScanEntry* e = WifiMgr::scanEntry(idx);

    // Two-line title so long SSIDs are not truncated alongside the
    // "WiFi pw" label. Both rows are size 2 (12×16 px glyphs); the
    // input box sits one line below.
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 0);
    M5.Display.print("WiFi pw");

    // SSID row. Width budget at size 2 is 20 chars × 12 px = 240 px
    // (= SCREEN_W), so even a long SSID like "MyHomeNetwork-5GHz" fits
    // without clipping. The "(open)" suffix reuses the dim grey so
    // open vs. secured is still visible at a glance.
    M5.Display.setTextColor(COLOR_ACCENT);
    M5.Display.setCursor(0, MAIN_Y + 18);
    M5.Display.print(e ? e->ssid : "");
    M5.Display.setTextColor(0x7384);
    M5.Display.print(e ? (e->open ? " (open)" : "") : "");

    // CAPS indicator — top-right of the first title row. Only drawn
    // when the keyboard's caps lock is engaged so a run of capitals
    // is unambiguous. Toggled by OPT in handleWifiScreen's password
    // branch (see main.cpp).
    if (M5Cardputer.Keyboard.capslocked()) {
        M5.Display.setTextColor(COLOR_WARN);
        M5.Display.setCursor(SCREEN_W - 56, MAIN_Y + 4);
        M5.Display.setTextSize(1);
        M5.Display.print("CAPS");
    }

    // Field box (32 px tall, holds a 16 px size-2 glyph with 8 px
    // padding top and bottom). Moved one line down (was MAIN_Y+22)
    // to make room for the SSID row above.
    constexpr int kBoxX = 4;
    constexpr int kBoxY = MAIN_Y + 38;
    constexpr int kBoxW = SCREEN_W - 8;
    constexpr int kBoxH = 32;
    M5.Display.drawRect(kBoxX, kBoxY, kBoxW, kBoxH, COLOR_FG);

    // CRITICAL: reset to size 2 BEFORE measuring charW and printing.
    // The CAPS indicator above dropped the global text size to 1;
    // without this call the field would render at size 1, half the
    // height of the size-2 glyph the box was sized for.
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(kBoxX + 6, kBoxY + 8);
    const char* txt = ti->value();
    const size_t len = ti->length();
    const size_t cur = ti->cursorPos();
    // Render each character masked by default. Shift+Space in TextInput
    // toggles reveal() so the user can sanity-check what they typed
    // without leaving the plaintext visible to a shoulder-surfer.
    const bool revealed = ti->reveal();
    const char mask = ti->maskChar();
    for (size_t i = 0; i < len; ++i) {
        M5.Display.print(revealed ? txt[i] : mask);
    }

    // Static caret at the cursor position. textWidth() returns the
    // scaled width at the current setTextSize(), so no extra multiplier
    // is needed — the old `* 2` double-counted and placed the caret
    // twice as far right as it should be. A blinking caret at 500 ms
    // interacted badly with the 50 ms renderer cadence and produced
    // visible flicker; the static bar is steady and far easier to read.
    const int charW = M5.Display.textWidth("M");
    const int cx = kBoxX + 6 + (int)cur * charW;
    if (cx + 2 <= kBoxX + kBoxW - 4) {
        M5.Display.fillRect(cx, kBoxY + 6, 2, kBoxH - 12, COLOR_FG);
    }

    // Hint row — kept at size 1 because the longer string with all
    // four gestures (commit, cursor, show, back) overflows at size 2.
    // Placed ~10 px below the input box (which ends at MAIN_Y+70=90)
    // so the bottom of the screen stays visible on the 135 px LCD.
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 80);
    M5.Display.print("ENTER ok  ,/:cur  FN show  ESC bk");
}

void CardputerDisplay::showWifiNetworkInfo(MorseModel& model) {
    const NetState st = mirrorState(model.wifiState());

    // Five size-2 lines fit between the status bar (y=20) and the
    // bottom edge (y=135): title, state, SSID, IP/error, hint. The
    // retry countdown gets the gap above the hint, sized down where
    // space is tight.
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 0);
    M5.Display.print("WiFi");
    M5.Display.setTextColor(COLOR_ACCENT);
    M5.Display.setCursor(56, MAIN_Y + 0);
    M5.Display.print(stateLabel(st));

    // SSID on its own line so the value can be long enough to read
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setCursor(0, MAIN_Y + 22);
    M5.Display.print("SSID:");
    M5.Display.setCursor(68, MAIN_Y + 22);
    const char* ssid = WifiMgr::connectedSSID();
    M5.Display.print(ssid[0] ? ssid : "(none)");

    // IP, or the latest error
    M5.Display.setCursor(0, MAIN_Y + 44);
    if (WifiMgr::isConnected()) {
        const uint32_t ip = model.wifiLocalIP();
        const uint8_t a = (uint8_t)(ip >> 24);
        const uint8_t b = (uint8_t)(ip >> 16);
        const uint8_t c = (uint8_t)(ip >>  8);
        const uint8_t d = (uint8_t)(ip);
        M5.Display.printf("IP: %u.%u.%u.%u", a, b, c, d);
    } else {
        M5.Display.setTextColor(COLOR_WARN);
        const char* msg = WifiMgr::lastErrorMessage();
        M5.Display.print(msg[0] ? msg : "(no connection)");
    }
    M5.Display.setTextColor(COLOR_FG);

    // Retry countdown and credential-source marker share the same
    // line just above the hint. At size 1 they fit alongside the
    // hint without crowding the main info above.
    M5.Display.setTextSize(1);
    if (!WifiMgr::isConnected() && model.wifiSecondsUntilRetry() > 0) {
        M5.Display.setTextColor(0x7384);
        M5.Display.setCursor(0, MAIN_Y + 66);
        M5.Display.printf("retry in %us", (unsigned)model.wifiSecondsUntilRetry());
    }

    // Hint row
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 98);
    M5.Display.print("X: forget  R: retry  ENT: back");
}

// ─── Memory keyer screens ───────────────────────────────────────────────────
//
// Two screens:
//   MEMORY_PICK — 5×2 grid of slots with a short preview of each.
//   MEMORY_EDIT — single-line TextInput modal mirroring the Wi-Fi password
//                 screen layout (so the operator gets a consistent editor).

void CardputerDisplay::showMemoryPick(MorseModel& model) {
    // Title
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_ACCENT);
    M5.Display.setCursor(0, MAIN_Y + 0);
    M5.Display.print("Memory");

    // Sub-prompt — same line as the title, right-aligned so the layout
    // matches the Wi-Fi password screen ("WiFi pw" + SSID below).
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 18);
    M5.Display.print("Pick slot (0-9)");

    // 5 rows × 2 columns of slot previews. At size 1 the default
    // bitmap font is 8×16 px per glyph; row height 20 px (16 + 4
    // margin) leaves the bottom hint row visible above y=130 on the
    // 135 px LCD. Column width 120 px fits ~15 chars at size 1, plenty
    // for "<digit>: <10 char preview>".
    constexpr int kRowH    = 20;
    constexpr int kFirstY  = MAIN_Y + 38;
    constexpr int kColX0   = 0;
    constexpr int kColX1   = 120;
    constexpr int kDigitW  = 8;       // 1 char + ':' at size 1
    constexpr int kMaxText = 10;      // preview length budget per cell

    M5.Display.setTextSize(1);
    for (uint8_t i = 0; i < kMemSlots; ++i) {
        const int row = i / 2;       // 0..4
        const int col = i % 2;       // 0..1
        const int y   = kFirstY + row * kRowH;
        const int x   = (col == 0) ? kColX0 : kColX1;

        // Slot digit + colon in accent so the operator can scan digits
        // quickly even when most slots are populated.
        M5.Display.setTextColor(COLOR_ACCENT);
        M5.Display.setCursor(x, y);
        M5.Display.printf("%u:", (unsigned)i);

        // Preview the slot's content, or "(empty)" in dim grey when
        // the slot has no text yet. The preview is clamped to kMaxText
        // chars to keep both columns aligned.
        M5.Display.setCursor(x + kDigitW + 4, y);
        const char* txt = model.getMemory(i);
        if (!txt || txt[0] == '\0') {
            M5.Display.setTextColor(0x7384);
            M5.Display.print("(empty)");
        } else {
            M5.Display.setTextColor(COLOR_FG);
            for (int j = 0; j < kMaxText && txt[j] != '\0'; ++j) {
                M5.Display.print(txt[j]);
            }
        }
    }

    // Hint row
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 110);
    M5.Display.print("0-9: pick   ESC: back");
}

void CardputerDisplay::showMemoryEdit(MorseModel& model) {
    const int slot = model.memoryEditingSlot();
    // Defensive: if a render races with a screen change (e.g. the
    // operator pressed ESC from the picker a millisecond before the
    // display task woke), fall back to a placeholder title rather
    // than printing "Mem -1".
    const char* title = "Mem ?";
    char titleBuf[16];
    if (slot >= 0 && slot < (int)kMemSlots) {
        snprintf(titleBuf, sizeof(titleBuf), "Mem %d", slot);
        title = titleBuf;
    }

    // Title — size 2 in accent color. Slot number is read at a glance.
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_ACCENT);
    M5.Display.setCursor(0, MAIN_Y + 0);
    M5.Display.print(title);

    // CAPS indicator — top-right, before the count badge. Only drawn
    // when caps lock is engaged.
    if (M5Cardputer.Keyboard.capslocked()) {
        M5.Display.setTextColor(COLOR_WARN);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(SCREEN_W - 88, MAIN_Y + 4);
        M5.Display.print("CAPS");
    }

    // Char-count badge — top-right, warning-red when within three
    // chars of the cap so the operator notices they are running out
    // of room before the field starts refusing characters.
    TextInput* ti = model.memoryInput();
    const size_t len  = ti->length();
    const size_t cap  = kMemLen - 1;
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(len >= cap - 2 ? COLOR_WARN : 0x7384);
    M5.Display.setCursor(SCREEN_W - 40, MAIN_Y + 4);
    M5.Display.printf("%zu/%zu", len, cap);

    // Field box. Width matches the Wi-Fi password screen for muscle
    // memory; height (32 px) is sized to fit a size-3 glyph (24 px)
    // with 4 px padding top and bottom. The text inside is rendered
    // at SIZE 3 — much larger than the Wi-Fi password screen, which
    // is sized for the masked glyph only. Memory text is never
    // masked (the operator needs to read what they typed), so we
    // can afford the larger font.
    constexpr int kBoxX = 4;
    constexpr int kBoxY = MAIN_Y + 38;
    constexpr int kBoxW = SCREEN_W - 8;
    constexpr int kBoxH = 32;
    M5.Display.drawRect(kBoxX, kBoxY, kBoxW, kBoxH, COLOR_FG);

    // Horizontal scroll window. At size 3 each glyph is ~18 px wide;
    // 13 characters fit in the box minus padding. When the cursor
    // walks past the right edge we slide the visible window leftward
    // so the cursor stays anchored at the right of the box — newly
    // typed text is never off-screen. Walking back left follows the
    // window until it hits the left edge, then stays put.
    //
    // CRITICAL: bump to size 3 BEFORE measuring charW and printing.
    // The count badge above left the global text size at 1, and
    // Adafruit GFX's textWidth / print honour the current size — so
    // without this call the field would render at size 1 (the same
    // size-1 default that masked the Wi-Fi password screen).
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COLOR_FG);
    const int charW = M5.Display.textWidth("M");
    constexpr int kPadL = 6;
    constexpr int kPadR = 4;
    const int maxChars = (kBoxW - kPadL - kPadR) / charW;
    const size_t cur = ti->cursorPos();
    const size_t scrollStart = (cur > (size_t)(maxChars - 1))
                             ? cur - (maxChars - 1)
                             : 0;
    const size_t renderEnd   = (len < scrollStart + (size_t)maxChars)
                             ? len
                             : scrollStart + (size_t)maxChars;

    M5.Display.setCursor(kBoxX + kPadL, kBoxY + 4);
    const char* txt = ti->value();
    for (size_t i = scrollStart; i < renderEnd; ++i) {
        M5.Display.print(txt[i]);
    }

    // Static caret at the cursor position within the visible window.
    // The mask glyph is irrelevant — memory text is always visible.
    // (Wi-Fi passwords stay masked; this is the intentional asymmetry
    // between the two editor screens.)
    const int cx = kBoxX + kPadL + (int)(cur - scrollStart) * charW;
    if (cx + 2 <= kBoxX + kBoxW - 4) {
        M5.Display.fillRect(cx, kBoxY + 4, 2, kBoxH - 8, COLOR_FG);
    }

    // Hint rows — no FN show (the field is never masked), no
    // "0-9: switch" (digits type as text inside the editor; switching
    // slots requires ESC back to MEMORY_PICK). Two compact lines fit
    // between the box bottom and the screen edge on the 135 px LCD.
    M5.Display.setFont(nullptr);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7384);
    M5.Display.setCursor(0, MAIN_Y + 76);
    M5.Display.print("ENTER ok  ,/:cur  OPT caps");
    M5.Display.setCursor(0, MAIN_Y + 92);
    M5.Display.print("ESC bk");
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
    Log::debug("[CD] renderScrollingText: len=%zu head=%zu start=%zu maxChars=%d -> \"%s\"",
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