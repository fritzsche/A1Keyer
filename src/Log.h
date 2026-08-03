#pragma once
/**
 * Log.h — Centralised logging wrapper.
 *
 * Controls all debug output from a single point.
 * Set LOG_SERIAL=0 to silence all Log::* calls at compile time.
 *
 * All output is routed through Console (src/console_io.h), the mode gate
 * that owns the serial line. In Console mode it goes to the wire; in
 * WinKey mode it is buffered and replayed later, so debug text never
 * corrupts the WinKeyer binary stream. Prefer Log::* over raw Serial.*
 * everywhere so nothing bypasses the gate.
 *
 * Usage:
 *   Log::info("volume %d%%", vol);
 *   Log::warning("codecWrite failed");
 *   Log::error("FATAL: out of memory");
 *   Log::debug("phase=%.2f", phase);  // only if Log::DEBUG == true
 */

#include <cstdarg>
#include "console_io.h"

class Log {
public:
    /// Master switch: set to false to compile out all debug output.
    static constexpr bool ENABLED = true;

    /// Verbose debug messages (detailed diagnostics). Set to false for release.
    static constexpr bool DEBUG = true;

#if LOG_SERIAL
private:
    // Single choke point: prefix + formatted body + newline, all via the
    // Console mode gate.
    static void emit(const char* prefix, const char* fmt, va_list args) {
        if (prefix) Console::write((const uint8_t*)prefix, prefixLen(prefix));
        Console::vprintf(fmt, args);
        Console::write((const uint8_t*)"\r\n", 2);
    }
    static size_t prefixLen(const char* s) {
        size_t n = 0; while (s[n]) ++n; return n;
    }

public:
    static void info(const char* fmt, ...) {
        if (!ENABLED) return;
        va_list args; va_start(args, fmt);
        emit("[INFO] ", fmt, args);
        va_end(args);
    }

    static void warning(const char* fmt, ...) {
        if (!ENABLED) return;
        va_list args; va_start(args, fmt);
        emit("[WARN] ", fmt, args);
        va_end(args);
    }

    static void error(const char* fmt, ...) {
        if (!ENABLED) return;
        va_list args; va_start(args, fmt);
        emit("[ERROR] ", fmt, args);
        va_end(args);
    }

    static void debug(const char* fmt, ...) {
        if (!ENABLED || !DEBUG) return;
        va_list args; va_start(args, fmt);
        emit("[DEBUG] ", fmt, args);
        va_end(args);
    }

    /** Write text without prefix or newline. Use for continuous decoder output. */
    static void write(const char* fmt, ...) {
        if (!ENABLED) return;
        va_list args; va_start(args, fmt);
        Console::vprintf(fmt, args);
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
