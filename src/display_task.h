#pragma once
#include <cstdint>
#include "display_interface.h"

namespace DisplayTask {
    void begin(DisplayInterface* display);
    void requestRender();

    /** Called from morse key ISR to wake the display from screen-saver.
     *  This function is safe to call from an ISR context. */
    void wakeFromScreensaver();

    /** Read-and-clear the wake-up flag. Returns true if a wake request
     *  was pending (i.e. a key-down transition or other activity
     *  happened since the last call). The display task calls this
     *  once per tick to drain pending requests; tests use it to
     *  verify that keying activity bumps the flag. */
    bool consumeWakeRequest();
}