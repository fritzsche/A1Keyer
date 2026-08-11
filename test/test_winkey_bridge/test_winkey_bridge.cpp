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
    // Probe so the bridge primes its text gate. Real hosts (RUMlogNG,
    // N1MM) send GET_POT and/or REQ_STATUS as part of their init; the
    // spy tests that follow need text bytes to flow normally, so we
    // prime here. See docs/winkey.md § 13.9.
    b.feed(0x07);                 // GET_POT → 0x80 reply, _primed = true
    g_spy.out.clear();
}

// Host-open WITHOUT priming — used by tests that probe the unprimed
// state explicitly (e.g. verifying that text bytes arriving before
// the host's first GET_POT/REQ_STATUS are swallowed).
void openUnprimed(WinkeyBridge& b) {
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
    // Real hosts re-probe after a defensive reset, so we re-prime here.
    b.feed(0x07);                                // GET_POT → re-prime
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

// ─── Primed gate: text bytes received before the host has probed us ─────
//
// RUMlogNG's init sequence streams a stray text byte ('D' = 0x44) from
// its outgoing-CW buffer between the Host-Open reply and the first
// GET_POT / REQ_STATUS probe. Without the primed gate that byte lands
// in the send buffer and is keyed as CW at boot — the user hears "D"
// without having typed anything. The gate suppresses text until we've
// answered at least one probe query. See docs/winkey.md § 13.9.

static void test_text_ignored_before_primed() {
    WinkeyBridge b = makeBridge();
    openUnprimed(b);
    CHECK(b.isOpen());
    CHECK(!b.isPrimed());
    feedText(b, "D");
    b.poll();
    CHECK_EQ(g_spy.sendCnt, 0);   // NOT sent
}

static void test_text_accepted_after_get_pot() {
    WinkeyBridge b = makeBridge();
    openUnprimed(b);
    b.feed(0x07);                 // GET_POT → 0x80 reply, primed
    CHECK(b.isPrimed());
    feedText(b, "A");
    b.poll();
    CHECK_STR_EQ(g_spy.lastSend.c_str(), "A");
}

static void test_text_accepted_after_req_status() {
    WinkeyBridge b = makeBridge();
    openUnprimed(b);
    b.feed(0x15);                 // REQ_STATUS → statusByte reply, primed
    CHECK(b.isPrimed());
    feedText(b, "A");
    b.poll();
    CHECK_STR_EQ(g_spy.lastSend.c_str(), "A");
}

static void test_rumlog_init_does_not_play_text() {
    // Replay the byte sequence RUMlogNG streams at open. The 0x44
    // ('D') that arrives between sidetone and pinconfig must NOT be
    // keyed — only the "A" the user types later (after GET_POT has
    // primed us) should reach the send hook.
    WinkeyBridge b = makeBridge();
    openUnprimed(b);
    // RUMlogNG init: sidetone, then a stray admin+text pair,
    // then pinconfig + the rest of the parameter commands, then
    // GET_POT and REQ_STATUS.
    b.feed(0x01); b.feed(0x10);   // sidetone
    b.feed(0x00); b.feed(0x0E); b.feed(0x44); // admin 14 + leaked 'D'
    b.feed(0x09); b.feed(0x04);   // pinconfig
    b.feed(0x05); b.feed(0x12); b.feed(0x16); b.feed(0x00); // set-pot
    b.feed(0x03); b.feed(0x32);   // weighting
    b.feed(0x0D); b.feed(0x1A);   // farnsworth
    b.feed(0x04); b.feed(0x00); b.feed(0x00); // PTT times
    b.feed(0x11); b.feed(0x00);   // key comp
    b.feed(0x17); b.feed(0x32);   // ratio
    b.feed(0x07);                 // GET_POT → primed
    CHECK(b.isPrimed());
    // User-typed text AFTER the probe:
    feedText(b, "A");
    b.poll();
    CHECK_STR_EQ(g_spy.lastSend.c_str(), "A");
}

static void test_reset_clears_primed() {
    // After a soft reset the host is expected to re-probe; the gate
    // must drop back to "unprimed" so any text the host accidentally
    // sends during its reset-init sequence is again swallowed.
    WinkeyBridge b = makeBridge();
    open(b);
    CHECK(b.isPrimed());
    b.feed(0x00); b.feed(0x01);   // soft reset
    CHECK(!b.isPrimed());
    feedText(b, "X");
    b.poll();
    CHECK_EQ(g_spy.sendCnt, 0);
}

// ─── Bidirectional WPM sync (docs/winkey.md § 16.7) ────────────────────
//
// A1Keyer synthesises the K1EL physical speed pot using the
// `(wpm - pot_wpm_low_value) | 0x80` encoding — the same single-byte
// idiom K3NG uses for both the live pin-event (k3ng_keyer.ino:5730)
// and the GET_POT reply (k3ng_keyer.ino:11788). The 0x80 bit is the
// documented "speed pot changed" indicator; the low 7 bits carry the
// offset from the pot-low WPM. Hosts (RUMlogNG, N1MM, fldigi) decode
// via `wpm = low + (byte & 0x7F)`. At open, the bridge also replies to
// admin 7 (Get Values) with the persisted WPM. These tests pin both
// halves of the sync.

static void test_setWpmFromLocal_emits_pin_event_when_open() {
    WinkeyBridge b = makeBridge();
    open(b);
    size_t before = g_spy.out.size();
    b.setWpmFromLocal(25);
    // One byte: (wpm - low) | 0x80 = (25-5) | 0x80 = 20 | 0x80 = 0x94.
    CHECK_EQ((int)(g_spy.out.size() - before), 1);
    CHECK_EQ((int)g_spy.out[before], 0x94);
    CHECK_EQ(b.wpm(), 25);
}

static void test_setWpmFromLocal_silent_before_open() {
    WinkeyBridge b = makeBridge();
    size_t before = g_spy.out.size();
    b.setWpmFromLocal(25);
    // No host-open → no emit (the host isn't listening). The bridge
    // still caches the value so a later emit / Get Values reply sees it.
    CHECK_EQ((int)(g_spy.out.size() - before), 0);
    CHECK_EQ(b.wpm(), 25);
}

static void test_setWpmFromLocal_no_emit_when_value_unchanged() {
    WinkeyBridge b = makeBridge();
    open(b);
    b.setWpmFromLocal(25);            // first push
    size_t afterFirst = g_spy.out.size();
    b.setWpmFromLocal(25);            // same value → no second emit
    CHECK_EQ((int)(g_spy.out.size() - afterFirst), 0);
    b.setWpmFromLocal(26);            // change → push
    CHECK_EQ((int)(g_spy.out.size() - afterFirst), 1);
}

static void test_setWpmFromLocal_after_host_setSpeed_no_echo_feedback() {
    // Host sends 0x02 30 → bridge mirrors to MorseModel. The next
    // setWpmFromLocal(30) (which the polling syncWpmFromLocal would
    // invoke after observing the changeCounter tick) must NOT emit
    // because _lastPushedWpm is stamped in the WK_SPEED handler.
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x02); b.feed(30);
    size_t afterCmd = g_spy.out.size();
    b.setWpmFromLocal(30);
    CHECK_EQ((int)(g_spy.out.size() - afterCmd), 0);
}

