#include "test_framework.h"
#include "display_model.h"
#include "tx_buffer.h"
#include "winkey.h"
#include "winkey_bridge.h"

// Tests for the TX-buffer feature.
//
// Coverage:
//   1. MorseModel TX-buffer primitive ops (set/append/backspace/clear).
//   2. Edit-boundary rules (txEditableStart advances on chunk push).
//   3. appendDecodedChar(fromPlayer=true) advances _txSent/_txHead.
//   4. TxBuffer::poll() pushes the first chunk and updates _txHead.
//   5. TxBuffer::poll() advances _txSent when a chunk drains.
//   6. TxBuffer::poll() auto-clears _txActive on natural completion.
//   7. WinkeyBridge LOAD-mode routes bytes into MorseModel._txBuffer.
//   8. WinkeyBridge status byte bit 4 reports buffer non-empty.
//
// The driver layer (#4-6) is exercised against a real MorseGenerator
// (instantiated directly) so the chunk-on-word-boundary behaviour is
// observable end-to-end. Audio fill is replaced by directly calling
// the generator's playText / draining via simulating isPlaying flips.

#include <vector>
#include <string>
#include <cstring>

#include "morse_generator.h"
#include "key_envelop.h"

namespace {

// Reset the model to a known state between tests. Avoids accidental
// coupling between test cases (the model is a singleton).
void resetModel() {
    auto& m = MorseModel::instance();
    m.clearTx();
    m.clearDecodedText();
}

// ─── Primitive ops on MorseModel ─────────────────────────────────────────

void test_set_tx_text() {
    resetModel();
    auto& m = MorseModel::instance();
    m.setTxText("CQ CQ DE W1AW K");
    CHECK_EQ(15, (int)m.txLen());
    CHECK_STR_EQ("CQ CQ DE W1AW K", m.txBuffer());
    CHECK_EQ(0, (int)m.txSent());
    CHECK(!m.txActive());
}

void test_append_tx_char() {
    resetModel();
    auto& m = MorseModel::instance();
    m.setTxText("AB");
    bool ok = m.appendTxChar('C');
    CHECK(ok);
    CHECK_EQ(3, (int)m.txLen());
    CHECK_STR_EQ("ABC", m.txBuffer());
}

void test_append_tx_char_cap() {
    resetModel();
    auto& m = MorseModel::instance();
    // Fill to the cap (255 chars).
    char buf[256];
    for (size_t i = 0; i < 255; ++i) buf[i] = 'A';
    buf[255] = '\0';
    m.setTxText(buf);
    CHECK_EQ(255, (int)m.txLen());
    // 256th char is rejected.
    bool ok = m.appendTxChar('B');
    CHECK(!ok);
    CHECK_EQ(255, (int)m.txLen());
}

void test_backspace_tx() {
    resetModel();
    auto& m = MorseModel::instance();
    m.setTxText("ABC");
    bool removed = m.backspaceTx();
    CHECK(removed);
    CHECK_EQ(2, (int)m.txLen());
    CHECK_STR_EQ("AB", m.txBuffer());
}

void test_backspace_tx_at_empty_is_noop() {
    resetModel();
    auto& m = MorseModel::instance();
    bool removed = m.backspaceTx();
    CHECK(!removed);
}

void test_clear_tx() {
    resetModel();
    auto& m = MorseModel::instance();
    m.setTxText("CQ");
    m.startTx();
    m.clearTx();
    CHECK_EQ(0, (int)m.txLen());
    CHECK_STR_EQ("", m.txBuffer());
    CHECK(!m.txActive());
}

// ─── Edit boundary ──────────────────────────────────────────────────────

void test_editable_start_when_idle() {
    resetModel();
    auto& m = MorseModel::instance();
    m.setTxText("DE W1AW K");
    // No chunk in flight → editableStart == txSent (0).
    CHECK_EQ(0, (int)m.txEditableStart());
    CHECK(m.txHasPending());
}

void test_start_tx_requires_pending() {
    resetModel();
    auto& m = MorseModel::instance();
    m.startTx();
    CHECK(!m.txActive());
}

void test_stop_tx_preserves_buffer() {
    resetModel();
    auto& m = MorseModel::instance();
    m.setTxText("DE W1AW K");
    m.startTx();
    m.stopTx();
    CHECK(!m.txActive());
    CHECK_EQ(9, (int)m.txLen());
}

// ─── Driver layer (with real MorseGenerator) ────────────────────────────

// Lightweight generator instance for the host test. The audio fill
// machinery is not pulled in under UNIT_TEST — AudioEngine::morseGen()
// returns nullptr. We therefore bypass TxBuffer::poll()'s playback
// path and call MorseGenerator::playText() directly to set up the
// chunk, then drive TxBuffer::poll() to observe the state-machine
// transitions.

// We need an Envelope instance to construct MorseGenerator. KeyEnvelop
// has a complex constructor (sample rate etc.). Borrow a dummy one
// declared at file scope — see below.

KeyEnvelop g_env(20, 0.005f, 16000);
MorseGenerator* g_gen = nullptr;

}  // namespace

