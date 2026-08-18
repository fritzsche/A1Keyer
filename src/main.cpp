/**
 * main.cpp - A1Keyer firmware (M5Stack Morse Trainer)
 *
 * Supports: Tab5 (320×480, capacitive touch, ES8388+PI4IOE5V6408)
 *           Cardputer (240×135, keyboard + Button A, NS4168 I2S amp)
 * Build with: pio run -e esp32p4_pioarduino  (Tab5)
 *              pio run -e esp32s3_cardputer   (Cardputer)
 */
#define A1KEYER_VERSION "0.4.0"
#include <M5Unified.h>
#include <Preferences.h>
#include <driver/gpio.h>
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
#include "key_event_bus.h"
#include "radio_keyer.h"
#include "winkey.h"
#include "console_io.h"
#include "network_manager.h"
#include "text_input.h"
#include "memory_store.h"
#if ENABLE_WIFI_DEBUG
#include <WiFi.h>
#include "wifi_debug.h"
#include "http_server.h"
#include "console_server.h"
#include "web_ui.h"
#endif
#ifdef BOARD_CARDPUTER
#include "cardputer_display.h"
#endif

#ifdef BOARD_CARDPUTER
#include <M5Cardputer.h>
#endif

// ---------------------------------------------------------------------------
// pollKeys — sample the Cardputer keyboard once into a CardputerKeyState.
//
// Reads modifier flags, special keys (Enter, Backspace, Esc) and the first
// printable held. Returns an empty state on non-Cardputer boards and when
// no key is down. Callers (handleKeyboard, the wifi screen handlers) feed
// the result to TextInput::feed() for typed text, or read its fields
// directly for navigation.
//
// Esc is the Fn-layer backspace position (see M5Cardputer Keyboard.h:
// `_keys_state_buffer.esc` is set only when Fn is held). That is the
// natural "cancel" gesture on this keyboard.
// ---------------------------------------------------------------------------
static CardputerKeyState pollKeys() {
    CardputerKeyState ks{};
#ifdef BOARD_CARDPUTER
    auto& kb = M5Cardputer.Keyboard;
    if (kb.keyList().empty()) return ks;

    const auto& st = kb.keysState();
    ks.anyKey    = true;
    ks.shift     = st.shift;
    ks.fn        = st.fn;
    ks.opt       = st.opt;
    ks.enter     = kb.isKeyPressed(KEY_ENTER);
    ks.backspace = st.backspace;
    ks.escape    = st.esc;
    // Fn-layer arrows. The keyboard library populates these on the
    // fn-layer pass (Keyboard.cpp PASS 2) and returns early before
    // PASS 3, so `word` is empty whenever `fn` is held — that means
    // ks.printable can NEVER be `,` or `/` while Fn is down, and the
    // Fn-gated printable detection in handleWifiScreen /
    // handleMemoryScreen was structurally dead. Read the native
    // fn-layer flags directly: left = Fn + ,, right = Fn + /.
    ks.left      = st.left;
    ks.right     = st.right;

    // Use the library's own `word` buffer (already filtered for case by
    // the keyboard reader — see Keyboard.cpp PASS 3 which pushes the
    // value_second / value_first into `word` only for non-special,
    // non-modifier keys). Iterating `kb.keyList()` and calling
    // `getKey()` instead would surface KEY_BACKSPACE (0x2a = '*') as a
    // printable, because 0x2a sits inside the 0x20..0x7F ASCII range.
    // The library's `word` skips it.
    for (char c : st.word) {
        if (c >= 0x20 && c < 0x7F) {
            ks.printable = c;
            break;
        }
    }
#endif
    return ks;
}

