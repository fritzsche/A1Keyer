/**
 * morse_decoder.cpp — CW decoder implementation.
 *
 * Consumes symbols ('.', '-', '*', ' ') from the active keyer's lock-free
 * ring buffer (IambicKeyer or StraightKeyer) and decodes them to text.
 *
 * Routing is based on MorseModel::keyerType():
 *   PADDLE → IambicKeyer's ring buffer
 *   STRAIGHT → StraightKeyer's ring buffer
 *
 * Symbol meanings:
 *   '.' or '-' : accumulate into buffer
 *   '*'         : END_OF_CHAR — flush buffer, decode and print
 *   ' '         : SPACE_CHAR — print space immediately (word gap)
 */

#include "Log.h"
#include "morse_decoder.h"
#include "iambic_keyer.h"
#include "straight_keyer.h"
#include "morse_encoder.h"
#include "display_model.h"
#include <cstdio>

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------
IambicKeyer*    MorseDecoder::s_keyer        = nullptr;
StraightKeyer*  MorseDecoder::s_straightKeyer = nullptr;

// ---------------------------------------------------------------------------
// MorseDecoder::begin
// ---------------------------------------------------------------------------
void MorseDecoder::begin(IambicKeyer* keyer, StraightKeyer* straightKeyer) {
    s_keyer        = keyer;
    s_straightKeyer = straightKeyer;
}

// ---------------------------------------------------------------------------
// MorseDecoder::read — delegate to the active keyer's ring buffer
// ---------------------------------------------------------------------------
bool MorseDecoder::read(char* out) {
    if (!out) return false;
    if (MorseModel::instance().keyerType() == KeyerType::STRAIGHT) {
        if (!s_straightKeyer) return false;
        return s_straightKeyer->decoderRead(out);
    } else {
        if (!s_keyer) return false;
        return s_keyer->decoderRead(out);
    }
}

// ---------------------------------------------------------------------------
// MorseDecoder::available
// ---------------------------------------------------------------------------
size_t MorseDecoder::available() {
    if (MorseModel::instance().keyerType() == KeyerType::STRAIGHT) {
        if (!s_straightKeyer) return 0;
        return s_straightKeyer->decoderAvailable();
    } else {
        if (!s_keyer) return 0;
        return s_keyer->decoderAvailable();
    }
}

// ---------------------------------------------------------------------------
// MorseDecoder::accumulate
// ---------------------------------------------------------------------------
void MorseDecoder::accumulate(char symbol, char* buffer,
                              size_t bufferSize, size_t* inoutPos) {
    if (!buffer || !inoutPos) return;

    if (symbol == END_OF_CHAR) {
        // End of character: flush buffer and print decoded character
        flush(buffer, inoutPos);
        return;
    }

    if (symbol == SPACE_CHAR) {
        // Word space: flush any pending character, then append space to model
        flush(buffer, inoutPos);
        Log::write(" ");
        MorseModel::instance().appendDecodedChar(' ');
        return;
    }

    // '.' or '-': accumulate into buffer
    if (*inoutPos < bufferSize - 1) {
        buffer[(*inoutPos)++] = symbol;
    }
}

// ---------------------------------------------------------------------------
// MorseDecoder::flush — decode accumulated morse buffer and output
// ---------------------------------------------------------------------------
void MorseDecoder::flush(char* buffer, size_t* inoutPos) {
    if (!buffer || !inoutPos || *inoutPos == 0) return;

    buffer[*inoutPos] = '\0';

    const char* decoded = MorseEncoder::charFromMorse(buffer);
    if (!decoded) {
        // Unknown morse — show the pattern in angle brackets, e.g. <----> or <......>
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "<%s>", buffer);
        Log::write("%s", errBuf);
        for (const char* p = errBuf; *p; ++p) {
            MorseModel::instance().appendDecodedChar(*p);
        }
    } else {
        Log::write("%s", decoded);
        // Append every character of the decoded result (prosigns like <ka>
        // expand to multiple display characters).
        for (const char* p = decoded; *p; ++p) {
            MorseModel::instance().appendDecodedChar(*p);
        }
    }

    *inoutPos = 0;
}