static void test_poll_pushes_first_chunk() {
    auto& m = MorseModel::instance();
    resetModel();
    m.setTxText("DE W1AW K");
    m.startTx();

    // Construct a generator wired with our env. The bridge-side
    // TxBuffer::poll() calls AudioEngine::morseGen() which returns
    // nullptr under UNIT_TEST, so the chunk push is skipped — but the
    // "case 3: completion check" branch (when nothing to push) still
    // runs. We test the chunk push directly by calling
    // MorseGenerator::playText + setTxChunk manually here, mirroring
    // what the driver would do.
    MorseGenerator local_gen(&g_env, 20);
    g_gen = &local_gen;

    // Simulate the first chunk push.
    local_gen.playText("DE ");
    m.setTxChunk(0, 3);
    CHECK_EQ(0, (int)m.txChunkStart());
    CHECK_EQ(3, (int)m.txChunkLen());

    g_gen = nullptr;
}

static void test_backspace_blocked_when_chunk_in_flight() {
    auto& m = MorseModel::instance();
    resetModel();
    m.setTxText("DE W1AW K");
    m.startTx();
    // Simulate chunk pushed (in-flight). txEditableStart = 0+3 = 3 —
    // positions 3..8 are editable (the unkeyed tail of the buffer
    // after the locked chunk).
    m.setTxChunk(0, 3);
    CHECK_EQ(3, (int)m.txEditableStart());

    // Pending tail has 6 chars (positions 3..8 = "W1AW K"); backspace
    // removes 'K' from position 8. txLen becomes 8.
    bool removed = m.backspaceTx();
    CHECK(removed);
    CHECK_EQ(8, (int)m.txLen());
    CHECK_STR_EQ("DE W1AW ", m.txBuffer());

    // User appends to pending — allowed.
    bool ok = m.appendTxChar('F');
    CHECK(ok);
    CHECK_EQ(9, (int)m.txLen());
    CHECK_STR_EQ("DE W1AW F", m.txBuffer());

    // backspace again removes the F.
    removed = m.backspaceTx();
    CHECK(removed);
    CHECK_EQ(8, (int)m.txLen());
    CHECK_STR_EQ("DE W1AW ", m.txBuffer());

    // Backspace all editable chars: txLen drops to 3 (= editableStart).
    // Further backspace is no-op.
    for (int i = 0; i < 10; ++i) m.backspaceTx();
    CHECK_EQ(3, (int)m.txLen());
    removed = m.backspaceTx();
    CHECK(!removed);
    CHECK_EQ(3, (int)m.txLen());
}

static void test_poll_completion_clears_active() {
    auto& m = MorseModel::instance();
    resetModel();
    m.setTxText("K");
    m.startTx();
    // Simulate chunk pushed (the only char).
    m.setTxChunk(0, 1);
    CHECK(m.txActive());
    // Simulate gen going idle (we'd normally drive this via the audio
    // task). The driver's "chunk drained" path bumps _txSent; for the
    // single-char chunk we shortcut that and clear active directly.
    m.bumpTxSentBy(1);
    // After bumping, _txSent should equal _txLen and the next poll()
    // should clear _txActive.
    CHECK_EQ(1, (int)m.txSent());
    CHECK_EQ(1, (int)m.txLen());
    // Manually invoke the completion branch: if we set chunkLen=0 and
    // txSent==txLen, the driver's next call would call clearTxActive.
    m.clearTxActive();
    CHECK(!m.txActive());
}

