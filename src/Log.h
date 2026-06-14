#pragma once
/**
 * Log.h — Centralised logging wrapper.
 *
 * Controls all debug output from a single point.
 * Set LOG_SERIAL=0 to silence all Log::* calls at compile time.
 *
 * Usage:
 *   Log.info("volume %d%%", vol);
 *   Log.warning("codecWrite failed");
 *   Log.error("FATAL: out of memory");
 *   Log.debug("phase=%.2f", phase);  // only if Log::DEBUG == true
 */

#include <Arduino.h>
#include <cstdarg>

class Log {
public:
    /// Master switch: set to false to compile out all debug output.
    static constexpr bool ENABLED = true;

    /// Verbose debug messages (detailed diagnostics). Set to false for release.
    static constexpr bool DEBUG = true;

#if LOG_SERIAL
    static void info(const char* fmt, ...) {
        if (!ENABLED) return;
        va_list args;
        va_start(args, fmt);
        Serial.print("[INFO] ");
        Serial.vprintf(fmt, args);
        Serial.println();
        va_end(args);
    }

    static void warning(const char* fmt, ...) {
        if (!ENABLED) return;
        va_list args;
        va_start(args, fmt);
        Serial.print("[WARN] ");
        Serial.vprintf(fmt, args);
        Serial.println();
        va_end(args);
    }

    static void error(const char* fmt, ...) {
        if (!ENABLED) return;
        va_list args;
        va_start(args, fmt);
        Serial.print("[ERROR] ");
        Serial.vprintf(fmt, args);
        Serial.println();
        va_end(args);
    }

    static void debug(const char* fmt, ...) {
        if (!ENABLED || !DEBUG) return;
        va_list args;
        va_start(args, fmt);
        Serial.print("[DEBUG] ");
        Serial.vprintf(fmt, args);
        Serial.println();
        va_end(args);
    }

    /** Write text without prefix or newline. Use for continuous decoder output. */
    static void write(const char* fmt, ...) {
        if (!ENABLED) return;
        va_list args;
        va_start(args, fmt);
        Serial.vprintf(fmt, args);
        va_end(args);
    }
#else
    static void info(const char*, ...) {}
    static void warning(const char*, ...) {}
    static void error(const char*, ...) {}
    static void debug(const char*, ...) {}
    static void write(const char*, ...) {}
#endif
};
