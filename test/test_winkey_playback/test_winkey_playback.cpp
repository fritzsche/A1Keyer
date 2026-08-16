#include "test_framework.h"
#include "winkey.h"
#include "winkey_bridge.h"

// The meaningful logic of Winkey::playLocalMemoryText (open vs.
// closed branch, busy-gate, mode flip) lives inside winkey.cpp
// where it touches AudioEngine / MorseGenerator / the bridge's
// real singleton. UNIT_TEST builds compile playLocalMemoryText
// as a no-op stub, so this suite can only assert the layer below
// the facade:
//
//   1. The header is includable and the method is linkable in
//      UNIT_TEST.
//   2. The UNIT_TEST stub honours the documented early-return
//      contract for null / empty input — i.e. it never crashes
//      regardless of caller behaviour.
//   3. The actual host-driven multi-byte playback path inside
//      WinkeyBridge is exercised here too: feeding a multi-byte
//      ASCII phrase through feed() then poll() MUST produce
//      exactly one cbSendText invocation with the concatenated
//      text. This is the path the host-facing playback uses,
//      and playLocalMemoryText reuses it.
//
// Real-device coverage for the open vs. closed branch + radio
// keying wiring lives in the on-device smoke test in
// docs/memory.md §Verification.

static void test_stub_does_not_crash_on_null() {
    // First-class invariant: a stray pointer (e.g. pressing a digit
    // for an empty slot) must not crash the device.
    Winkey::playLocalMemoryText(nullptr);
    CHECK(true); // survived the call
}

static void test_stub_does_not_crash_on_empty_string() {
    Winkey::playLocalMemoryText("");
    CHECK(true);
}

static void test_stub_does_not_crash_on_long_string() {
    // The contract says cap is kMemLen-1 on device; in UNIT_TEST any
    // input must be safe.
    const char* phrases[] = {
        "CQ CQ DE W1AW K",
        "5NN/B",
        "TU 5NN 001",
        "<ar>",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789",
    };
    for (auto p : phrases) {
        Winkey::playLocalMemoryText(p);
    }
    CHECK(true);
}

// ─── Multi-byte send test (the actual open-branch path) ────────────────────
//
// This is the code path that the open-branch of
// Winkey::playLocalMemoryText traverses: feed(char) for every byte,
// then poll() to drain. We exercise it directly against the bridge
// with spy callbacks — the same harness style as test_winkey_bridge.

#include <vector>
#include <string>

namespace {

struct SendSpy {
    std::vector<std::string> sentChunks;
    int canAcceptCalls = 0;
    bool canAccept     = true;
};

SendSpy g_send;

void spyOut(uint8_t /*b*/, void*) {}
void spyWpm(int /*w*/, void*) {}
void spySidetoneHz(int /*h*/, void*) {}
void spyOutEn(bool /*on*/, void*) {}
void spySend(const char* t, void*) {
    g_send.sentChunks.emplace_back(t ? t : "");
}
void spyStop(void*) {}
bool spyCanAccept(void*) {
    ++g_send.canAcceptCalls;
    return g_send.canAccept;
}

WinkeyBridge makePrimedBridge() {
    g_send = SendSpy{};
    WinkeyBridge b;
    WinkeyBridge::Callbacks cb;
    cb.setWpm          = &spyWpm;
    cb.setSidetoneHz   = &spySidetoneHz;
    cb.setOutputEnable = &spyOutEn;
    cb.sendText        = &spySend;
    cb.canAcceptText   = &spyCanAccept;
    cb.stopSending     = &spyStop;
    b.begin(&spyOut, nullptr, cb);

    // Host open → version, then enter status mode, then GET_POT to
    // prime. Mirrors the helper in test_winkey_bridge.
    b.feed(0x00); b.feed(0x02);
    b.feed(0x00); b.feed(0x0B);
    b.feed(0x07);
    return b;
}

}  // namespace

static void test_feed_then_poll_emits_single_concatenated_chunk() {
    WinkeyBridge b = makePrimedBridge();
    g_send.sentChunks.clear();

    // This is the same byte stream the memory-keyer playLocalMemoryText
    // produces for the slot "CQ CQ DE W1AW K" with a host attached.
    const char* phrase = "CQ CQ DE W1AW K";
    for (const char* p = phrase; *p; ++p) {
        b.feed(static_cast<uint8_t>(*p));
    }
    b.poll();

    CHECK_EQ(1, (int)g_send.sentChunks.size());
    CHECK_STR_EQ(phrase, g_send.sentChunks.front().c_str());
}

static void test_feed_then_poll_with_pause_in_middle() {
    // Two chunks separated by inter-word spacing (the bridge
    // accumulates FIFO bytes between polls).
    WinkeyBridge b = makePrimedBridge();
    g_send.sentChunks.clear();
    g_send.canAccept = true;

    for (const char* p = "TU"; *p; ++p) {
        b.feed(static_cast<uint8_t>(*p));
    }
    b.poll();
    CHECK_EQ(1, (int)g_send.sentChunks.size());

    for (const char* p = " 5NN"; *p; ++p) {
        b.feed(static_cast<uint8_t>(*p));
    }
    b.poll();

    CHECK_EQ(2, (int)g_send.sentChunks.size());
    CHECK_STR_EQ("TU",   g_send.sentChunks[0].c_str());
    CHECK_STR_EQ(" 5NN", g_send.sentChunks[1].c_str());
}

// Empty / whitespace inputs the bridge should round-trip without
// emitting sendText (the bridge already tests this internally;
// re-asserting here pins the contract from the memory-keyer side).
static void test_empty_phrase_emits_nothing() {
    WinkeyBridge b = makePrimedBridge();
    g_send.sentChunks.clear();
    // No feed() before poll() — equivalent to an empty slot in the
    // memory-keyer pipeline (we early-return before touching the
    // bridge, so this is a sanity check that an idle bridge stays
    // silent).
    b.poll();
    CHECK_EQ(0, (int)g_send.sentChunks.size());
}

int main() {
    printf("=== test_winkey_playback ===\n");
    RUN(test_stub_does_not_crash_on_null);
    RUN(test_stub_does_not_crash_on_empty_string);
    RUN(test_stub_does_not_crash_on_long_string);

    RUN(test_feed_then_poll_emits_single_concatenated_chunk);
    RUN(test_feed_then_poll_with_pause_in_middle);
    RUN(test_empty_phrase_emits_nothing);
    return test_summary();
}
