#include "display_task.h"
#include "display_model.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static DisplayInterface* s_display = nullptr;
static TaskHandle_t s_handle = nullptr;

// ISR-safe flag: morse key ISR sets this to wake the display from screen-saver.
// The display task polls this every tick and resets it after handling.
static volatile bool s_wakeRequested = false;

void DisplayTask::wakeFromScreensaver() {
    s_wakeRequested = true;
}

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

            while (true) {
                auto& m = MorseModel::instance();
                bool displayActive = m.isDisplayActive();
                uint32_t cur = m.changeCounter();

                // Screen-saver: turn off display after DISPLAY_TIMEOUT_MS of inactivity
                if (displayActive) {
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
                    Serial.printf("[DSP] *** RENDER t=%u counter=%u active=%d wakeArmed=%d ***\n",
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

                // Overlay auto-timeout
                if (m.screen() != DisplayScreen::DECODER) {
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
        5,
        &s_handle,
        0
    );
}

void DisplayTask::requestRender() {
    MorseModel::instance().touch();
    MorseModel::instance().incrementChangeCounter();
}