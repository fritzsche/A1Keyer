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

    /** Put display into low-power / sleep mode (screen-saver). */
    virtual void powerOff() = 0;

    /** Wake display from sleep mode. */
    virtual void powerOn() = 0;
};