// ---------------------------------------------------------------------------
// handleWifiScreen — per-screen keyboard routing for the three Wi-Fi screens.
//
// Called from handleKeyboard() AFTER the global W/F/V/M/K/D/C/N handlers.
// The universal Enter-dismisses-overlay block at the bottom of
// handleKeyboard() skips its work when the screen is one of the three
// wifi screens, so the Enter pressed here reaches its destination.
// ---------------------------------------------------------------------------
static void handleWifiScreen(MorseModel& model, const CardputerKeyState& ks) {
    const DisplayScreen sc = model.screen();

    // Refresh the overlay timer for every wifi-screen tick. Without
    // this, OVERLAY_TIMEOUT_MS (10 s) auto-dismisses the screen back
    // to DECODER even while the user is mid-typing or browsing a
    // scan list. The same responsibility is performed per-keypress in
    // handleKeyboard() for the WPM/FREQ/VOLUME/MODE/KEYING overlays;
    // doing it here once covers all three wifi screens.
    model.setOverlayStartMillis(millis());

    // Track the screen we processed last tick. Each screen-local
    // block below holds its own edge-detection statics that span
    // ticks; when we transition between screens, those statics
    // belong to whatever screen last occupied the foreground and
    // may be stale for the new one. Priming each static to the
    // current key state on transition means a key the user is still
    // holding through a screen change (e.g. Enter to commit the
    // password, then the next tick the WIFI_NETWORK_INFO handler
    // runs) is treated as "already seen" instead of a fresh edge —
    // which would otherwise dismiss the screen and cancel the
    // in-flight connection the user just initiated.
    static DisplayScreen s_lastSc = DisplayScreen::DECODER;
    const bool screenJustChanged = (sc != s_lastSc);
    s_lastSc = sc;

    if (sc == DisplayScreen::WIFI_SCAN_LIST) {
        static bool wasSemi = false, wasPeriod = false, wasEnter = false, wasEsc = false;
        const bool semi   = ks.printable == ';';
        const bool period = ks.printable == '.';
        const bool enter  = ks.enter;
        const bool esc    = ks.escape;
        if (screenJustChanged) {
            wasSemi = semi; wasPeriod = period; wasEnter = enter; wasEsc = esc;
        }

        // ; = up arrow (Cardputer convention) → move towards smaller index
        // . = down arrow                          → move towards larger index
        if (semi   && !wasSemi)   model.wifiAdjustScanCursor(-1);
        if (period && !wasPeriod) model.wifiAdjustScanCursor(+1);
        if (semi || period) {
            // The cursor moved in the model; the display task must
            // repaint so the user actually sees the new selection.
            DisplayTask::requestRender();
        }

        if (enter && !wasEnter) {
            const int idx = model.wifiScanCursor();
            const NetScanEntry* e = WifiMgr::scanEntry(idx);
            if (e && e->open) {
                WifiMgr::connect(idx, "");
                model.setScreen(DisplayScreen::WIFI_NETWORK_INFO);
            } else if (e) {
                model.passwordInput()->clear();
                // Enter opened this screen and is still being held. Without
                // priming, the next tick's TextInput::feed() would see
                // enterEdge=true on an empty buffer and commit a blank
                // password — which kicks WifiMgr::connect(idx, "") and
                // pops the user straight into the CONNECTING state without
                // ever showing the password entry screen.
                model.passwordInput()->primeEnterHeld();
                model.setScreen(DisplayScreen::WIFI_PASSWORD_INPUT);
            }
            DisplayTask::requestRender();
        }
        if (esc && !wasEsc) {
            WifiMgr::cancel();
            model.wifiResetUIState();
            model.setScreen(DisplayScreen::DECODER);
            DisplayTask::requestRender();
        }
        wasSemi = semi; wasPeriod = period; wasEnter = enter; wasEsc = esc;
        return;
    }

    if (sc == DisplayScreen::WIFI_PASSWORD_INPUT) {
        // OPT key (bottom row, second from left) toggles caps lock.
        // The Cardputer library's `capslocked()` flag makes `getKey()`
        // return value_second for every letter while held, so once
        // locked the user can type a run of capitals hands-free. OPT
        // is a single-keystroke gesture (unlike double-tap Shift, which
        // is fragile in practice — the user has to release Shift between
        // taps, and the keyboard matrix scans fast enough that two quick
        // taps can land in the same tick). OPT was previously unused.
        // Holding Shift+letter still capitalises one char (existing
        // behaviour through st.shift / st.word).
        static bool wasOpt = false;
        if (screenJustChanged) {
            // Prime OPT (and the cursor-nav statics below) to the
            // current key state so a key the user is still holding
            // through the scan-list → password-input transition does
            // not flip caps lock on entry. TextInput's own
            // primeEnterHeld() call (in the scan-list handler) covers
            // Enter; OPT is handled here.
            wasOpt = ks.opt;
        }
        const bool optEdge = ks.opt && !wasOpt;
        wasOpt = ks.opt;
        if (optEdge) {
#ifdef BOARD_CARDPUTER
            M5Cardputer.Keyboard.setCapsLocked(
                !M5Cardputer.Keyboard.capslocked());
#endif
            DisplayTask::requestRender();
        }

        // Fn-layer cursor navigation. The Cardputer keyboard library
        // populates `keysState().left` on Fn + , and `.right` on Fn + /
// (Keyboard.cpp PASS 2). Because the library returns early on the
// fn-layer, `word` is empty whenever Fn is held — meaning bare ',' and
// '/' (without Fn) are always typed as ordinary text. CW macros
// routinely contain '/' and bare ',' is a legal Morse element, so
// silently swallowing them would corrupt the operator's contest
// exchange. The printable is zeroed on the cursor-moved path so
// feed() does not also insert the punctuation into the buffer.
        static bool wasLeft = false, wasRight = false;
        TextInput* ti = model.passwordInput();
        if (screenJustChanged) {
            wasLeft = ks.left; wasRight = ks.right;
        }
        bool cursorMoved = false;
        if (ks.left  && !wasLeft)  { ti->moveCursor(-1); cursorMoved = true; }
        if (ks.right && !wasRight) { ti->moveCursor(+1); cursorMoved = true; }
        wasLeft  = ks.left;
        wasRight = ks.right;

        CardputerKeyState ksForEditor = ks;
        if (cursorMoved) ksForEditor.printable = 0;

        // TextInput does its own edge detection against the previous tick;
        // we just hand it the (possibly filtered) key state.
        const TextInput::Result r = ti->feed(ksForEditor);
        if (r == TextInput::Result::ENTER) {
            const int idx = model.wifiScanCursor();
            WifiMgr::connect(idx, ti->value());
            model.setScreen(DisplayScreen::WIFI_NETWORK_INFO);
            model.wifiClearPassword();
            DisplayTask::requestRender();
        } else if (r == TextInput::Result::ESC) {
            model.wifiClearPassword();
            model.setScreen(DisplayScreen::WIFI_SCAN_LIST);
            DisplayTask::requestRender();
        } else if (cursorMoved || r == TextInput::Result::CHANGED) {
            // Buffer (or reveal flag, or cursor) moved; reflect it on
            // screen. IDLE is intentionally ignored — repainting on
            // every tick at 20 Hz is what made the password screen
            // flicker. The overlay timer itself is refreshed at the
            // top of handleWifiScreen(), so a half-typed passphrase
            // is not auto-dismissed.
            DisplayTask::requestRender();
        }
        return;
    }

    if (sc == DisplayScreen::WIFI_NETWORK_INFO) {
        static bool wasX = false, wasR = false, wasEnter = false, wasEsc = false;
        const bool xKey  = ks.printable == 'x' || ks.printable == 'X';
        const bool rKey  = ks.printable == 'r' || ks.printable == 'R';
        const bool enter = ks.enter;
        const bool esc   = ks.escape;
        if (screenJustChanged) {
            // CRITICAL: prime Enter. The user just committed a
            // password with an Enter press that they are very likely
            // still holding — the natural gesture is to tap-and-hold,
            // not tap-and-release-and-press-again. Without priming,
            // the next tick the Enter handler would treat the still-
            // held Enter as a fresh edge, cancel the in-flight
            // connection (WifiMgr::state() == CONNECTING), and snap
            // the screen back to DECODER. The user would see the
            // connecting state for one tick at most before the device
            // silently bailed out of the wifi flow.
            wasX = xKey; wasR = rKey; wasEnter = enter; wasEsc = esc;
        }

        if (xKey && !wasX) {
            WifiMgr::disconnectAndForget();
            DisplayTask::requestRender();
        }
        if (rKey && !wasR) {
            WifiMgr::retry();
            DisplayTask::requestRender();
        }
        // Enter / Esc leave the screen. NETWORK_INFO is not an input
        // field, so unlike WIFI_PASSWORD_INPUT the universal
        // Enter-dismisses-overlay block is allowed to fire here — but
        // we still gate that block on `!onWifiScreen` for the other
        // two wifi screens, so handle dismissal explicitly. Enter while
        // CONNECTING also aborts the in-flight attempt via cancel().
        if (enter && !wasEnter) {
            if (WifiMgr::state() == NetState::CONNECTING) {
                WifiMgr::cancel();
            }
            model.setScreen(DisplayScreen::DECODER);
            DisplayTask::requestRender();
        }
        if (esc && !wasEsc) {
            if (WifiMgr::state() == NetState::CONNECTING) {
                WifiMgr::cancel();
            }
            model.setScreen(DisplayScreen::DECODER);
            DisplayTask::requestRender();
        }
        wasX = xKey; wasR = rKey; wasEnter = enter; wasEsc = esc;
        return;
    }
}

