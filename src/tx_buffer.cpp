/**
 * tx_buffer.cpp — TX-buffer playback driver.
 *
 * See tx_buffer.h for the design. Stateless — all state in MorseModel.
 *
 * The driver chunks the buffer on word boundaries and pushes each chunk
 * to MorseGenerator::playText(). When a chunk drains (the generator
 * goes idle AND has no more bytes pending), MorseModel::bumpTxSentBy()
 * advances the "sent" counter and the next chunk is pushed. When the
 * last chunk finishes, _txActive flips false.
 *
 * Cross-core note: this driver runs on Core 0 (the main loop). It reads
 * MorseGenerator::_charIdx via the relaxed-atomic charIndex() accessor
 * (Core 1 may be writing it during fillSamplesMono). It does NOT touch
 * MorseModel::_txSent directly during playback — the gen's
 * appendDecodedChar(fromPlayer=true) call is the authoritative path that
 * advances _txSent on Core 1 (atomic). The driver only reads _txSent at
 * chunk boundaries (between chunks) where no race exists.
 */

#include "tx_buffer.h"

#ifndef UNIT_TEST
#include "display_model.h"
#include "audio_engine.h"
#include "morse_generator.h"
#include "Log.h"
#else
#include "../test/mocks/arduino_mock.h"
#include "display_model.h"
#include "morse_generator.h"
#include "Log.h"
#endif

namespace TxBuffer {

namespace {
// Local copy of AudioEngine::morseGen() that hides the include for the
// host test build. Returns nullptr if the generator isn't up yet.
MorseGenerator* gen() {
#ifndef UNIT_TEST
    return AudioEngine::morseGen();
#else
    return nullptr;
#endif
}
}  // namespace

void beginSession() {
    // Idempotent. Sets the chunk boundary to (txSent, 0) so the first
    // call to poll() sees no chunk in flight and pushes the first chunk
    // fresh (no isContinuation prepend). Same effect as
    // MorseModel::startTx() but exposed as a free function so the
    // WinkeyBridge callback doesn't have to know about MorseModel.
    MorseModel::instance().startTx();
}

void poll() {
    auto& model = MorseModel::instance();

    // Quick exit when no session is active — the common path when the
    // device is idle or the operator hasn't clicked TX yet.
    if (!model.txActive()) {
        return;
    }

    MorseGenerator* g = gen();

    // Case 1: chunk in flight — update the caret visual only.
    if (g && g->isPlaying()) {
        size_t ci = g->charIndex();
        // The chunk's first char maps to buffer[txChunkStart]; the
        // gen's per-chunk _charIdx is offset from there.
        size_t head = model.txChunkStart() + ci;
        model.setTxHead(head);
        return;
    }

    // Case 2: chunk drained (or no chunk ever pushed). Advance _txSent
    // by the chunk length and check for completion.
    size_t chunkStart = model.txChunkStart();
    size_t chunkLen   = model.txChunkLen();
    if (chunkLen > 0) {
        // The chunk that was in flight is now done. Advance the sent
        // counter by the chunk's length. The MorseModel method also
        // resets txChunkStart=txSent and txChunkLen=0.
        model.bumpTxSentBy(chunkLen);
        Log::info("[TX] chunk drained: sent=%zu/%zu",
                  model.txSent(), model.txLen());
    }

    // Case 3: completion check.
    if (model.txSent() >= model.txLen()) {
        // Natural completion. Flip _txActive false and reset the
        // chunk boundary. The buffer text is preserved so the
        // operator can review what was sent.
        model.clearTxActive();
        Log::info("[TX] session complete: sent=%zu/%zu",
                  model.txSent(), model.txLen());
        return;
    }

    // Case 4: push the next chunk. Walk _txBuffer[txSent..txLen-1]
    // up to and including the next space (or end of buffer).
    const char* buf   = model.txBuffer();
    size_t      start  = model.txSent();
    size_t      len    = model.txLen();
    size_t      end    = len;
    for (size_t i = start; i < len; ++i) {
        if (buf[i] == ' ') {
            end = i + 1;   // include the trailing space
            break;
        }
    }
    size_t chunkLenNew = end - start;
    if (chunkLenNew == 0) {
        // Defensive — should not happen if txLen > txSent.
        Log::warning("[TX] zero-length chunk at start=%zu (txLen=%zu)",
                     start, len);
        model.clearTxActive();
        return;
    }

    // Copy the chunk into a stack buffer (NUL-terminated). The gen's
    // playText() expects a NUL-terminated string. Max chunk length is
    // bounded by kTxBufLen so a 256-byte local buffer is sufficient.
    char chunk[MorseModel::kTxBufLen];
    if (chunkLenNew >= sizeof(chunk)) {
        // Defensive truncation — shouldn't happen with the cap.
        chunkLenNew = sizeof(chunk) - 1;
    }
    memcpy(chunk, buf + start, chunkLenNew);
    chunk[chunkLenNew] = '\0';

    if (g) {
        g->playText(chunk);
    }
    model.setTxChunk(start, chunkLenNew);
    // _txHead moves to the first char of the new chunk (the gen will
    // adjust it from there as it advances).
    model.setTxHead(start);
    Log::info("[TX] chunk pushed: start=%zu len=%zu \"%.40s%s\"",
              start, chunkLenNew, chunk,
              chunkLenNew > 40 ? "..." : "");
}

}  // namespace TxBuffer
