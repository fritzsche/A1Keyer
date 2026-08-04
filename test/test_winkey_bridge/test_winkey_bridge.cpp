// test_winkey_bridge.cpp — host unit tests for the WinKeyer 2.x bridge.
//
// Verifies the real K1EL byte protocol (command codes 0x00-0x1F, text
// >= 0x20 sent as CW, admin host-open, echo/status). The bridge takes
// bytes via feed() and emits via an OutputFn, with side effects through
// a Callbacks struct, so tests use spy callbacks — no ESP32/audio dep.

#include "test_framework.h"
#include "winkey_bridge.h"
#include <string>
#include <vector>

namespace {

struct Spy {
    std::vector<uint8_t> out;      // bytes the bridge emitted to the host
    int  lastWpm      = -1;
    int  lastSidetone = -1;
    int  outEnCnt     = 0;  bool lastOutputEn = true;
    int  sendCnt      = 0;  std::string lastSend;
    int  stopCnt      = 0;
    int  canAcceptCnt = 0;         // canAcceptText() probe count
    bool canAccept    = true;      // return value for canAcceptText()
};

Spy g_spy;

void spyOut(uint8_t b, void*)       { g_spy.out.push_back(b); }
void spyWpm(int w, void*)           { g_spy.lastWpm = w; }
void spySidetoneHz(int hz, void*)   { g_spy.lastSidetone = hz; }
void spyOutEn(bool on, void*)       { g_spy.lastOutputEn = on; ++g_spy.outEnCnt; }
void spySend(const char* t, void*)  { g_spy.lastSend = t; ++g_spy.sendCnt; }
void spyStop(void*)                 { ++g_spy.stopCnt; }
bool spyCanAccept(void*)            { ++g_spy.canAcceptCnt; return g_spy.canAccept; }

WinkeyBridge makeBridge() {
    g_spy = Spy{};
    WinkeyBridge b;
    WinkeyBridge::Callbacks cb;
    cb.setWpm          = &spyWpm;
    cb.setSidetoneHz   = &spySidetoneHz;
    cb.setOutputEnable = &spyOutEn;
    cb.sendText        = &spySend;
    cb.canAcceptText   = &spyCanAccept;
    cb.stopSending     = &spyStop;
    b.begin(&spyOut, nullptr, cb);
    return b;
}

// Host-open handshake, then clear captured output.
void open(WinkeyBridge& b) {
    b.feed(0x00); b.feed(0x02);   // Host Open → version reply
    b.feed(0x00); b.feed(0x0B);   // Enter WK2 status mode
    g_spy.out.clear();
}

void feedText(WinkeyBridge& b, const std::string& s) {
    for (char c : s) b.feed((uint8_t)c);
}

}  // namespace

// ─── Host open ──────────────────────────────────────────────────────────
static void test_host_open_returns_version() {
    WinkeyBridge b = makeBridge();
    CHECK(!b.isOpen());
    b.feed(0x00);
    b.feed(0x02);
    CHECK(b.isOpen());
    // Admin/param bytes are NOT echoed; only the version byte comes back.
    CHECK_EQ((int)g_spy.out.size(), 1);
    CHECK_EQ((int)g_spy.out.back(), (int)WinkeyBridge::kVersion);  // 0x17
}

static void test_commands_ignored_before_open() {
    WinkeyBridge b = makeBridge();
    b.feed(0x02); b.feed(25);   // speed before open → ignored
    CHECK_EQ(g_spy.lastWpm, -1);
}

static void test_text_ignored_before_open() {
    WinkeyBridge b = makeBridge();
    feedText(b, "CQ");
    b.poll();
    CHECK_EQ(g_spy.sendCnt, 0);
}

// ─── Speed (0x02) ─────────────────────────────────────────────────────────
static void test_speed_command() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x02); b.feed(25);
    CHECK_EQ(g_spy.lastWpm, 25);
    CHECK_EQ(b.wpm(), 25);
}

static void test_speed_clamps_high() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x02); b.feed(200);   // above WK max 99
    CHECK_EQ(b.wpm(), 99);
}

static void test_speed_zero_uses_pot_no_change() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x02); b.feed(0);     // 0 = use pot → no WPM change / no hook
    CHECK_EQ(g_spy.lastWpm, -1);
    CHECK_EQ(b.wpm(), 20);       // unchanged default
}