static void test_setWpmFromLocal_after_host_setSpeed_with_different_value_pushes() {
    // Operator overrides host-set speed on the keyer side: the new
    // value diverges from _lastPushedWpm so the bridge re-emits.
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x02); b.feed(30);                 // host sets 30
    size_t afterCmd = g_spy.out.size();
    b.setWpmFromLocal(35);                     // operator overrides to 35
    CHECK_EQ((int)(g_spy.out.size() - afterCmd), 1);
    CHECK_EQ((int)g_spy.out[afterCmd], 0x9E);   // (35-5) | 0x80 = 30 | 0x80 = 0x9E
}

// Pin the WPM→byte encoding for the default pot range [5, 50]. WPM
// values 5/10/20/27/30/40/50 → bytes 0x80/0x85/0x8F/0x96/0x99/0xA3/0xAD.
static void test_speed_pot_value_formula_table() {
    WinkeyBridge b = makeBridge();
    open(b);
    int table[][2] = {
        {5,   0x80},
        {10,  0x85},
        {20,  0x8F},
        {27,  0x96},
        {30,  0x99},
        {40,  0xA3},
        {50,  0xAD},
    };
    for (auto& row : table) {
        size_t before = g_spy.out.size();
        b.setWpmFromLocal(row[0]);
        CHECK_EQ((int)(g_spy.out.size() - before), 1);
        CHECK_EQ((int)g_spy.out[before], row[1]);
    }
}

