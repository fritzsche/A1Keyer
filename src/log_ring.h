#pragma once
/**
 * log_ring.h — passive log-byte ring buffer tapped from Console::write.
 *
 * Console::write() is the single choke point every Log::* call routes
 * through (see console_io.h). This module captures those bytes into a
 * small evict-oldest ring so an HTTP handler (or test) can serve the
 * most recent log tail without disturbing the USB serial line or the
 * existing WinKey-mode replay ring.
 *
 * Design choices:
 *   - Header-only and dependency-free so it compiles identically on
 *     device (Arduino) and in host unit tests. No Arduino String, no
 *     IPAddress, no WiFi — just std::atomic + std::string + std::vector.
 *   - Evict-oldest, byte-oriented. The snapshot walks the buffer in
 *     chronological order, splits on '\n', and returns the last N
 *     complete lines.
 *   - Concurrency: lock-free std::atomic. The write is a three-step
 *     update (store byte, advance head, conditionally bump len).
 *     Racing writers can corrupt at most one byte; the next call
 *     overwrites it. This is benign for log capture — no protocol
 *     depends on it. If contention rises, swap to a tiny critical
 *     section; do not over-engineer now.
 *
 * Wire-up: enable the tap by setting ENABLE_WIFI_DEBUG=1 in
 * platformio.ini. The hook lives in console_io.cpp.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class LogRing {
public:
    /// 4 KB holds ~50 log lines at ~80 chars/line; plenty for "what just
    /// happened" diagnostics over HTTP.
    static constexpr size_t kCapacity = 4096;

    /// Append one byte. Evicts oldest when full.
    void write(uint8_t b) {
        size_t h = _head.load(std::memory_order_relaxed);
        size_t l = _len.load(std::memory_order_relaxed);
        _buf[h] = static_cast<char>(b);
        h = (h + 1) % kCapacity;
        _head.store(h, std::memory_order_relaxed);
        if (l < kCapacity) {
            _len.store(l + 1, std::memory_order_relaxed);
        }
        // When full, _head wraps; _len stays at kCapacity and the
        // oldest byte is implicitly at _head (the next write target).
    }

    /// Bytes currently stored (0..kCapacity).
    size_t len() const { return _len.load(std::memory_order_relaxed); }

    /// True when nothing has been written since construction/reset.
    bool empty() const { return len() == 0; }

    /**
     * snapshotLines — return the last n complete LF-terminated lines.
     *
     * A "complete line" is text between two '\n' bytes (or between the
     * start of valid data and the first '\n'). The trailing '\n' (and
     * any preceding '\r' from CRLF) are stripped so the JSON string
     * represents what the user would see on a serial monitor.
     *
     * Lines that have not yet been terminated by '\n' are NOT
     * returned — a partial line at the buffer tail is ignored. This
     * prevents the JSON from showing half-written output.
     *
     * @param n  Maximum number of lines to return (clamped to [1, 200]).
     * @return   A std::string holding a JSON array of the lines,
     *           e.g. `["line one","line two"]`. Empty array when no
     *           complete lines have been captured yet.
     */
    std::string snapshotLines(size_t n) const {
        if (n == 0) n = 1;
        if (n > 200) n = 200;

        const size_t l = len();
        if (l == 0) return std::string("[]");

        const size_t cap = kCapacity;
        const size_t head = _head.load(std::memory_order_relaxed);

        // The oldest valid byte: index 0 when not yet wrapped, else _head.
        const size_t startIdx = (l < cap) ? 0 : head;

        // Copy buffer contents into a contiguous string in chronological order.
        std::string all(l, '\0');
        size_t a = startIdx;
        for (size_t k = 0; k < l; ++k) {
            all[k] = _buf[a];
            a = (a + 1) % cap;
        }

        // Find every '\n' position.
        std::vector<size_t> nlPos;
        for (size_t k = 0; k < l; ++k) {
            if (all[k] == '\n') nlPos.push_back(k);
        }
        if (nlPos.empty()) return std::string("[]");

        // Keep only the last n '\n'-terminated lines.
        size_t startK = (nlPos.size() > n) ? (nlPos.size() - n) : 0;

        std::string out;
        out.reserve(startK == 0 ? (nlPos.size() * 64 + 2) : (n * 64 + 2));
        out.push_back('[');
        for (size_t k = startK; k < nlPos.size(); ++k) {
            size_t lineStart = (k == 0) ? 0 : (nlPos[k - 1] + 1);
            size_t lineEnd   = nlPos[k];          // inclusive of '\n'
            // Strip trailing CR/LF so the JSON string is what the user
            // would see on a monitor (no embedded escape noise).
            while (lineEnd > lineStart &&
                   (all[lineEnd] == '\n' || all[lineEnd] == '\r')) {
                --lineEnd;
            }
            if (lineEnd < lineStart) continue;    // empty line, skip
            std::string line = all.substr(lineStart, lineEnd - lineStart + 1);
            if (out.size() > 1) out.push_back(',');
            out.push_back('"');
            jsonEscapeAppend(out, line);
            out.push_back('"');
        }
        out.push_back(']');
        return out;
    }

    /// Reset the ring (drops all captured bytes).
    void clear() {
        _head.store(0, std::memory_order_relaxed);
        _len.store(0, std::memory_order_relaxed);
    }

    /// Singleton accessor — LogRing is process-wide.
    static LogRing& instance() {
        static LogRing r;
        return r;
    }

    /// Constructor is public so unit tests can create ephemeral instances.
    /// In production code prefer the `instance()` singleton.
    LogRing() = default;

    static void jsonEscapeAppend(std::string& dst, const std::string& src) {
        for (char c : src) {
            switch (c) {
                case '"':  dst.append("\\\""); break;
                case '\\': dst.append("\\\\"); break;
                case '\b': dst.append("\\b");  break;
                case '\f': dst.append("\\f");  break;
                case '\n': dst.append("\\n");  break;
                case '\r': dst.append("\\r");  break;
                case '\t': dst.append("\\t");  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x",
                                      static_cast<unsigned char>(c));
                        dst.append(buf);
                    } else {
                        dst.push_back(c);
                    }
            }
        }
    }

    char _buf[kCapacity]{};
    std::atomic<size_t> _head{0};
    std::atomic<size_t> _len{0};
};