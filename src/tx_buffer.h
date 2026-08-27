#pragma once
/**
 * tx_buffer.h — TX-buffer driver.
 *
 * Owns the playback of MorseModel::_txBuffer through MorseGenerator. Called
 * every loop() iteration from main.cpp (next to Winkey::poll()). Stateless
 * on the module level — all state lives in MorseModel.
 *
 * Two entry points share the same buffer (single source of truth):
 *   - Web UI    : HTTP /api/tx/start + /api/tx/edit (writes to
 *                 MorseModel::_txBuffer via setTxText / appendTxChar /
 *                 backspaceTx) and POST /api/tx/start to arm.
 *   - WinKey    : ADMIN_TX_BUFFER_LOAD + text bytes + ADMIN_TX_BUFFER_START
 *                 (routed through WinkeyBridge callbacks to MorseModel,
 *                 see winkey.cpp::cbTxBufferFeed et al.).
 *
 * Both call beginSession() (host-START or web-TX-button) which sets
 * txActive=true and resets the chunk boundary to (txSent, 0). From there
 * poll() drives chunk-on-word-boundary playback.
 *
 * Edit boundary semantics (see docs/tx_buffer.md § 6):
 *   - No chunk in flight → operator can edit buffer[txSent..txLen-1].
 *   - Chunk in flight   → operator can only edit buffer[chunkEnd..txLen-1].
 *     The current chunk's chars (keyed + unkeyed) are locked.
 *   - Edit attempts outside the editable region are silently rejected by
 *     MorseModel::backspaceTx() / HTTP validation.
 *
 * See docs/tx_buffer.md for the full design.
 */
class MorseGenerator;   // forward — full include lives in tx_buffer.cpp

class MorseModel;

namespace TxBuffer {

/// Arm a TX session. Idempotent: no-op if already active or nothing to send.
/// Called by both WinkeyBridge::cbTxBufferStart (host-START admin sub-command)
/// and the HTTP /api/tx/start handler.
void beginSession();

/// Per-loop pump. Advances the chunk-in-flight state machine:
///   idle                  → nothing
///   chunk in flight       → update _txHead (caret visual)
///   chunk just drained    → bump _txSent, push next chunk
///   chunk drained + done  → clear _txActive, log "[TX] session complete"
void poll();

}  // namespace TxBuffer
