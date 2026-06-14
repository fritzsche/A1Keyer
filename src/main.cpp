/**
 * main.cpp - A1Keyer firmware (M5Stack Morse Trainer)
 *
 * Supports: Tab5 (320×480, capacitive touch, ES8388+PI4IOE5V6408)
 *           Cardputer (240×135, keyboard + Button A, NS4168 I2S amp)
 * Build with: pio run -e esp32p4_pioarduino  (Tab5)
 *              pio run -e esp32s3_cardputer   (Cardputer)
 */
#define A1KEYER_VERSION "0.1.0"
#include <M5Unified.h>
#include <Preferences.h>
#include "audio_engine.h"
#include "morse_generator.h"
#include "morse_encoder.h"
#include "morse_key.h"
#include "morse_decoder.h"
#include "morse_constants.h"
#include "ui_layout.h"
#include "Log.h"
#include "display_model.h"
#include "display_task.h"
#ifdef BOARD_CARDPUTER
#include "cardputer_display.h"
#endif

#ifdef BOARD_CARDPUTER
#include <M5Cardputer.h>
#endif

// ---------------------------------------------------------------------------
// Keyboard handling — drives MorseModel state
// ---------------------------------------------------------------------------
static void handleKeyboard() {
#ifdef BOARD_CARDPUTER
    auto& kb = M5Cardputer.Keyboard;
    kb.updateKeyList();

    // Wake screen-saver on any keyboard activity and reset inactivity timer
    if (kb.keyList().size() > 0) {
        DisplayTask::wakeFromScreensaver();
        MorseModel::instance().touch();
    }

    static int dbg = 0;
    if (kb.keyList().size() > 0 && (++dbg % 50 == 0)) {
        Serial.printf("[KB] keys: ");
        for (auto& k : kb.keyList()) {
            char c = kb.getKey(k);
            Serial.printf("%c(0x%02X) ", c >= 32 ? c : '?', (unsigned char)c);
        }
        Serial.println();
    }

    static bool wasW = false, wasF = false, wasP = false, wasV = false, wasM = false;
    static bool wasEnter = false, wasShift = false;
    static bool wasBtnA = false;
    static bool wasSemicolon = false, wasPeriod = false;

    bool wKey       = kb.isKeyPressed('W') || kb.isKeyPressed('w');
    bool fKey       = kb.isKeyPressed('F') || kb.isKeyPressed('f');
    bool pKey       = kb.isKeyPressed('P') || kb.isKeyPressed('p');
    bool vKey       = kb.isKeyPressed('V') || kb.isKeyPressed('v');
    bool mKey       = kb.isKeyPressed('M') || kb.isKeyPressed('m');
    bool enter      = kb.isKeyPressed(KEY_ENTER);
    bool shift      = kb.keysState().shift;
    bool btnA       = M5Cardputer.BtnA.isPressed();
    bool semicolon  = kb.isKeyPressed(';');
    bool period     = kb.isKeyPressed('.');

    auto& model = MorseModel::instance();

    // W → WPM settings (toggle).
    if (wKey && !wasW) {
        Serial.println("[KB] W pressed");
        if (model.screen() == DisplayScreen::WPM_SETTINGS) {
            model.setScreen(DisplayScreen::DECODER);
        } else {
            model.setScreen(DisplayScreen::WPM_SETTINGS);
            model.setOverlayStartMillis(millis());
        }
        DisplayTask::requestRender();
    }

    // F → frequency settings (toggle).
    if (fKey && !wasF) {
        if (model.screen() == DisplayScreen::FREQ_SETTINGS) {
            model.setScreen(DisplayScreen::DECODER);
        } else {
            model.setScreen(DisplayScreen::FREQ_SETTINGS);
            model.setOverlayStartMillis(millis());
        }
        DisplayTask::requestRender();
    }

    // V → volume settings (toggle).
    if (vKey && !wasV) {
        if (model.screen() == DisplayScreen::VOLUME_SETTINGS) {
            model.setScreen(DisplayScreen::DECODER);
        } else {
            model.setScreen(DisplayScreen::VOLUME_SETTINGS);
            model.setOverlayStartMillis(millis());
        }
        DisplayTask::requestRender();
    }

    // M → mode settings (toggle: Paddle / Straight).
    if (mKey && !wasM) {
        if (model.screen() == DisplayScreen::MODE_SETTINGS) {
            model.setScreen(DisplayScreen::DECODER);
        } else {
            model.setScreen(DisplayScreen::MODE_SETTINGS);
            model.setOverlayStartMillis(millis());
        }
        DisplayTask::requestRender();
    }

    // P → start Morse encoder playback.
    if (pKey && !wasP) {
        auto gen = AudioEngine::morseGen();
        if (gen && !gen->isPlaying()) {
            gen->playText("Hello Morse!");
            model.setMode(KeyerMode::ENCODER);
            DisplayTask::requestRender();
        }
    } else if (!pKey && wasP && model.mode() == KeyerMode::ENCODER) {
        // P released and was in ENCODER mode — switch back if playback also finished
        auto gen = AudioEngine::morseGen();
        if (!gen || !gen->isPlaying()) {
            model.setMode(KeyerMode::KEYER);
            DisplayTask::requestRender();
        }
    }
    // Also check every loop: if in ENCODER mode but playback finished, switch back.
    // This catches the case where P is held or released after playback already ended.
    if (model.mode() == KeyerMode::ENCODER) {
        auto gen = AudioEngine::morseGen();
        if (!gen || !gen->isPlaying()) {
            model.setMode(KeyerMode::KEYER);
            DisplayTask::requestRender();
        }
    }
    wasP = pKey;

    // In WPM settings: ; = +1, . = -1
    if (model.screen() == DisplayScreen::WPM_SETTINGS) {
        if (semicolon && !wasSemicolon) { model.adjustWPM(+1);  DisplayTask::requestRender(); }
        if (period    && !wasPeriod)    { model.adjustWPM(-1);  DisplayTask::requestRender(); }
        model.setOverlayStartMillis(millis());
    }
    // In WPM overlay: Shift+W = +1, Shift+F = -1
    else if (model.screen() == DisplayScreen::WPM_VIEW) {
        if (wKey && shift && !wasW) { model.adjustWPM(+1);  DisplayTask::requestRender(); }
        if (fKey && shift && !wasF) { model.adjustWPM(-1);  DisplayTask::requestRender(); }
        model.setOverlayStartMillis(millis());
    }
    // In FREQ settings: ; = +10Hz, . = -10Hz
    else if (model.screen() == DisplayScreen::FREQ_SETTINGS) {
        if (semicolon && !wasSemicolon) { model.adjustFrequency(+10.0f); DisplayTask::requestRender(); }
        if (period    && !wasPeriod)    { model.adjustFrequency(-10.0f); DisplayTask::requestRender(); }
        model.setOverlayStartMillis(millis());
    }
    // In VOLUME settings: ; = +10, . = -10
    else if (model.screen() == DisplayScreen::VOLUME_SETTINGS) {
        if (semicolon && !wasSemicolon) { model.adjustVolume(+10);  DisplayTask::requestRender(); }
        if (period    && !wasPeriod)     { model.adjustVolume(-10);  DisplayTask::requestRender(); }
        model.setOverlayStartMillis(millis());
    }
    // In MODE settings: ; = Paddle, . = Straight
    else if (model.screen() == DisplayScreen::MODE_SETTINGS) {
        if (semicolon && !wasSemicolon) { model.setKeyerType(KeyerType::PADDLE);  DisplayTask::requestRender(); }
        if (period    && !wasPeriod)     { model.setKeyerType(KeyerType::STRAIGHT); DisplayTask::requestRender(); }
        model.setOverlayStartMillis(millis());
        // Clear ISR memory and keyer state on type switch to avoid stale state
        MorseKey::clearMemory();
        if (auto sk = AudioEngine::straightKeyer()) sk->reset();
    }

    // Enter: dismiss overlay and return to DECODER. Save settings if in settings screens.
    if (enter && !wasEnter) {
        if (model.screen() == DisplayScreen::WPM_SETTINGS) {
            Preferences prefs;
            prefs.begin("morse", false);  // read-write
            prefs.putInt("wpm", model.wpm());
            prefs.end();
            Serial.printf("[KB] saved WPM=%d to preferences\n", model.wpm());
        }
        if (model.screen() == DisplayScreen::FREQ_SETTINGS) {
            Preferences prefs;
            prefs.begin("morse", false);  // read-write
            prefs.putInt("freq", (int)model.frequency());
            prefs.end();
            Serial.printf("[KB] saved freq=%d to preferences\n", (int)model.frequency());
        }
        if (model.screen() == DisplayScreen::VOLUME_SETTINGS) {
            Preferences prefs;
            prefs.begin("morse", false);  // read-write
            prefs.putInt("vol", model.volume());
            prefs.end();
            Serial.printf("[KB] saved vol=%d to preferences\n", model.volume());
        }
        if (model.screen() == DisplayScreen::MODE_SETTINGS) {
            Preferences prefs;
            prefs.begin("morse", false);  // read-write
            prefs.putString("keytype", model.keyerType() == KeyerType::PADDLE ? "paddle" : "straight");
            prefs.end();
            Serial.printf("[KB] saved keyerType=%s\n", model.keyerType() == KeyerType::PADDLE ? "paddle" : "straight");
        }
        if (model.screen() != DisplayScreen::DECODER) {
            model.setScreen(DisplayScreen::DECODER);
            DisplayTask::requestRender();
        }
    }
    wasEnter = enter;

    // Button A: same as Enter.
    if (btnA && !wasBtnA) {
        if (model.screen() != DisplayScreen::DECODER) {
            model.setScreen(DisplayScreen::DECODER);
            DisplayTask::requestRender();
        }
    }
    wasBtnA = btnA;

    wasW = wKey; wasF = fKey; wasV = vKey; wasM = mKey; wasShift = shift;
    wasSemicolon = semicolon; wasPeriod = period;
#else
    (void)0;
#endif
}

