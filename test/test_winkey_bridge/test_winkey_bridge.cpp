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
};

Spy g_spy;

void spyOut(uint8_t b, void*)       { g_spy.out.push_back(b); }
void spyWpm(int w, void*)           { g_spy.lastWpm = w; }
void spySidetoneHz(int hz, void*)   { g_spy.lastSidetone = hz; }
void spyOutEn(bool on, void*)       { g_spy.lastOutputEn = on; ++g_spy.outEnCnt; }
void spySend(const char* t, void*)  { g_spy.lastSend = t; ++g_spy.sendCnt; }
void spyStop(void*)                 { ++g_spy.stopCnt; }

WinkeyBridge makeBridge() {
    g_spy = Spy{};
    WinkeyBridge b;
    WinkeyBridge::Callbacks cb;
    cb.setWpm          = &spyWpm;
    cb.setSidetoneHz   = &spySidetoneHz;
    cb.setOutputEnable = &spyOutEn;
    cb.sendText        = &spySend;
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

// ─── Reset (admin 0x01) restores defaults and closes ────────────────────
static void test_reset_restores_and_closes() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x02); b.feed(40);
    CHECK_EQ(b.wpm(), 40);
    b.feed(0x00); b.feed(0x01);  // admin reset
    CHECK_EQ(b.wpm(), 20);
    CHECK(!b.isOpen());
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
    RUN(test_reset_restores_and_closes);
    RUN(test_ptt_times_consumes_two_params);
    return test_summary();
}