// ---------------------------------------------------------------------------
// mirrorWifiState — copy the volatile bits of NetworkManager into MorseModel
// once per loop tick, so the display task can render purely from the model.
//
// ChangeCounter-based setters no-op when the value is unchanged, so this is
// cheap on idle ticks.
// ---------------------------------------------------------------------------
static void mirrorWifiState(MorseModel& model) {
    model.setWifiState((int)WifiMgr::state());
    model.setWifiLocalIP(WifiMgr::localIP());
    model.setWifiHasCredentials(WifiMgr::hasSavedCredentials());
    model.setWifiCredSource((int)WifiMgr::credentialSource());
    model.setWifiSecondsUntilRetry(WifiMgr::secondsUntilRetry());
    model.setWifiScanCount(WifiMgr::scanCount());
}

// ---------------------------------------------------------------------------
// handleMemoryScreen — per-screen keyboard routing for MEMORY_PICK and
// MEMORY_EDIT.
//
// Called from handleKeyboard() AFTER the global W/F/V/M/K/D/C/N handlers
// but BEFORE the universal Enter-dismisses-overlay block — MEMORY_PICK
// and MEMORY_EDIT are exempt from the universal block (see onMemoryScreen
// at the bottom of handleKeyboard), so the Enter pressed inside the editor
// reaches TextInput::feed() and fires its ENTER result.
//
// Picker (MEMORY_PICK): accept a single digit 0-9 → transition to the
// editor pre-filled with the slot's persisted text. ESC → DECODER.
//
// Editor (MEMORY_EDIT): OPT caps lock (mirrors the wifi password screen),
// ',' / '/' cursor nav (mirrors the wifi password screen), ENTER commits
// the buffer to the slot AND saves the bank to NVS, ESC discards. A digit
// 0-9 inside the editor pivots to a different slot — useful when the
// operator opened memory 3 then realised they meant memory 5.
// ---------------------------------------------------------------------------
static void handleMemoryScreen(MorseModel& model, const CardputerKeyState& ks) {
    const DisplayScreen sc = model.screen();

    // Keep the overlay alive while the operator types — otherwise
    // OVERLAY_TIMEOUT_MS would auto-dismiss the screen back to DECODER
    // mid-edit (same responsibility as handleWifiScreen for the wifi
    // screens).
    model.setOverlayStartMillis(millis());

    static DisplayScreen s_lastSc = DisplayScreen::DECODER;
    const bool screenJustChanged = (sc != s_lastSc);
    s_lastSc = sc;

    if (sc == DisplayScreen::MEMORY_PICK) {
        // Track each digit 0-9 separately so the picker can fire on a
        // rising edge of any one of them. A fresh screen primes the
        // digits to the current key state so a key the operator is
        // still holding across a screen change (rare, but possible
        // coming back from MEMORY_EDIT via ESC) is not double-counted.
        static bool wasDigit[10] = {false,false,false,false,false,
                                    false,false,false,false,false};
        static bool wasEsc = false;
        // Enter primes identically to Esc — so a held Enter that
        // occurred just before M-press does not auto-commit.
        static bool wasEnter = false;
        // x/X primes to the current printable-held state so a key the
        // operator was still holding across M-entry does not double-fire.
        static bool wasX = false;

        int digit = -1;
        if (ks.printable >= '0' && ks.printable <= '9') digit = ks.printable - '0';
        const bool xKey = (ks.printable == 'x' || ks.printable == 'X');

        if (screenJustChanged) {
            for (int i = 0; i < 10; ++i) wasDigit[i] = (i == digit);
            wasEsc = ks.escape;
            wasEnter = ks.enter;
            wasX = xKey;
        }

        // Digit → switch the slot the picker is showing. We DO NOT
        // transition to MEMORY_EDIT here — that happens on Enter. The
        // currently shown slot lives in `_memoryPickSlot`, which the
        // renderer (showMemoryPick) reads each frame.
        if (digit >= 0 && !wasDigit[digit]) {
            model.setMemoryPickSlot(digit);
            DisplayTask::requestRender();
        }

        // Esc → cancel picker, back to DECODER.
        if (ks.escape && !wasEsc) {
            model.setScreen(DisplayScreen::DECODER);
            DisplayTask::requestRender();
        }

        // x/X → clear the currently shown slot and persist. Edge-triggered
        // so holding the key does not keep wiping. No confirmation prompt:
        // the operator asked for it explicitly and the picker immediately
        // re-renders the red "(empty)" placeholder, which is the visible
        // feedback. Stays on MEMORY_PICK so the operator can immediately
        // press Enter to type fresh content into the now-empty slot.
        if (xKey && !wasX) {
            const int cursor = model.memoryPickSlot();
            const uint8_t target = (cursor >= 0 && cursor < (int)kMemSlots)
                                   ? (uint8_t)cursor : 0;
            model.setMemory(target, "");
            MemoryBank bank;
            for (uint8_t i = 0; i < kMemSlots; ++i) {
                memCopyStr(bank.slot[i], kMemLen, model.getMemory(i));
            }
            if (memoryBankSave(bank)) {
                Log::info("[MEM] cleared m%d", target);
            } else {
                Log::error("[MEM] clear FAILED for slot %d", target);
            }
            DisplayTask::requestRender();
        }

        // Enter → commit the currently shown slot into MEMORY_EDIT,
        // pre-filling the TextInput buffer with the slot's persisted
        // text. The "switch then commit" flow is `M → 3 → Enter` to
        // edit slot 3 (or any default slot on first M then Enter).
        //
        // CRITICAL: primePrintableHeld prevents the still-held
        // printable (the Enter-as-printable on the keyboard, or zero)
        // from typing itself into the buffer on the very next tick.
        // setValue() resets _prevPrintable to 0, so without priming
        // the editor's first feed() sees a fresh "edge" on the same
        // key the user was holding — the bug primePrintableHeld was
        // introduced to fix in the original digit-edge path.
        //
        // CRITICAL: primeEnterHeld() is just as important. The Enter
        // the operator just pressed is still physically held when the
        // editor's first feed() runs on the next tick; setValue() resets
        // _prevEnter to 0 too, so without priming the held Enter would
        // immediately commit on entry — the operator would never see
        // MEMORY_EDIT, the screen would jump straight back to DECODER.
        // Mirrors primeEnterHeld() in the wifi password screen.
        //
        // The early `return;` is required: we leave MEMORY_PICK on
        // this tick, so the digit/Esc closing writes below must NOT
        // run (they belong to the PICK state, not EDIT). MEMORY_EDIT
        // does its own screenJustChanged handling on the next tick.
        if (ks.enter && !wasEnter) {
            const int cursor = model.memoryPickSlot();
            const uint8_t target = (cursor >= 0 && cursor < (int)kMemSlots)
                                   ? (uint8_t)cursor : 0;
            model.setMemoryEditingSlot((int)target);
            TextInput* ti = model.memoryInput();
            ti->setValue(model.getMemory(target));
            ti->primePrintableHeld(ks.printable);
            ti->primeEnterHeld();
            model.setScreen(DisplayScreen::MEMORY_EDIT);
            DisplayTask::requestRender();
            return;
        }
        wasEnter = ks.enter;

        for (int i = 0; i < 10; ++i) wasDigit[i] = (i == digit);
        wasEsc = ks.escape;
        wasX = xKey;
        return;
    }

    if (sc == DisplayScreen::MEMORY_EDIT) {
        // OPT → caps lock. Same wiring as WIFI_PASSWORD_INPUT — see the
        // comment block at main.cpp:171-180.
        static bool wasOpt = false;
        if (screenJustChanged) {
            wasOpt = ks.opt;
        }
        const bool optEdge = ks.opt && !wasOpt;
        wasOpt = ks.opt;
        if (optEdge) {
#ifdef BOARD_CARDPUTER
            M5Cardputer.Keyboard.setCapsLocked(
                !M5Cardputer.Keyboard.capslocked());
#endif
            DisplayTask::requestRender();
        }

        // Digits 0-9 are typed as text — no slot pivot inside the
        // editor. CW macros routinely contain numbers ("5NN", "599",
        // contest exchanges) and an accidental pivot would have
        // overwritten the operator's in-progress edits. To switch
        // slots, ESC back to MEMORY_PICK and press a different digit.

        // Fn-layer cursor navigation (mirror wifi password handler at
        // main.cpp:201-220). The Cardputer keyboard library populates
        // `keysState().left` on Fn + , and `.right` on Fn + /
        // (Keyboard.cpp PASS 2). Because the library returns early on
        // the fn-layer, `word` is empty whenever Fn is held — meaning
        // bare ',' and '/' (without Fn) are always typed as ordinary
        // text. CW macros routinely contain '/' and bare ',' is a legal
        // Morse element, so silently swallowing them would corrupt the
        // operator's contest exchange. The printable is zeroed on the
        // cursor-moved path so feed() does not also insert the
        // punctuation into the buffer.
        static bool wasLeft = false, wasRight = false;
        TextInput* ti = model.memoryInput();
        if (screenJustChanged) {
            wasLeft = ks.left; wasRight = ks.right;
        }
        bool cursorMoved = false;
        if (ks.left  && !wasLeft)  { ti->moveCursor(-1); cursorMoved = true; }
        if (ks.right && !wasRight) { ti->moveCursor(+1); cursorMoved = true; }
        wasLeft  = ks.left;
        wasRight = ks.right;

        CardputerKeyState ksForEditor = ks;
        if (cursorMoved) ksForEditor.printable = 0;

        const TextInput::Result r = ti->feed(ksForEditor);
        if (r == TextInput::Result::ENTER) {
            // Commit: copy the editor buffer into the slot, then save
            // the whole bank to NVS. Saving all 10 slots on every commit
            // is fine — Preferences.putString() is a no-op when the
            // value is unchanged, so rows we didn't touch cost nothing.
            const int slot = model.memoryEditingSlot();
            if (slot >= 0 && slot < (int)kMemSlots) {
                const char* newText = ti->value();
                model.setMemory((uint8_t)slot, newText);
                MemoryBank bank;
                for (uint8_t i = 0; i < kMemSlots; ++i) {
                    memCopyStr(bank.slot[i], kMemLen, model.getMemory(i));
                }
                if (memoryBankSave(bank)) {
                    Log::info("[MEM] saved m%d=\"%s\"", slot, newText);
                } else {
                    Log::error("[MEM] save FAILED for slot %d", slot);
                }
            }
            model.setMemoryEditingSlot(-1);
            model.setMemoryPickSlot(-1);
            model.setScreen(DisplayScreen::DECODER);
            DisplayTask::requestRender();
        } else if (r == TextInput::Result::ESC) {
            // Discard: editor buffer is dropped automatically (the
            // TextInput stays bound to it; the next visit will
            // setValue() over it). The persisted slot text is
            // unchanged.
            model.setMemoryEditingSlot(-1);
            model.setMemoryPickSlot(-1);
            model.setScreen(DisplayScreen::DECODER);
            DisplayTask::requestRender();
        } else if (cursorMoved || r == TextInput::Result::CHANGED) {
            DisplayTask::requestRender();
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Keyboard handling — drives MorseModel state
// ---------------------------------------------------------------------------
static void handleKeyboard() {
#ifdef BOARD_CARDPUTER
    auto& kb = M5Cardputer.Keyboard;
    kb.updateKeyList();
    // Populate _keys_state_buffer (st.shift, st.backspace, st.word,
    // etc.). The Cardputer library splits keyboard scanning across two
    // calls: updateKeyList() fills _key_list with the held coordinates,
    // and updateKeysState() walks that list to derive the high-level
    // state we read via keysState() / getKey(). Only M5Cardputer.update()
    // calls both, and our main loop calls M5.update() instead — so we
    // must invoke updateKeysState() ourselves. Without this, st.word is
    // empty and pollKeys() (which polls keysState() for printable chars)
    // reports no input.
    kb.updateKeysState();

    // Wake screen-saver on any keyboard activity and reset inactivity timer
    if (kb.keyList().size() > 0) {
        DisplayTask::wakeFromScreensaver();
        MorseModel::instance().touch();
    }

    static int dbg = 0;
    if (kb.keyList().size() > 0 && (++dbg % 50 == 0)) {
        Log::write("[KB] keys: ");
        for (auto& k : kb.keyList()) {
            char c = kb.getKey(k);
            Log::write("%c(0x%02X) ", c >= 32 ? c : '?', (unsigned char)c);
        }
        Log::write("\r\n");
    }

    static bool wasW = false, wasF = false, wasP = false, wasV = false, wasM = false;
    static bool wasK = false;
    static bool wasD = false;
    static bool wasC = false, wasN = false;
    static bool wasS = false;
    static bool wasEnter = false, wasShift = false;
    static bool wasBtnA = false;
    static bool wasSemicolon = false, wasPeriod = false;
    // K-hold-to-radio latches. kRadioActive tracks whether we currently
    // own a KeyEventBus::keyDown() that needs a matching keyUp() on release.
    // suppressKUntilRelease prevents a held K from immediately keying the
    // radio right after the user closes the KEYING_SETTINGS overlay.
    static bool kRadioActive = false;
    static bool suppressKUntilRelease = false;

    bool wKey       = kb.isKeyPressed('W') || kb.isKeyPressed('w');
    bool fKey       = kb.isKeyPressed('F') || kb.isKeyPressed('f');
    bool pKey       = kb.isKeyPressed('P') || kb.isKeyPressed('p');
    bool vKey       = kb.isKeyPressed('V') || kb.isKeyPressed('v');
    bool mKey       = kb.isKeyPressed('M') || kb.isKeyPressed('m');
    bool kKey       = kb.isKeyPressed('K') || kb.isKeyPressed('k');
    bool dKey       = kb.isKeyPressed('D') || kb.isKeyPressed('d');
    bool cKey       = kb.isKeyPressed('C') || kb.isKeyPressed('c');
    bool nKey       = kb.isKeyPressed('N') || kb.isKeyPressed('n');
    bool sKey       = kb.isKeyPressed('S') || kb.isKeyPressed('s');
    bool enter      = kb.isKeyPressed(KEY_ENTER);
    bool shift      = kb.keysState().shift;
    bool btnA       = M5Cardputer.BtnA.isPressed();
    bool semicolon  = kb.isKeyPressed(';');
    bool period     = kb.isKeyPressed('.');

    auto& model = MorseModel::instance();
    const DisplayScreen sc = model.screen();

    // ─── Cancel playback on any key press ─────────────────────────────
    // Any keyboard key press during memory / P-key playback stops the
    // MorseGenerator and is "consumed" — its per-key handler does NOT
    // fire on this tick or on subsequent ticks while the key remains
    // held. The user must release the key and press it again for the
    // action to take effect. We update wasX = xKey for every tracked
    // key before returning so the next tick sees no stale edge — that
    // is what implements "ignore the key until release". See
    // docs/memory.md §"Cancel-on-any-keypress".
    MorseGenerator* gen = AudioEngine::morseGen();

    // ─── ONE-SHOT keypress log (only when the held-key state changes) ──
    // Throttled: skip the same key state until it changes. Keeps the log
    // ring free for real events.
    static int s_lastKeyState = -1;
    const int keyStateHash =
        (int)kb.keyList().size() +
        (pKey   ? 0x01 : 0) +
        (mKey   ? 0x02 : 0) +
        (kKey   ? 0x04 : 0) +
        (dKey   ? 0x08 : 0) +
        (cKey   ? 0x10 : 0) +
        (nKey   ? 0x20 : 0) +
        (wKey   ? 0x40 : 0) +
        (enter  ? 0x80 : 0) +
        (semicolon ? 0x100 : 0) +
        (period    ? 0x200 : 0) +
        (sKey      ? 0x400 : 0) +
        ((unsigned)pollKeys().printable << 11);
    if (keyStateHash != s_lastKeyState) {
        s_lastKeyState = keyStateHash;
        Log::write("[KB-NEW] list=%d printable=%02x pKey=%d mKey=%d kKey=%d dKey=%d cKey=%d nKey=%d wKey=%d sKey=%d enter=%d semi=%d period=%d sc=%d\n",
            (int)kb.keyList().size(),
            (unsigned)pollKeys().printable,
            pKey, mKey, kKey, dKey, cKey, nKey, wKey, sKey,
            enter, semicolon, period, (int)sc);
    }

    // ─── Cancel playback on rising edge of any tracked key ─────────────
    // EDGE-triggered (not level-triggered): we only cancel on the FIRST
    // tick of a fresh keypress. After that, the same held press is
    // "consumed" by updating wasX = xKey and returning, so the per-key
    // edge handlers below don't fire on subsequent ticks. This is what
    // prevents the gate from cancelling playback the very next tick
    // after it was started by the SAME press — which would otherwise
    // cut off audio after only a click of the first element.
    static bool s_wasAnyKeyHeld = false;
    const bool anyKeyHeld = wKey || fKey || pKey || vKey || mKey
                          || kKey || dKey || cKey || nKey || sKey || enter
                          || semicolon || period;
    const bool anyKeyEdge = anyKeyHeld && !s_wasAnyKeyHeld;
    s_wasAnyKeyHeld = anyKeyHeld;

    if (gen && gen->isPlaying() && anyKeyEdge) {
        Log::write("[GATE] cancelling playback (edge) — list=%d printable=%02x\n",
            (int)kb.keyList().size(), (unsigned)pollKeys().printable);
        gen->stop();
        model.setMode(KeyerMode::KEYER);
    }
    if (gen && gen->isPlaying() && anyKeyHeld) {
        // Same press still held — consume it: update wasX so the per-key
        // edge handlers below don't fire on this held press.
        wasW = wKey; wasF = fKey; wasP = pKey; wasV = vKey; wasM = mKey;
        wasK = kKey; wasD = dKey; wasC = cKey; wasN = nKey; wasS = sKey;
        wasEnter = enter; wasShift = shift; wasBtnA = btnA;
        wasSemicolon = semicolon; wasPeriod = period;
        return;
    }

    // W → WPM settings (toggle). The gate is "not the password input
    // field" — i.e. the function keys stay available on the wifi scan
    // list and the wifi network-info screens, so the user can reach a
    // settings overlay mid-wifi-flow. The only screen where we suppress
    // the toggle is WIFI_PASSWORD_INPUT, where W must type into the
    // passphrase field, not open WPM_SETTINGS.
    if (wKey && !wasW &&
        sc != DisplayScreen::WIFI_PASSWORD_INPUT &&
        (sc == DisplayScreen::DECODER || sc == DisplayScreen::WPM_SETTINGS)) {
        Log::write("[KB] W pressed\r\n");
        if (sc == DisplayScreen::WPM_SETTINGS) {
            model.setScreen(DisplayScreen::DECODER);
        } else {
            model.setScreen(DisplayScreen::WPM_SETTINGS);
            model.setOverlayStartMillis(millis());
        }
        DisplayTask::requestRender();
    }

    // F → frequency settings (toggle). Same gate as W.
    if (fKey && !wasF &&
        sc != DisplayScreen::WIFI_PASSWORD_INPUT &&
        (sc == DisplayScreen::DECODER || sc == DisplayScreen::FREQ_SETTINGS)) {
        if (sc == DisplayScreen::FREQ_SETTINGS) {
            model.setScreen(DisplayScreen::DECODER);
        } else {
            model.setScreen(DisplayScreen::FREQ_SETTINGS);
            model.setOverlayStartMillis(millis());
        }
        DisplayTask::requestRender();
    }

    // V → volume settings (toggle). Same gate as W.
    if (vKey && !wasV &&
        sc != DisplayScreen::WIFI_PASSWORD_INPUT &&
        (sc == DisplayScreen::DECODER || sc == DisplayScreen::VOLUME_SETTINGS)) {
        if (sc == DisplayScreen::VOLUME_SETTINGS) {
            model.setScreen(DisplayScreen::DECODER);
        } else {
            model.setScreen(DisplayScreen::VOLUME_SETTINGS);
            model.setOverlayStartMillis(millis());
        }
        DisplayTask::requestRender();
    }

    // M → memory-keyer picker (open from DECODER). The two-key
    // editing gesture (M + 0-9) reuses the M key that previously
    // toggled MODE_SETTINGS — paddle-vs-straight still lives behind
    // MODE_SETTINGS, but is no longer reachable from the keyboard.
    // (A future gesture can revive it if needed; the screen enum and
    // renderer are kept so the state machine stays self-consistent.)
    if (mKey && !wasM &&
        sc != DisplayScreen::WIFI_PASSWORD_INPUT &&
        sc != DisplayScreen::MEMORY_PICK &&
        sc != DisplayScreen::MEMORY_EDIT &&
        sc == DisplayScreen::DECODER) {
        Log::write("[KB] M pressed → MEMORY_PICK\n");
        // Default cursor lands on slot 0 so the operator sees the first
        // memory by default. They switch slots with 0-9 inside the picker.
        model.setMemoryPickSlot(0);
        model.setScreen(DisplayScreen::MEMORY_PICK);
        model.setOverlayStartMillis(millis());
        DisplayTask::requestRender();
    }

    // D → toggle Console (development) mode ↔ WinKey mode. In WinKey mode
    // the single USB serial port speaks the WinKeyer WK2 protocol to a
    // host logger (RUMlogNG); debug output is buffered and replayed when
    // switching back to Console mode. Default is WinKey mode (the bridge
    // is "always on"; the operator enters Console mode for firmware upload
    // or `pio device monitor`).
    if (dKey && !wasD) {
        bool toWinkey = (Console::mode() != Console::Mode::WinKey);
        Console::setMode(toWinkey ? Console::Mode::WinKey
                                  : Console::Mode::Console);
        model.setWinkeyMode(toWinkey);
        Log::info("[MODE] %s", toWinkey ? "WinKey" : "Console");
        DisplayTask::requestRender();
    }
    wasD = dKey;

    // C → Wi-Fi scan list. Only fires from DECODER (so a stray C inside
    // another overlay cannot interrupt it). On entry, kicks off an async
    // scan — never blocks the keyer.
    if (cKey && !wasC && model.screen() == DisplayScreen::DECODER) {
        model.wifiResetUIState();
        WifiMgr::startScan();
        model.setScreen(DisplayScreen::WIFI_SCAN_LIST);
        model.setOverlayStartMillis(millis());
        DisplayTask::requestRender();
    }
    wasC = cKey;

    // N → Wi-Fi network info / status. Same gate as C: only from DECODER.
    if (nKey && !wasN && model.screen() == DisplayScreen::DECODER) {
        model.setScreen(DisplayScreen::WIFI_NETWORK_INFO);
        model.setOverlayStartMillis(millis());
        DisplayTask::requestRender();
    }
    wasN = nKey;

    // K → keying settings (toggle On/Off radio output). Suppresses a
    // single follow-up press for hold-to-key so opening the overlay
    // with K cannot also key the radio. Same gate as W/F/V/M.
    if (kKey && !wasK &&
        sc != DisplayScreen::WIFI_PASSWORD_INPUT &&
        (sc == DisplayScreen::DECODER || sc == DisplayScreen::KEYING_SETTINGS)) {
        if (sc == DisplayScreen::KEYING_SETTINGS) {
            model.setScreen(DisplayScreen::DECODER);
            suppressKUntilRelease = true;
        } else {
            model.setScreen(DisplayScreen::KEYING_SETTINGS);
            model.setOverlayStartMillis(millis());
            suppressKUntilRelease = true;
        }
        // If we owned a key-down (e.g. switching screens mid-key),
        // release it so we don't leave the line HIGH.
        if (kRadioActive) {
            KeyEventBus::keyUp();
            kRadioActive = false;
        }
        DisplayTask::requestRender();
    }

    // S → paddle polarity settings (Normal/Reversed). Same open/close gate
    // as W/F/V/M/K. There is no hold-to-key behaviour on S, so unlike K it
    // needs no suppress-until-release latch.
    if (sKey && !wasS &&
        sc != DisplayScreen::WIFI_PASSWORD_INPUT &&
        (sc == DisplayScreen::DECODER || sc == DisplayScreen::POLARITY_SETTINGS)) {
        Log::write("[KB] S pressed\r\n");
        if (sc == DisplayScreen::POLARITY_SETTINGS) {
            model.setScreen(DisplayScreen::DECODER);
        } else {
            model.setScreen(DisplayScreen::POLARITY_SETTINGS);
            model.setOverlayStartMillis(millis());
        }
        DisplayTask::requestRender();
    }

    // P → start Morse encoder playback. Suppressed while a memory
    // input field has focus — typing P as part of a contest macro
    // ("TEST DE W1AW POTA K") would otherwise fire "Hello Morse!"
    // mid-edit and clobber the operator's typed text. The release
    // branch below is unaffected: it only fires when mode is already
    // ENCODER, which can only be true if a press actually started
    // playback on a non-memory screen.
    if (pKey && !wasP &&
        sc != DisplayScreen::MEMORY_PICK &&
        sc != DisplayScreen::MEMORY_EDIT) {
        Log::write("[DIAG-P] P handler firing: sc=%d pKey=%d wasP=%d\n", (int)sc, pKey, wasP);
        auto gen = AudioEngine::morseGen();
        if (gen && !gen->isPlaying()) {
            gen->playText("Hello Morse!");
            model.setMode(KeyerMode::ENCODER);
            DisplayTask::requestRender();
            Log::write("[DIAG-P] playText called, isPlaying=%d\n", (int)gen->isPlaying());
        } else {
            Log::write("[DIAG-P] playText SKIPPED: gen=%p isPlaying=%d\n", (void*)gen, gen ? (int)gen->isPlaying() : -1);
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
    // In KEYING settings: ; = On, . = Off
    else if (model.screen() == DisplayScreen::KEYING_SETTINGS) {
        if (semicolon && !wasSemicolon) { model.setRadioKeyingEnabled(true);  DisplayTask::requestRender(); }
        if (period    && !wasPeriod)     { model.setRadioKeyingEnabled(false); DisplayTask::requestRender(); }
        model.setOverlayStartMillis(millis());
    }
    // In POLARITY settings: ; = Normal, . = Reversed. Applied to the running
    // keyer immediately (audible on the next element); Enter persists it.
    else if (model.screen() == DisplayScreen::POLARITY_SETTINGS) {
        if (semicolon && !wasSemicolon) { model.setPolarityReversed(false); DisplayTask::requestRender(); }
        if (period    && !wasPeriod)     { model.setPolarityReversed(true);  DisplayTask::requestRender(); }
        model.setOverlayStartMillis(millis());
    }
    // Wi-Fi screens: dedicated handlers. Each screen has its own edge
    // detection; we always rebuild the keystate via pollKeys() so they see
    // Esc/Enter/Backspace even when the printable-key short-circuits above
    // did not fire.
    else if (model.screen() == DisplayScreen::WIFI_SCAN_LIST ||
             model.screen() == DisplayScreen::WIFI_PASSWORD_INPUT ||
             model.screen() == DisplayScreen::WIFI_NETWORK_INFO) {
        handleWifiScreen(model, pollKeys());
    }
    // Memory-keyer screens: dedicated handlers. Same pattern as Wi-Fi.
    else if (model.screen() == DisplayScreen::MEMORY_PICK ||
             model.screen() == DisplayScreen::MEMORY_EDIT) {
        handleMemoryScreen(model, pollKeys());
    }
    // DECODER: plain-digit playback. The digits 0-9 each map to a memory
    // slot; pressing one plays the slot's text through the sidetone, the
    // WinKeyer bridge (if a host is attached), and the radio keying
    // output (when KEYING is enabled). Edge-detected so holding a digit
    // does not spam playback.
    else if (model.screen() == DisplayScreen::DECODER) {
        static int s_lastDigit = -1;
        const CardputerKeyState pk = pollKeys();
        int digit = -1;
        if (pk.printable >= '0' && pk.printable <= '9') {
            digit = pk.printable - '0';
        }
        if (digit >= 0 && digit != s_lastDigit) {
            Log::write("[DIAG-D] digit=%d printable=%02x lastDigit=%d\n", digit, (unsigned)pk.printable, s_lastDigit);
            const char* text = model.getMemory((uint8_t)digit);
            if (text && text[0] != '\0') {
                Log::write("[DIAG-D] slot has text: \"%.32s\"\n", text);
                // Only call the playback method when there is actually
                // something to play — pressing an unset slot is a
                // silent no-op rather than an error beep.
                Winkey::playLocalMemoryText(text);
                DisplayTask::requestRender();
            } else {
                Log::write("[DIAG-D] slot empty: text=%p text[0]=%d\n", (const void*)text, text ? (int)(unsigned char)text[0] : -1);
            }
        }
        s_lastDigit = digit;
    }

    // K hold-to-key (only when keying is enabled and we are NOT inside
    // the KEYING_SETTINGS overlay, and not suppressed by a recent
    // overlay open/close). We use the central KeyEventBus so iambic,
    // straight key, and this keyboard K all key the radio symmetrically.
    if (model.radioKeyingEnabled() &&
        model.screen() != DisplayScreen::KEYING_SETTINGS &&
        !suppressKUntilRelease) {
        if (kKey && !kRadioActive) {
            KeyEventBus::keyDown();
            kRadioActive = true;
        } else if (!kKey && kRadioActive) {
            KeyEventBus::keyUp();
            kRadioActive = false;
        }
    } else {
        // If we entered the overlay / were suppressed / keying disabled
        // while K was held, drop any owned key-down.
        if (kRadioActive) {
            KeyEventBus::keyUp();
            kRadioActive = false;
        }
    }
    // Clear suppression once the user releases K physically.
    if (!kKey) suppressKUntilRelease = false;

    // Enter: dismiss overlay and return to DECODER. Save settings if in settings screens.
    // The three wifi screens AND the two memory-keyer screens are exempt —
    // handleWifiScreen() / handleMemoryScreen() already gave Enter its
    // meaning for them (select a network, commit a password, pick a slot,
    // commit an edited memory), and the screen they transitioned to would
    // be immediately stomped back to DECODER otherwise.
    const bool onWifiScreen =
        model.screen() == DisplayScreen::WIFI_SCAN_LIST ||
        model.screen() == DisplayScreen::WIFI_PASSWORD_INPUT ||
        model.screen() == DisplayScreen::WIFI_NETWORK_INFO;
    const bool onMemoryScreen =
        model.screen() == DisplayScreen::MEMORY_PICK ||
        model.screen() == DisplayScreen::MEMORY_EDIT;
    if (enter && !wasEnter && !onWifiScreen && !onMemoryScreen) {
        if (model.screen() == DisplayScreen::WPM_SETTINGS) {
            Preferences prefs;
            prefs.begin("morse", false);  // read-write
            prefs.putInt("wpm", model.wpm());
            prefs.end();
            Log::write("[KB] saved WPM=%d to preferences\n", model.wpm());
        }
        if (model.screen() == DisplayScreen::FREQ_SETTINGS) {
            Preferences prefs;
            prefs.begin("morse", false);  // read-write
            prefs.putInt("freq", (int)model.frequency());
            prefs.end();
            Log::write("[KB] saved freq=%d to preferences\n", (int)model.frequency());
        }
        if (model.screen() == DisplayScreen::VOLUME_SETTINGS) {
            Preferences prefs;
            prefs.begin("morse", false);  // read-write
            prefs.putInt("vol", model.volume());
            prefs.end();
            Log::write("[KB] saved vol=%d to preferences\n", model.volume());
        }
        if (model.screen() == DisplayScreen::MODE_SETTINGS) {
            Preferences prefs;
            prefs.begin("morse", false);  // read-write
            prefs.putString("keytype", model.keyerType() == KeyerType::PADDLE ? "paddle" : "straight");
            prefs.end();
            Log::write("[KB] saved keyerType=%s\n", model.keyerType() == KeyerType::PADDLE ? "paddle" : "straight");
        }
        if (model.screen() == DisplayScreen::KEYING_SETTINGS) {
            Preferences prefs;
            prefs.begin("morse", false);  // read-write
            prefs.putBool("keying", model.radioKeyingEnabled());
            prefs.end();
            Log::write("[KB] saved keying=%d\n", model.radioKeyingEnabled() ? 1 : 0);
        }
        if (model.screen() == DisplayScreen::POLARITY_SETTINGS) {
            Preferences prefs;
            prefs.begin("morse", false);  // read-write
            prefs.putBool("polarity", model.polarityReversed());
            prefs.end();
            Log::write("[KB] saved polarity=%s\n", model.polarityReversed() ? "reversed" : "normal");
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

    wasW = wKey; wasF = fKey; wasV = vKey; wasM = mKey; wasK = kKey; wasShift = shift;
    wasSemicolon = semicolon; wasPeriod = period;
    wasS = sKey;
    // wasC/wasN are tracked where they are used (above).
#else
    (void)0;
#endif
}

void setup() {
    // NOTE: we deliberately do NOT call Serial.begin() here. With
    // ARDUINO_USB_CDC_ON_BOOT=0 (see platformio.ini) `Serial` is aliased
    // to Serial0 (UART0), which has no physical pins on the Cardputer —
    // a Serial.begin(115200) call would be a no-op against the wrong
    // peripheral. The CDC peripheral that the WinKeyer bridge uses is
    // brought up later by Console::begin() — see below — after M5 has
    // settled pin ownership, so the macOS CDC driver sees a stable
    // enumeration (no peripheral-manager pin-dance race during early
    // boot).

    auto cfg = M5.config();
    cfg.internal_spk = false;
    M5.begin(cfg);

#ifdef BOARD_CARDPUTER
    Log::write("[setup] M5.getBoard() = %d\n", (int)M5.getBoard());
    M5Cardputer.begin(true);
#endif

    // Bring up the CDC peripheral for Console/WinKey I/O AFTER M5 has
    // claimed whatever pins it needs. Doing this earlier (e.g. as the
    // very first line of setup()) triggered a macOS enumeration race
    // where the host's CDC driver bound to a stale descriptor and
    // required a manual reset to recover. See docs/winkey_test.md §
    // "The USB-CDC startup issue" for the full trace.
    Console::begin();

    // Bring the display up BEFORE the rest of the heavy init, so the
    // user sees the device is alive immediately on plug-in. If audio
    // or keying init fails later, the screen at least shows something.
#ifdef BOARD_CARDPUTER
    DisplayTask::begin(new CardputerDisplay());
#else
    DisplayTask::begin(nullptr);
#endif

    Log::write("=== Morse Trainer ===\r\n");

    if (!AudioEngine::begin()) {
        // Log and continue rather than hang forever. The display will
        // still be alive; the user can read the error on screen and on
        // the serial monitor. Previously this was `while(true) delay(1000)`
        // which left the device looking dead until a reset.
        Log::error("FATAL: AudioEngine::begin failed (continuing without audio)");
    } else {
        AudioEngine::createMorseGen();

        // WinKeyer 2.x emulation on USB CDC1 (second serial port). Host
        // loggers (e.g. RUMlogNG) drive keying over this port while CDC0
        // stays free for upload + monitor + logs. See docs/winkey.md.
        Winkey::begin();

        // Paddle key input (GPIO interrupts)
        MorseKey::begin();

        // Decoder: wire both keyers' ring buffers to MorseDecoder
        MorseDecoder::begin(AudioEngine::keyer(), AudioEngine::straightKeyer());
    }

    // Radio keying output (Cardputer only). Subscribes to KeyEventBus.
    // Independent of audio — works even if AudioEngine::begin() failed.
#ifdef BOARD_CARDPUTER
    RadioKeyer::begin();
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
        bool savedKeying = prefs.getBool("keying", false);
        bool savedPolarity = prefs.getBool("polarity", false);
        prefs.end();
        model.setWPM(savedWpm);
        model.setFrequency((float)savedFreq);
        model.setVolume(savedVol);
        model.setKeyerType(savedKeyType == "straight" ? KeyerType::STRAIGHT : KeyerType::PADDLE);
        // Apply the persisted keying setting AFTER RadioKeyer::begin() so
        // the GPIO is owned and ready. Default is Off (safe).
        model.setRadioKeyingEnabled(savedKeying);
        // Paddle polarity — forwarded to the iambic keyer, which AudioEngine
        // has already constructed by this point. Default is Normal.
        model.setPolarityReversed(savedPolarity);
        Log::write("[setup] loaded WPM=%d freq=%d vol=%d keytype=%s keying=%d polarity=%s from preferences\n",
            savedWpm, savedFreq, savedVol, savedKeyType.c_str(), savedKeying ? 1 : 0,
            savedPolarity ? "reversed" : "normal");
    }

    // Memory-keyer bank — separate NVS namespace ("memory") so clearing
    // the device settings (the "morse" namespace above) does not wipe
    // the operator's stored CQ/contest exchanges. Loaded into MorseModel
    // once at startup; per-commit saves happen on Enter from MEMORY_EDIT
    // (see handleMemoryScreen()).
    {
        MemoryBank bank;
        memoryBankLoad(bank);
        MorseModel::instance().copyMemoryBank(bank);
        int populated = 0;
        for (uint8_t i = 0; i < kMemSlots; ++i) {
            if (bank.slot[i][0] != '\0') ++populated;
        }
        Log::info("[setup] loaded memory bank: %d/%d slots populated",
            populated, (int)kMemSlots);
    }

    Log::info("A1Keyer v%s", A1KEYER_VERSION);
    Log::info("Ready.");

    // ─── Network manager ─────────────────────────────────────────────────
    // Bring up WiFi via NetworkManager. This is non-blocking: the link
    // comes up over the next few seconds through the WiFi-event pipeline.
    // Credentials come exclusively from NVS, populated by the on-device
    // keyboard flow (see docs/network.md §3).
    WifiMgr::bindPlatformHal();
    WifiMgr::begin();

    if (WifiMgr::hasSavedCredentials()) {
        WifiMgr::connectWithSaved();
    }

    // Register HTTP routes. The listener itself starts lazily from
    // HttpServer::poll() once WifiMgr reports connected — see
    // src/http_server.cpp for the lazy-init rationale. Stays
    // compiled-out in shipping builds.
#if ENABLE_WIFI_DEBUG
    HttpServer::begin();
    ConsoleServer::begin(80);
    WebUI::begin(80);
#endif
}

void loop() {
    M5.update();
    handleKeyboard();

    // Service the Wi-Fi state machine and mirror its state into the model
    // so the display task can render purely from MorseModel. The mirror
    // setters are no-ops when values are unchanged, so idle ticks stay
    // cheap.
    WifiMgr::poll();
    mirrorWifiState(MorseModel::instance());

#if ENABLE_WIFI_DEBUG
    // Service the HTTP listener and the on-device web UI. No-op when
    // the link is down; the gate compiles out entirely in shipping builds.
    HttpServer::poll();
    WebUI::poll();
#endif

    // Service the WinKeyer stream (host logger → keying), only in WinKey mode.
    Winkey::poll();

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