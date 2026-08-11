#pragma once
/**
 * morse_decoder.h — Architecture for CW decoder.
 *
 * The decoder consumes symbols ('.', '-', '*', ' ') from the active keyer's
 * lock-free ring buffer and decodes them to text.
 *
 * Both IambicKeyer and StraightKeyer have their own ring buffers.
 * Routing is based on MorseModel::keyerType().
 *
 * Consumption happens from loop() — non-blocking read.
 *
 * Decoded characters can optionally be forwarded to a host via the
 * `DecodedCharFn` hook (installed by `setDecodedCharHook`). The hook fires
 * once per decoded character — including the space character for word
 * gaps — so a WinKey bridge can re-emit the result to the host logger
 * (mirrors K3NG's `winkey_paddle_echo_buffer` decode path at
 * `k3ng_keyer.ino:11623-11631`). Multiple-character prosigns (`<AR>`,
 * `<SK>`, etc.) expand to one hook call per display character.
 */

#include <cstdint>
#include <cstddef>
#include "morse_constants.h"

class IambicKeyer;    // forward declaration
class StraightKeyer;  // forward declaration

class MorseDecoder {
public:
    /** Decoder symbol aliases — see MorseConstants for definitions. */
    static constexpr char END_OF_CHAR = MorseConstants::END_OF_CHAR;
    static constexpr char SPACE_CHAR  = MorseConstants::SPACE_CHAR;
    static constexpr char DIT_SYMBOL  = MorseConstants::DIT_SYMBOL;
    static constexpr char DAH_SYMBOL  = MorseConstants::DAH_SYMBOL;

    /**
     * Optional sink for decoded characters. The hook fires from the
     * decoder consumer (loop()/main.cpp) — never from an ISR or audio
     * task context — so it may safely call non-trivial code (e.g.
     * wake a host logger via the WK bridge).
     */
    using DecodedCharFn = void (*)(char c, void* ctx);

    /**
     * Initialise the decoder with both keyer ring buffers.
     * @param keyer         The IambicKeyer instance.
     * @param straightKeyer The StraightKeyer instance.
     */
    static void begin(IambicKeyer* keyer, StraightKeyer* straightKeyer);

    /**
     * Register a hook to receive decoded characters as they are
     * produced. Replaces any previous hook. Pass `nullptr` to disable.
     * The hook is called from `accumulate()` / `flush()` only — i.e.
     * from the consumer thread, not from an ISR.
     */
    static void setDecodedCharHook(DecodedCharFn fn, void* ctx);

    /**
     * Non-blocking read from the active keyer's ring buffer.
     * Routes to IambicKeyer or StraightKeyer based on MorseModel::keyerType().
     * @param out  Pointer to receive the character ('.', '-', '*', ' ')
     * @return true if a character was available and placed in *out
     */
    static bool read(char* out);

    /** Number of symbols waiting in the active keyer's buffer. */
    static size_t available();

    // --- Decode helpers (used by loop()) ---

    /** Accumulate symbol into current character buffer. */
    static void accumulate(char symbol, char* buffer, size_t bufferSize, size_t* inoutPos);

    /** Decode and print the current buffer. */
    static void flush(char* buffer, size_t* inoutPos);

private:
    static IambicKeyer* s_keyer;
    static StraightKeyer* s_straightKeyer;
    static DecodedCharFn s_decodedCharHook;
    static void*         s_decodedCharCtx;
    static void fireDecodedChar(char c);
};