void setup() {
    Serial.begin(115200);
    delay(500);

    auto cfg = M5.config();
    cfg.internal_spk = false;
    M5.begin(cfg);

#ifdef BOARD_CARDPUTER
    Serial.printf("[setup] M5.getBoard() = %d\n", (int)M5.getBoard());
    M5Cardputer.begin(true);
#endif

    delay(500);
    Serial.println("=== Morse Trainer ===");

    if (!AudioEngine::begin()) {
        Log::error("FATAL: AudioEngine::begin failed");
        while (true) delay(1000);
    }
    AudioEngine::createMorseGen();

    // Paddle key input (GPIO interrupts)
    MorseKey::begin();

    // Decoder: wire both keyers' ring buffers to MorseDecoder
    MorseDecoder::begin(AudioEngine::keyer(), AudioEngine::straightKeyer());

    // Initialize display system
#ifdef BOARD_CARDPUTER
    DisplayTask::begin(new CardputerDisplay());
#else
    DisplayTask::begin(nullptr);
#endif

    // Load persisted settings from EEPROM (Preferences)
    {
        auto& model = MorseModel::instance();
        Preferences prefs;
        prefs.begin("morse", true);  // read-only
        int savedWpm = prefs.getInt("wpm", 20);
        int savedFreq = prefs.getInt("freq", 600);
        int savedVol = prefs.getInt("vol", 50);
        String savedKeyType = prefs.getString("keytype", "paddle");
        prefs.end();
        model.setWPM(savedWpm);
        model.setFrequency((float)savedFreq);
        model.setVolume(savedVol);
        model.setKeyerType(savedKeyType == "straight" ? KeyerType::STRAIGHT : KeyerType::PADDLE);
        Serial.printf("[setup] loaded WPM=%d freq=%d vol=%d keytype=%s from preferences\n",
            savedWpm, savedFreq, savedVol, savedKeyType.c_str());
    }

    Log::info("A1Keyer v%s", A1KEYER_VERSION);
    Log::info("Ready.");
}

