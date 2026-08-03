#pragma once
/**
 * console_io.h — single mode-gated owner of the USB serial line.
 *
 * A1Keyer exposes ONE hardware USB-Serial-JTAG port (ARDUINO_USB_MODE=1).
 * That single port serves two runtime modes, toggled by the 'D' key:
 *
 *   - Mode::Console (default) — normal debug serial. All log/print output
 *     goes straight to the wire.
 *   - Mode::WinKey — the port speaks the K1EL WinKeyer WK2 binary
 *     protocol (for RUMlogNG / N1MM / fldigi). The protocol is binary, so
 *     ANY stray debug byte would corrupt it. In this mode all console
 *     output is captured into a RAM ring buffer instead of being written,
 *     and the WinkeyBridge is the ONLY writer to the wire (via
 *     rawWinkeyWrite()).
 *
 * On switching WinKey → Console the buffered log bytes are replayed to the
 * terminal, so a developer sees what happened during a WinKey test
 * session. On overflow the ring evicts the oldest bytes.
 *
 * Everything the firmware logs must go through Console (Log.h routes here;
 * raw Serial.* calls were converted to Log::*), so the mode gate is the
 * single choke point that keeps the WinKeyer stream clean.
 *
 * Concurrency: the audio task (Core 1) logs too, so writes are guarded by
 * a portMUX critical section. On host (UNIT_TEST) builds this compiles to
 * a plain passthrough with no Arduino dependency.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

class Console {
public:
    enum class Mode : uint8_t { Console = 0, WinKey = 1 };

    /// Bring up the serial line. Call once from setup().
    static void begin();

    /// Current mode. Cheap load.
    static Mode mode();

    /// Change mode. On WinKey → Console this replays the buffered log
    /// bytes to the wire and clears the ring. Idempotent for same mode.
    static void setMode(Mode m);

    /// Convenience: true when in WinKey mode.
    static bool isWinKey();

    /// Console output sink used by Log.h. In Console mode writes to the
    /// wire; in WinKey mode appends to the replay ring (evict-oldest).
    static void write(uint8_t byte);
    static void write(const uint8_t* data, size_t len);
    static void vprintf(const char* fmt, va_list args);

    /// The ONLY path that writes real bytes to the wire while in WinKey
    /// mode. Used by the WinkeyBridge output sink so the WK2 protocol
    /// stream is never polluted by log output.
    static void rawWinkeyWrite(uint8_t byte);

    /// Bytes available from the wire (RX). Used by the WinKey transport;
    /// callers must gate on mode themselves.
    static int available();
    static int read();

private:
    Console() = delete;
};
