#pragma once
#include "display_model.h"

class DisplayInterface {
public:
    virtual ~DisplayInterface() = default;

    /** Initialize the display hardware. Called once from setup(). */
    virtual void init() = 0;

    /** Clear the entire screen. */
    virtual void clear() = 0;

    /**
     * Render the current screen state from the model.
     * Called by the display task whenever changeCounter increases.
     */
    virtual void render() = 0;

    /** Update the status line area only (top strip). */
    virtual void updateStatusLine(MorseModel& model) = 0;

    /** Update the main text area only. */
    virtual void updateMainText(MorseModel& model) = 0;

    /** Show the WPM overlay screen. */
    virtual void showWPMView(MorseModel& model) = 0;

    /** Show the frequency overlay screen. */
    virtual void showFreqView(MorseModel& model) = 0;

    /** Show the WPM settings screen (in-place editing with Fn+;/Fn+.). */
    virtual void showWPMSettingsView(MorseModel& model) = 0;

    /** Show the frequency settings screen (in-place editing with Fn+;/Fn+.). */
    virtual void showFreqSettingsView(MorseModel& model) = 0;

    /** Show the volume overlay screen. */
    virtual void showVolumeView(MorseModel& model) = 0;

    /** Show the volume settings screen (in-place editing with Fn+;/Fn+.). */
    virtual void showVolumeSettingsView(MorseModel& model) = 0;

    /** Show the mode overlay screen. */
    virtual void showModeView(MorseModel& model) = 0;

    /** Show the mode settings screen (in-place editing with Fn+;/Fn+.). */
    virtual void showModeSettingsView(MorseModel& model) = 0;

    /** Show the radio-keying settings screen (On/Off toggle, ;/.). */
    virtual void showKeyingSettingsView(MorseModel& model) = 0;

    /** Show the paddle-polarity settings screen (Normal/Reversed toggle, ;/.). */
    virtual void showPolaritySettingsView(MorseModel& model) = 0;

    /** Show the Wabun mode settings screen (International / Katakana / Hiragana). */
    virtual void showWabunSettingsView(MorseModel& model) = 0;

    /** Show the Wi-Fi scan list / "Scanning…" / scan-failed screen. */
    virtual void showWifiScanList(MorseModel& model) = 0;

    /** Show the Wi-Fi passphrase entry screen. */
    virtual void showWifiPasswordInput(MorseModel& model) = 0;

    /**
     * Show the AP passphrase entry screen. Same TextInput-backed UI as
     * the STA password screen, but the title and SSID line are locked
     * to "A1Keyer (AP)" and there is no scan-cursor.
     */
    virtual void showWifiApPasswordInput(MorseModel& model) = 0;

    /** Show the Wi-Fi status / IP / error screen. */
    virtual void showWifiNetworkInfo(MorseModel& model) = 0;

    /**
     * Draw the "Forget network?" Y/N overlay on top of the Wi-Fi
     * status screen. main.cpp drives the modal state through
     * MorseModel::wifiNetConfirmForget(); this method is a pure
     * renderer and assumes the underlying screen has already been
     * drawn this frame.
     */
    virtual void showNetConfirmForget(MorseModel& model) = 0;

    /**
     * Show the memory-keyer slot picker.
     *
     * Rendered after `M` from DECODER. Body lists the 10 slots with a
     * small preview of any non-empty entries so the operator can see at
     * a glance which slots are populated. The next keystroke (0-9)
     * advances to MEMORY_EDIT for that slot.
     */
    virtual void showMemoryPick(MorseModel& model) = 0;

    /**
     * Show the memory-keyer slot editor.
     *
     * Modal single-line editor reusing the same TextInput control that
     * backs the Wi-Fi passphrase screen. Title is "Mem N" in accent
     * color; field is masked by default with FN toggling reveal.
     */
    virtual void showMemoryEdit(MorseModel& model) = 0;

    /** Put display into low-power / sleep mode (screen-saver). */
    virtual void powerOff() = 0;

    /** Wake display from sleep mode. */
    virtual void powerOn() = 0;
};