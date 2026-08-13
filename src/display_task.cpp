#include "display_task.h"
#include "display_model.h"
#include "Log.h"
#ifndef UNIT_TEST
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

static DisplayInterface* s_display = nullptr;
#ifndef UNIT_TEST
static TaskHandle_t s_handle = nullptr;
#endif

// ISR-safe flag: morse key ISR sets this to wake the display from screen-saver.
// The display task polls this every tick and resets it after handling.
static volatile bool s_wakeRequested = false;

void DisplayTask::wakeFromScreensaver() {
    s_wakeRequested = true;
}

bool DisplayTask::consumeWakeRequest() {
    if (s_wakeRequested) {
        s_wakeRequested = false;
        return true;
    }
    return false;
}

#ifndef UNIT_TEST
void DisplayTask::begin(DisplayInterface* display) {
    s_display = display;
    display->init();

    // Initialize activity timer so screen-saver doesn't fire immediately
    MorseModel::instance().touch();

    xTaskCreatePinnedToCore(
        [](void* param) {
            auto* disp = reinterpret_cast<DisplayInterface*>(param);
            uint32_t lastCounter = 0;
            bool lastDisplayActive = true;
            bool wasDisplayActive = true;  // track prior state to avoid redundant powerOn/Off
            bool wakeArmed = false;         // arm after power-on so next tick doesn't re-wake

            // Always render once at startup so the screen is lit immediately
            disp->render();

            // Yield once at startup so loop() on Core 0 gets a chance to
            // run before this task claims a slot. Without this the first
            // delay(50) call can land before loop() has executed at all,
            // and the Wi-Fi init / first keyboard read happens a frame late.
            vTaskDelay(pdMS_TO_TICKS(1));

            while (true) {
                auto& m = MorseModel::instance();
                bool displayActive = m.isDisplayActive();
                uint32_t cur = m.changeCounter();

                // Screen-saver: turn off display after DISPLAY_TIMEOUT_MS of inactivity.
                // The Wi-Fi screens are exempted — a half-typed passphrase or
                // a status screen the user is reading must not blank out.
                const bool wifiUiScreen =
                    m.screen() == DisplayScreen::WIFI_SCAN_LIST ||
                    m.screen() == DisplayScreen::WIFI_PASSWORD_INPUT ||
                    m.screen() == DisplayScreen::WIFI_NETWORK_INFO;

                if (displayActive && !wifiUiScreen) {
                    if (millis() - m.lastActivity() >= MorseModel::DISPLAY_TIMEOUT_MS) {
                        m.setDisplayActive(false);
                        displayActive = false;
                        wakeArmed = false;
                    }
                }

                // Transition-based power control — only call powerOn/Off on actual changes
                if (displayActive && !wasDisplayActive) {
                    disp->powerOn();
                    wakeArmed = true;  // arm: don't re-wake until screen goes off again
                } else if (!displayActive && wasDisplayActive) {
                    disp->powerOff();
                }

                // Wake from screen-saver: morse key ISR sets s_wakeRequested flag
                if (s_wakeRequested) {
                    s_wakeRequested = false;
                    m.touch(); // always reset timer on morse key activity
                    if (!m.isDisplayActive()) {
                        m.setDisplayActive(true);
                        wakeArmed = true;
                    }
                }

                // If screen just went inactive due to screen-saver timeout (line 44),
                // DON'T re-wake on every audio-thread counter bump. Only wake via
                // s_wakeRequested (paddle interrupt) or explicit requestRender() call.
                // The counter changes 20+ times/second from keyerPatternPercent updates
                // — those must NOT re-enable a screen we just turned off.
                wasDisplayActive = displayActive;

                // Only render when display is physically active. When screen-saver
                // blanks the backlight, stop ALL renders — no point wasting CPU and
                // flooding the serial log. lastCounter is kept in sync only while
                // display is active so the next state change after wake-up triggers
                // exactly one render.
                if (displayActive && (cur != lastCounter || !lastDisplayActive)) {
                    uint32_t now = millis();
                    Log::debug("[DSP] *** RENDER t=%u counter=%u active=%d wakeArmed=%d ***",
                        now, cur, (int)displayActive, (int)wakeArmed);
                    lastCounter = cur;
                    disp->render();
                    lastDisplayActive = true;
                } else {
                    if (displayActive) {
                        lastCounter = cur;  // display on but no change — keep in sync
                    } else {
                        // Display off — don't render at all. Sync counter so wake-up
                        // from screen-saver sees a real delta and triggers ONE render.
                        // Skip the render call entirely to eliminate all serial spam.
                    }
                }

                // Overlay auto-timeout. Excludes the three Wi-Fi screens:
                // a long passphrase or a slow scan would otherwise dismiss
                // the screen mid-flow even though handleWifiScreen() refreshes
                // the timer on every tick (a race between the display task
                // reading overlayStartMillis and the main loop writing it can
                // still let one tick fall past the threshold). The Wi-Fi
                // screens are full-screen forms, not transient overlays, and
                // they have their own explicit dismiss (Esc / Enter / X).
                const bool isWifiScreen =
                    m.screen() == DisplayScreen::WIFI_SCAN_LIST ||
                    m.screen() == DisplayScreen::WIFI_PASSWORD_INPUT ||
                    m.screen() == DisplayScreen::WIFI_NETWORK_INFO;
                if (m.screen() != DisplayScreen::DECODER && !isWifiScreen) {
                    if (millis() - m.overlayStartMillis() >= MorseModel::OVERLAY_TIMEOUT_MS) {
                        m.setScreen(DisplayScreen::DECODER);
                        m.incrementChangeCounter();
                    }
                }

                delay(50);
            }
        },
        "display",
        4096,
        display,
        // Priority 1 (same as the Arduino loopTask on Core 0). The display
        // task used to sit at priority 5 on Core 0, which let a single
        // render preempt loop() for the entire duration of an SPI write
        // burst — and with the password-input screen rendering the full
        // masked buffer on every keystroke, those bursts could add up to
        // 20+ seconds of loop starvation. Pinning to Core 1 puts the
        // display renderer on the same core as the audio task (which is
        // already at priority 22 and so wins every arbitration) and keeps
        // it off Core 0 entirely. A Core 0 with nothing to compete with
        // the keyboard handler responds to TCA8418 interrupts
        // immediately.
        1,
        &s_handle,
        1
    );
}

void DisplayTask::requestRender() {
    MorseModel::instance().touch();
    MorseModel::instance().incrementChangeCounter();
}
#endif  // !UNIT_TEST