static void test_speed_pot_value_clamps_input() {
    // WPM outside the wire-facing [5, 99] range saturates rather than
    // wraps. The byte is `(wpm - low) | 0x80` with low=5, so the
    // bottom of the range is 0x80 and the top is 0x80 | (99-5) = 0xDE.
    WinkeyBridge b = makeBridge();
    open(b);
    size_t before = g_spy.out.size();
    b.setWpmFromLocal(0);     // below min → clamp to 5  → offset 0  → 0x80
    CHECK_EQ((int)g_spy.out[before], 0x80);
    b.setWpmFromLocal(200);   // above max → clamp to 99 → offset 94 → 0xDE
    CHECK_EQ((int)g_spy.out[before + 1], 0xDE);
}

static void test_get_pot_returns_wpm_pot_value() {
    // 0x07 GET_POT reply uses the same (wpm - low) | 0x80 encoding as
    // the live pin-event and the admin 7 byte-offset-1 reply. The
    // old behaviour was a constant 0x80 sentinel — verify that's gone.
    WinkeyBridge b = makeBridge();
    open(b);
    size_t before = g_spy.out.size();
    b.feed(0x07);                          // GET_POT
    CHECK_EQ((int)(g_spy.out.size() - before), 1);
    CHECK_EQ((int)g_spy.out[before], 0x8F);   // default WPM=20 → 0x80|15
}

static void test_get_pot_reflects_local_change() {
    // After setWpmFromLocal moves the WPM, GET_POT reflects the new
    // value. This is what hosts (RUMlogNG hamlib, custom Mac tools)
    // see when they poll at any time after open.
    WinkeyBridge b = makeBridge();
    open(b);
    b.setWpmFromLocal(35);                 // _wpm = 35
    size_t before = g_spy.out.size();
    b.feed(0x07);                          // GET_POT
    CHECK_EQ((int)g_spy.out[before], 0x9E);   // (35-5) | 0x80 = 0x9E
}

static void test_admin_get_values_emits_14_bytes() {
    // K1EL WK2 datasheet v23 Table 14: Get Values (admin 7) replies
    // with 14 bytes. Only the WPM byte (offset 1) is filled with a
    // real value; the rest are zero-filled.
    WinkeyBridge b = makeBridge();
    open(b);
    size_t before = g_spy.out.size();
    b.feed(0x00); b.feed(0x07);    // admin Get Values
    CHECK_EQ((int)(g_spy.out.size() - before), 14);
}

static void test_admin_get_values_second_byte_is_wpm() {
    // After open, default WPM is 20. Get Values reply byte index 1
    // must carry that value.
    WinkeyBridge b = makeBridge();
    open(b);
    size_t before = g_spy.out.size();
    b.feed(0x00); b.feed(0x07);
    CHECK_EQ((int)g_spy.out[before + 1], 20);
    // Change WPM, re-probe, verify the reply updates.
    b.feed(0x02); b.feed(35);
    size_t before2 = g_spy.out.size();
    b.feed(0x00); b.feed(0x07);
    CHECK_EQ((int)g_spy.out[before2 + 1], 35);
}