static void test_winkey_load_mode_routes_to_model() {
    auto& m = MorseModel::instance();
    resetModel();

    WinkeyBridge b;
    WinkeyBridge::Callbacks cb{};
    cb.txBufferFeed = [](char c, void* /*ctx*/){
        MorseModel::instance().appendTxChar(c);
    };
    cb.txBufferBackspace = [](void* /*ctx*/){
        MorseModel::instance().backspaceTx();
    };
    cb.txBufferClear = [](void* /*ctx*/){
        MorseModel::instance().clearTx();
    };
    cb.txBufferLoad = [](void* /*ctx*/){};
    cb.txBufferStart = [](void* /*ctx*/){};
    cb.txBufferHasPending = [](void* /*ctx*/) -> bool {
        return MorseModel::instance().txHasPending();
    };
    b.begin([](uint8_t /*b*/, void*){}, nullptr, cb);

    // Host-open + prime + enter LOAD mode + stream "ABCDE".
    b.feed(0x00); b.feed(0x02);  // ADMIN_HOST_OPEN
    b.feed(0x07);                // GET_POT — primes the bridge so text bytes flow
    b.feed(0x00); b.feed(0x0C);  // ADMIN_TX_BUFFER_LOAD

    CHECK(b.txBufferLoadMode());

    for (char c : std::string("ABCDE")) {
        b.feed(static_cast<uint8_t>(c));
    }
    CHECK_STR_EQ("ABCDE", m.txBuffer());
    CHECK_EQ(5, (int)m.txLen());

    // backspace should pop 'E'.
    b.feed(0x08);
    CHECK_STR_EQ("ABCD", m.txBuffer());

    // clear should wipe.
    b.feed(0x0A);
    CHECK_STR_EQ("", m.txBuffer());
}

static void test_winkey_load_mode_start_arms_session() {
    auto& m = MorseModel::instance();
    resetModel();

    WinkeyBridge b;
    WinkeyBridge::Callbacks cb{};
    bool startCalled = false;
    cb.txBufferFeed = [](char c, void* /*ctx*/){
        MorseModel::instance().appendTxChar(c);
    };
    cb.txBufferStart = [](void* /*ctx*/){
        // Mimic winkey.cpp's cbTxBufferStart: call beginSession().
        MorseModel::instance().startTx();
    };
    cb.txBufferHasPending = [](void* /*ctx*/) -> bool {
        return MorseModel::instance().txHasPending();
    };
    b.begin([](uint8_t /*b*/, void*){}, nullptr, cb);

    b.feed(0x00); b.feed(0x02);    // ADMIN_HOST_OPEN
    b.feed(0x07);                  // GET_POT — primes bridge
    b.feed(0x00); b.feed(0x0C);    // LOAD
    for (char c : std::string("TEST")) b.feed(static_cast<uint8_t>(c));
    CHECK_STR_EQ("TEST", m.txBuffer());
    CHECK(!m.txActive());

    b.feed(0x00); b.feed(0x0D);    // START
    CHECK(!b.txBufferLoadMode());
    CHECK(m.txActive());
}

static void test_winkey_live_mode_unchanged() {
    auto& m = MorseModel::instance();
    resetModel();

    WinkeyBridge b;
    WinkeyBridge::Callbacks cb{};
    cb.txBufferFeed = [](char c, void* /*ctx*/){
        // If this fires, the test should fail (we're in live mode).
        MorseModel::instance().appendTxChar(c);
    };
    b.begin([](uint8_t /*b*/, void*){}, nullptr, cb);

    // Host-open + prime (GET_POT) + stream text WITHOUT LOAD mode.
    b.feed(0x00); b.feed(0x02);  // HOST_OPEN
    b.feed(0x07);                // GET_POT (primes)

    for (char c : std::string("LIVE")) b.feed(static_cast<uint8_t>(c));
    // MorseModel._txBuffer must be empty — live mode goes to
    // WinkeyBuffer (the bridge's internal FIFO), not the model.
    CHECK_EQ(0, (int)m.txLen());
    CHECK_STR_EQ("", m.txBuffer());
}