void loop() {
    M5.update();
    handleKeyboard();

#ifdef BOARD_CARDPUTER
    // Update headphone state
    {
        static bool lastHpState = false;
        static bool pendingHp = false;
        static int pendingCount = 0;
        bool hp = AudioEngine::isHeadphoneInserted();
        if (hp != pendingHp) {
            pendingHp = hp;
            pendingCount = 0;
        } else if (++pendingCount >= 5 && hp != lastHpState) {
            lastHpState = hp;
            AudioEngine::setHeadphoneMode(hp);
        }
    }
#endif

    // Poll decoder ring buffer, decode, and feed to model
    static char decodeBuffer[16] = { 0 };
    static size_t decodePos = 0;

    // Decode straight key directly — reads KeyEvent ring buffer,
    // classifies dit/dah, detects gaps, outputs to MorseModel.
    // No involvement of MorseDecoder for straight key mode.
    if (MorseModel::instance().keyerType() == KeyerType::STRAIGHT) {
        AudioEngine::straightKeyer()->decodeFromLoop(AUDIO_SAMPLE_RATE);
    } else {
        // Iambic/keyer path — MorseDecoder consumes IambicKeyer's ring buffer
        char sym;
        while (MorseDecoder::read(&sym)) {
            MorseDecoder::accumulate(sym, decodeBuffer, sizeof(decodeBuffer), &decodePos);
            if (sym == MorseDecoder::SPACE_CHAR) {
                decodePos = 0;
            }
        }
    }
}