// ─── Sidetone (0x01) ──────────────────────────────────────────────────────
static void test_sidetone_preset() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x01); b.feed(5);     // preset index 5 → 800 Hz
    CHECK_EQ(b.sidetoneHz(), 800);
    CHECK_EQ(g_spy.lastSidetone, 800);
}

// ─── Text is sent as CW (>= 0x20), not interpreted as commands ──────────
static void test_text_sent_as_cw() {
    WinkeyBridge b = makeBridge();
    open(b);
    feedText(b, "CQ TEST");      // includes 'S'(0x53), 'T'(0x54) — TEXT
    b.poll();
    CHECK_EQ(g_spy.sendCnt, 1);
    CHECK_STR_EQ(g_spy.lastSend.c_str(), "CQ TEST");
    // A letter that is ALSO a low command value is impossible here since
    // all letters are >= 0x41; this confirms they route to text.
}

static void test_lowercase_uppercased() {
    WinkeyBridge b = makeBridge();
    open(b);
    feedText(b, "cq");
    b.poll();
    CHECK_STR_EQ(g_spy.lastSend.c_str(), "CQ");
}

// ─── Command bytes are NOT echoed; only replies are emitted ─────────────
static void test_command_bytes_not_echoed() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x02); b.feed(25);    // speed command + param
    CHECK_EQ((int)g_spy.out.size(), 0);   // nothing echoed for a command
}

// ─── Backspace (0x08) and clear (0x0A) ──────────────────────────────────
static void test_backspace_removes_last_char() {
    WinkeyBridge b = makeBridge();
    open(b);
    feedText(b, "CQX");
    b.feed(0x08);                // backspace removes 'X'
    b.poll();
    CHECK_STR_EQ(g_spy.lastSend.c_str(), "CQ");
}

static void test_clear_buffer_stops() {
    WinkeyBridge b = makeBridge();
    open(b);
    feedText(b, "CQ");
    b.feed(0x0A);                // clear buffer → stopSending, empty
    CHECK_EQ(g_spy.stopCnt, 1);
    b.poll();
    CHECK_EQ(g_spy.sendCnt, 0);  // nothing to send
}

// ─── Status request (0x15) replies with a tagged status byte ────────────
static void test_status_request_replies() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x15);
    CHECK_EQ((int)g_spy.out.size(), 1);
    CHECK_EQ((int)(g_spy.out.back() & 0xE0), 0xC0);  // 3-MSB tag 110
}

// ─── Key immediate (0x0B) routes to output-enable ───────────────────────
static void test_key_immediate() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x0B); b.feed(1);     // key down (tune)
    CHECK(g_spy.lastOutputEn);
    b.feed(0x0B); b.feed(0);     // key up
    CHECK(!g_spy.lastOutputEn);
}

// ─── Reset (admin 0x01) restores defaults but keeps the host open ─────
// Real-world hosts (N1MM, RUMlogNG, fldigi, WriteLog) routinely send a
// defensive reset as part of their init sequence and then immediately
// follow up with GetPot / ReqStatus / text. The K1EL WK2 datasheet
// technically requires the host to re-open after reset, but every
// shipping WK2 emulator (K3NG, hamlib winkey.c) chooses to stay open
// because in practice no logger actually re-opens — see
// docs/winkey.md § 5.1 and the trace in the bug that motivated this
// test (RUMlogNG: "Interface is not available" after version 23).
static void test_reset_restores_defaults_keeps_open() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x02); b.feed(40);                // change WPM from default
    b.feed(0x00); b.feed(0x0B);              // enter WK2 mode
    b.feed(0x01); b.feed(5);                 // change sidetone preset 5
    CHECK_EQ(b.wpm(), 40);
    CHECK_EQ(b.sidetoneHz(), 800);

    b.feed(0x00); b.feed(0x01);              // admin reset

    // Defaults restored.
    CHECK_EQ(b.wpm(), 20);
    CHECK_EQ(b.sidetoneHz(), 600);

    // Bridge STAYS open — the host's defensive reset must not close
    // the interface.
    CHECK(b.isOpen());

    // Subsequent commands must still be processed: a SetSpeed after
    // reset should still update WPM, proving the parser is wired back
    // up and the bridge is alive.
    b.feed(0x02); b.feed(25);
    CHECK_EQ(b.wpm(), 25);

    // Text after reset must still echo (not be dropped as "before open").
    feedText(b, "CQ");
    b.poll();
    CHECK_STR_EQ(g_spy.lastSend.c_str(), "CQ");
}

