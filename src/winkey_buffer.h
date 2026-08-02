#pragma once
/**
 * winkey_buffer.h — WinKeyer CW send FIFO.
 *
 * WinKeyer text-to-send is not a load/play memory: in operating mode the
 * host streams printable ASCII (bytes >= 0x20) and the chip keys them as
 * CW immediately. This buffer accumulates those characters between
 * loop() iterations so the bridge can hand a chunk to the CW generator,
 * and supports the WK backspace (0x08) and clear-buffer (0x0A) commands.
 *
 * XOFF is asserted once occupancy passes 2/3 of capacity (WK2 § 12) so
 * the bridge can tell the host to throttle.
 *
 * Header-only and dependency-free: compiles identically on device and in
 * host unit tests. See docs/winkey.md § 13.
 */
#include <cstdint>
#include <cstddef>

class WinkeyBuffer {
public:
    /// Send-buffer capacity. WK2 cites ~110 chars; round up.
    static constexpr size_t kCapacity = 128;

    WinkeyBuffer() { clear(); }

    /// Append one character to send. Returns false (and latches overflow)
    /// when full.
    bool push(char c) {
        if (_len >= kCapacity) { _overflow = true; return false; }
        _buf[(_head + _len) % kCapacity] = c;
        ++_len;
        return true;
    }

    /// WK backspace (0x08): remove the most recently appended character
    /// that has not yet been drained. No-op when empty.
    void backspace() {
        if (_len > 0) --_len;
    }

    /// True when there is nothing left to send.
    bool empty() const { return _len == 0; }

    /// Number of characters waiting to be sent.
    size_t size() const { return _len; }

    /// Pop the oldest character (FIFO). Returns '\0' when empty.
    char pop() {
        if (_len == 0) return '\0';
        char c = _buf[_head];
        _head = (_head + 1) % kCapacity;
        --_len;
        return c;
    }

    /// Clear the buffer (WK 0x0A clear-buffer / reset).
    void clear() {
        _head = 0; _len = 0; _overflow = false;
    }

    /// XOFF threshold reached (occupancy > 2/3 capacity).
    bool xoff() const { return _len > (kCapacity * 2 / 3); }

    /// Overflow latched since the last clear().
    bool overflowed() const { return _overflow; }

private:
    char   _buf[kCapacity];
    size_t _head;      // index of the oldest char
    size_t _len;       // number of chars waiting
    bool   _overflow;
};