static void test_set_pot_configures_range() {
    // `0x05 <low> <range> <scale>` reconfigures the pot range. The
    // byte encoding is `(wpm - low) | 0x80` — same formula, new
    // low. The default in resetForTest is [5, 50]; after set pot
    // [18, 40] (RUMlogNG's init), WPM=23 → (23-18)|0x80 = 0x85.
    WinkeyBridge b = makeBridge();
    open(b);
    b.feed(0x05); b.feed(18); b.feed(22); b.feed(0);  // low=18, range=22
    size_t before = g_spy.out.size();
    b.feed(0x07);                                       // GET_POT
    // WPM is still 20 → (20-18)|0x80 = 0x82
    CHECK_EQ((int)g_spy.out[before], 0x82);
}

static void test_reset_clears_last_pushed_wpm() {
    // After a soft reset, _lastPushedWpm is cleared so the next
    // setWpmFromLocal() with the post-reset default (20) re-emits.
    WinkeyBridge b = makeBridge();
    open(b);
    b.setWpmFromLocal(40);             // _lastPushedWpm = 40
    size_t afterFirst = g_spy.out.size();
    b.feed(0x00); b.feed(0x01);        // soft reset → _wpm=20, _lastPushedWpm=-1
    b.setWpmFromLocal(20);             // would have been a no-op pre-reset
    CHECK_EQ((int)(g_spy.out.size() - afterFirst), 1);
    CHECK_EQ((int)g_spy.out[afterFirst], 0x8F);   // WPM=20 → 0x80|15
}

// ─── Decoded-paddle echo to host (docs/winkey.md § 16.8) ─────────────
//
// When the operator keys the paddle manually, the on-board MorseDecoder
// decodes the dit/dah stream and forwards each character to the WK
// bridge via emitDecodedChar(); the bridge emits one byte per character
// to the host so K3NG-compatible loggers (RUMlogNG, N1MM) mirror the
// keyed text in their log. Mirrors K3NG's `winkey_paddle_echo_buffer`
// decode path at k3ng_keyer.ino:11623-11631.

static void test_emitDecodedChar_silent_before_open() {
    // No host connected → no echo. Mirrors K3NG's
    // `if (winkey_host_open)` guard at k3ng_keyer.ino:11623.
    WinkeyBridge b = makeBridge();
    size_t before = g_spy.out.size();
    b.emitDecodedChar('C');
    CHECK_EQ((int)(g_spy.out.size() - before), 0);
}

static void test_emitDecodedChar_silent_before_primed() {
    // Open but not yet primed → the bridge is still in the host-init
    // window where any byte we send risks being interpreted as an
    // out-of-order reply. Sit on the byte silently — same logic
    // that suppresses text bytes arriving before the first probe.
    WinkeyBridge b = makeBridge();
    openUnprimed(b);
    size_t before = g_spy.out.size();
    b.emitDecodedChar('C');
    CHECK_EQ((int)(g_spy.out.size() - before), 0);
}

static void test_emitDecodedChar_uppercases_lowercase() {
    // Operators may paddle in lowercase habit; the host receives
    // uppercase to match the WK text-byte convention.
    WinkeyBridge b = makeBridge();
    open(b);
    size_t before = g_spy.out.size();
    b.emitDecodedChar('c');
    CHECK_EQ((int)(g_spy.out.size() - before), 1);
    CHECK_EQ((int)g_spy.out[before], (int)'C');
}

static void test_emitDecodedChar_passes_through_uppercase() {
    WinkeyBridge b = makeBridge();
    open(b);
    size_t before = g_spy.out.size();
    b.emitDecodedChar('Q');
    CHECK_EQ((int)(g_spy.out.size() - before), 1);
    CHECK_EQ((int)g_spy.out[before], (int)'Q');
}