// ─── resetForTest() still forces _open=false ──────────────────────────
// resetForTest() is the test-suite entry point; it must still force
// _open=false so each test starts from a clean slate. This guards
// against future refactors that might collapse resetForTest and
// resetParams into one path.
static void test_reset_for_test_still_closes() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.resetForTest();
    CHECK(!b.isOpen());
    CHECK_EQ(b.wpm(), 20);
}

// ─── canAcceptText gates poll() so back-to-back text accumulates ─────
// Hosts stream text faster than the audio player can key it. The
// bridge must NOT restart playback on every byte; instead it
// accumulates text and the next idle poll drains one bigger chunk.
// This test mimics the RumlogNG scenario: text arrives in three
// bursts while canAccept=false, then canAccept=true → single drain.
static void test_poll_skips_when_consumer_busy() {
    WinkeyBridge b = makeBridge();
    open(b);
    g_spy.canAccept = false;            // consumer mid-playback
    feedText(b, "C");                    // char 1 → buffered
    b.poll();
    CHECK_EQ(g_spy.sendCnt, 0);          // no drain while busy
    feedText(b, "Q");                    // char 2 → still buffered
    b.poll();
    CHECK_EQ(g_spy.sendCnt, 0);
    feedText(b, " TEST");                // chars 3..7 → still buffered
    b.poll();
    CHECK_EQ(g_spy.sendCnt, 0);
    // Consumer finishes; bridge must now drain the FULL accumulated
    // chunk in one sendText() call.
    g_spy.canAccept = true;
    b.poll();
    CHECK_EQ(g_spy.sendCnt, 1);
    CHECK_STR_EQ(g_spy.lastSend.c_str(), "CQ TEST");
}

// ─── canAcceptText is consulted before each drain attempt ────────────
// After a successful drain the bridge must still probe before the
// NEXT drain — i.e. the busy flag is re-checked every poll().
static void test_poll_consults_can_accept_each_cycle() {
    WinkeyBridge b = makeBridge();
    open(b);
    feedText(b, "A");
    int probesBefore = g_spy.canAcceptCnt;
    g_spy.canAccept = true;
    b.poll();                            // drains "A"
    CHECK(g_spy.canAcceptCnt > probesBefore);
    feedText(b, "B");                    // buffer non-empty
    int probesAfter = g_spy.canAcceptCnt;
    g_spy.canAccept = false;            // consumer busy again
    b.poll();                            // must probe AND skip
    CHECK(g_spy.canAcceptCnt > probesAfter);
    CHECK_EQ(g_spy.sendCnt, 1);          // still the original "A" drain
    CHECK_STR_EQ(g_spy.lastSend.c_str(), "A");
}

// ─── Multi-param command (PTT times, 0x04) consumes both params ─────────
static void test_ptt_times_consumes_two_params() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x04); b.feed(10); b.feed(25);   // lead=10, tail=25
    // After both params, parser returns to idle: a following text byte
    // must be treated as text, not swallowed as a stray param.
    feedText(b, "A");
    b.poll();
    CHECK_STR_EQ(g_spy.lastSend.c_str(), "A");
}

int main() {
    RUN(test_host_open_returns_version);
    RUN(test_commands_ignored_before_open);
    RUN(test_text_ignored_before_open);
    RUN(test_speed_command);
    RUN(test_speed_clamps_high);
    RUN(test_speed_zero_uses_pot_no_change);
    RUN(test_sidetone_preset);
    RUN(test_text_sent_as_cw);
    RUN(test_lowercase_uppercased);
    RUN(test_command_bytes_not_echoed);
    RUN(test_backspace_removes_last_char);
    RUN(test_clear_buffer_stops);
    RUN(test_status_request_replies);
    RUN(test_key_immediate);
    RUN(test_reset_restores_defaults_keeps_open);
    RUN(test_reset_for_test_still_closes);
    RUN(test_poll_skips_when_consumer_busy);
    RUN(test_poll_consults_can_accept_each_cycle);
    RUN(test_ptt_times_consumes_two_params);
    return test_summary();
}
