#pragma once
#include <cstdint>
#include "display_interface.h"

namespace DisplayTask {
    void begin(DisplayInterface* display);
    void requestRender();

    /** Called from morse key ISR to wake the display from screen-saver.
     *  This function is safe to call from an ISR context. */
    void wakeFromScreensaver();
}