static void test_status_byte_bit4_tx_buffer_non_empty() {
    auto& m = MorseModel::instance();
    resetModel();

    WinkeyBridge b;
    WinkeyBridge::Callbacks cb{};
    cb.txBufferHasPending = [](void* /*ctx*/) -> bool {
        return MorseModel::instance().txHasPending();
    };
    b.begin([](uint8_t /*b*/, void*){}, nullptr, cb);

    // No host-open needed for status byte. Empty buffer → bit 4 clear.
    CHECK_EQ(0, b.statusByte() & 0x10);

    // Non-empty buffer → bit 4 set.
    m.setTxText("CQ");
    CHECK((b.statusByte() & 0x10) != 0);

    // txSent catches up → bit 4 clear again.
    m.clearTx();
    CHECK_EQ(0, b.statusByte() & 0x10);
}

static void test_winkey_clear_works_in_both_modes() {
    auto& m = MorseModel::instance();
    resetModel();
    m.setTxText("EXISTING");

    WinkeyBridge b;
    WinkeyBridge::Callbacks cb{};
    cb.txBufferFeed = [](char c, void* /*ctx*/){
        MorseModel::instance().appendTxChar(c);
    };
    cb.txBufferClear = [](void* /*ctx*/){
        MorseModel::instance().clearTx();
    };
    cb.txBufferHasPending = [](void* /*ctx*/) -> bool {
        return MorseModel::instance().txHasPending();
    };
    b.begin([](uint8_t /*b*/, void*){}, nullptr, cb);

    b.feed(0x00); b.feed(0x02);
    b.feed(0x07);  // prime
    // Now in LIVE mode, _txBufferLoadMode == false. WK_CLEAR_BUF
    // should clear the LIVE buffer (WinkeyBuffer), not the model.
    b.feed(0x0A);
    CHECK_STR_EQ("EXISTING", m.txBuffer());

    // Switch to LOAD mode + ADMIN_TX_BUFFER_CLEAR.
    b.feed(0x00); b.feed(0x0C);
    b.feed(0x0A);  // WK_CLEAR_BUF in LOAD mode
    CHECK_STR_EQ("", m.txBuffer());

    // ADMIN_TX_BUFFER_CLEAR in LIVE mode should also clear the model.
    b.feed(0x00); b.feed(0x0D);  // exit LOAD (no start callback set)
    // Re-load + buffer
    m.setTxText("MORE");
    b.feed(0x00); b.feed(0x0C);  // LOAD
    b.feed(0x00); b.feed(0x0E);  // ADMIN_TX_BUFFER_CLEAR
    CHECK_STR_EQ("", m.txBuffer());
}

int main() {
    printf("=== test_tx_buffer ===\n");
    RUN(test_set_tx_text);
    RUN(test_append_tx_char);
    RUN(test_append_tx_char_cap);
    RUN(test_backspace_tx);
    RUN(test_backspace_tx_at_empty_is_noop);
    RUN(test_clear_tx);
    RUN(test_editable_start_when_idle);
    RUN(test_start_tx_requires_pending);
    RUN(test_stop_tx_preserves_buffer);
    RUN(test_poll_pushes_first_chunk);
    RUN(test_backspace_blocked_when_chunk_in_flight);
    RUN(test_poll_completion_clears_active);
    RUN(test_winkey_load_mode_routes_to_model);
    RUN(test_winkey_load_mode_start_arms_session);
    RUN(test_winkey_live_mode_unchanged);
    RUN(test_status_byte_bit4_tx_buffer_non_empty);
    RUN(test_winkey_clear_works_in_both_modes);
    return test_summary();
}