static void test_emitDecodedChar_emits_space_for_word_gap() {
    // Word-space is its own byte — K3NG sends a separate ' ' byte
    // at k3ng_keyer.ino:11637 after a word-space timeout.
    WinkeyBridge b = makeBridge();
    open(b);
    size_t before = g_spy.out.size();
    b.emitDecodedChar(' ');
    CHECK_EQ((int)(g_spy.out.size() - before), 1);
    CHECK_EQ((int)g_spy.out[before], (int)' ');
}

static void test_emitDecodedChar_emits_prosign_chars_unchanged() {
    // Prosigns like AR (.-.-.) decode to "<ar>" — the bridge passes
    // `<` and `>` through verbatim and uppercases only the letters.
    // The host sees "<AR>", which RUMlogNG renders as a single
    // prosign in the log.
    WinkeyBridge b = makeBridge();
    open(b);
    size_t before = g_spy.out.size();
    b.emitDecodedChar('<');
    b.emitDecodedChar('a');
    b.emitDecodedChar('r');
    b.emitDecodedChar('>');
    CHECK_EQ((int)(g_spy.out.size() - before), 4);
    CHECK_EQ((int)g_spy.out[before],     (int)'<');
    CHECK_EQ((int)g_spy.out[before + 1], (int)'A');
    CHECK_EQ((int)g_spy.out[before + 2], (int)'R');
    CHECK_EQ((int)g_spy.out[before + 3], (int)'>');
}

static void test_emitDecodedChar_emits_decoded_text_after_primed() {
    // End-to-end shape: after open, the bridge emits nothing until
    // a character is decoded. Then a stream of decoded chars + a
    // space + a stream of decoded chars arrives as the same byte
    // sequence on the wire. This is what the host sees.
    WinkeyBridge b = makeBridge();
    open(b);
    size_t before = g_spy.out.size();
    b.emitDecodedChar('C');
    b.emitDecodedChar('Q');
    b.emitDecodedChar(' ');
    b.emitDecodedChar('D');
    b.emitDecodedChar('E');
    CHECK_EQ((int)(g_spy.out.size() - before), 5);
    CHECK_EQ((int)g_spy.out[before],     (int)'C');
    CHECK_EQ((int)g_spy.out[before + 1], (int)'Q');
    CHECK_EQ((int)g_spy.out[before + 2], (int)' ');
    CHECK_EQ((int)g_spy.out[before + 3], (int)'D');
    CHECK_EQ((int)g_spy.out[before + 4], (int)'E');
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
    RUN(test_text_ignored_before_primed);
    RUN(test_text_accepted_after_get_pot);
    RUN(test_text_accepted_after_req_status);
    RUN(test_rumlog_init_does_not_play_text);
    RUN(test_reset_clears_primed);
    RUN(test_setWpmFromLocal_emits_pin_event_when_open);
    RUN(test_setWpmFromLocal_silent_before_open);
    RUN(test_setWpmFromLocal_no_emit_when_value_unchanged);
    RUN(test_setWpmFromLocal_after_host_setSpeed_no_echo_feedback);
    RUN(test_setWpmFromLocal_after_host_setSpeed_with_different_value_pushes);
    RUN(test_speed_pot_value_formula_table);
    RUN(test_speed_pot_value_clamps_input);
    RUN(test_admin_get_values_emits_14_bytes);
    RUN(test_admin_get_values_second_byte_is_wpm);
    RUN(test_reset_clears_last_pushed_wpm);
    RUN(test_emitDecodedChar_silent_before_open);
    RUN(test_emitDecodedChar_silent_before_primed);
    RUN(test_emitDecodedChar_uppercases_lowercase);
    RUN(test_emitDecodedChar_passes_through_uppercase);
    RUN(test_emitDecodedChar_emits_space_for_word_gap);
    RUN(test_emitDecodedChar_emits_prosign_chars_unchanged);
    RUN(test_emitDecodedChar_emits_decoded_text_after_primed);
    return test_summary();
}
