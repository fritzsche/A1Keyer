#include "test_framework.h"
#include "log_ring.h"

// Helper: push a null-terminated C string into the ring one byte at a time.
static void push(LogRing& r, const char* s) {
    while (*s) r.write((uint8_t)*s++);
}

// ─── empty ring ─────────────────────────────────────────────────────────────
static void test_empty_returns_empty_array() {
    LogRing r;
    CHECK_EQ(std::string("[]"), r.snapshotLines(10));
}

// ─── single line, no terminator yet ─────────────────────────────────────────
static void test_partial_line_not_returned() {
    LogRing r;
    push(r, "abc");
    // No '\n' yet — line is not complete, must not appear.
    CHECK_EQ(std::string("[]"), r.snapshotLines(10));
}

// ─── single complete line ───────────────────────────────────────────────────
static void test_single_complete_line() {
    LogRing r;
    push(r, "hello\n");
    CHECK_EQ(std::string("[\"hello\"]"), r.snapshotLines(10));
}

// ─── multiple lines, default take-last-n ────────────────────────────────────
static void test_returns_last_n_lines() {
    LogRing r;
    push(r, "one\ntwo\nthree\nfour\n");
    CHECK_EQ(std::string("[\"one\",\"two\",\"three\",\"four\"]"),
             r.snapshotLines(10));
    CHECK_EQ(std::string("[\"two\",\"three\",\"four\"]"), r.snapshotLines(3));
    CHECK_EQ(std::string("[\"four\"]"), r.snapshotLines(1));
}

// ─── trailing partial line is dropped ───────────────────────────────────────
static void test_trailing_partial_line_dropped() {
    LogRing r;
    push(r, "one\ntwo\nincomp");  // last line not terminated
    CHECK_EQ(std::string("[\"one\",\"two\"]"), r.snapshotLines(10));
}

// ─── CRLF normalisation ─────────────────────────────────────────────────────
static void test_crlf_stripped_from_end() {
    LogRing r;
    push(r, "a\r\nb\r\n");
    CHECK_EQ(std::string("[\"a\",\"b\"]"), r.snapshotLines(10));
}

// ─── wrap-around (ring fills, oldest evicted) ──────────────────────────────
static void test_wrap_around_keeps_recent() {
    LogRing r;
    // Fill with many complete lines so the ring overflows.
    std::string filler;
    for (int i = 0; i < 2000; ++i) filler += "old\n";   // 8000 bytes
    push(r, filler.c_str());

    // Push three new lines that must survive.
    push(r, "first\nsecond\nthird\n");

    auto out = r.snapshotLines(100);
    // The three new lines should be returned.
    CHECK(out.find("first")  != std::string::npos);
    CHECK(out.find("second") != std::string::npos);
    CHECK(out.find("third")  != std::string::npos);
}

// ─── JSON escaping for special characters in lines ─────────────────────────
static void test_json_escapes_quotes() {
    LogRing r;
    push(r, "say \"hi\"\n");
    auto out = r.snapshotLines(1);
    CHECK(out == std::string("[\"say \\\"hi\\\"\"]"));
}

int main() {
    RUN(test_empty_returns_empty_array);
    RUN(test_partial_line_not_returned);
    RUN(test_single_complete_line);
    RUN(test_returns_last_n_lines);
    RUN(test_trailing_partial_line_dropped);
    RUN(test_crlf_stripped_from_end);
    RUN(test_wrap_around_keeps_recent);
    RUN(test_json_escapes_quotes);
    return test_summary();
}