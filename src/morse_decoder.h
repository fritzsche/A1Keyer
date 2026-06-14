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
     * Initialise the decoder with both keyer ring buffers.
     * @param keyer         The IambicKeyer instance.
     * @param straightKeyer The StraightKeyer instance.
     */
    static void begin(IambicKeyer* keyer, StraightKeyer* straightKeyer);

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
};
