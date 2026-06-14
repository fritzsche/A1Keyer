#pragma once
// Self-contained test framework — zero external dependencies, C++17.
//
// Usage:
//   static void test_something() {
//       CHECK(1 + 1 == 2);
//       CHECK_EQ(42, compute());
//       CHECK_NEAR(3.14f, pi(), 0.01f);
//   }
//   int main() {
//       RUN(test_something);
//       return test_summary();
//   }

#include <cstdio>
#include <cmath>
#include <cstring>

// Per-binary counters (static = one copy per TU; only test .cpp includes this)
static int  _tf_pass    = 0;
static int  _tf_fail    = 0;
static char _tf_buf[4096];       // failure messages for current test
static int  _tf_buf_pos = 0;

inline void _tf_record(const char* file, int line, const char* msg) {
    // Strip directory — show only the filename
    const char* f = file;
    for (const char* p = file; *p; ++p)
        if (*p == '/' || *p == '\\') f = p + 1;
    _tf_buf_pos += snprintf(_tf_buf + _tf_buf_pos,
                            (int)sizeof(_tf_buf) - _tf_buf_pos,
                            "          %s:%d  %s\n", f, line, msg);
    ++_tf_fail;
}

template<typename A, typename B>
inline void _tf_eq(const char* file, int line,
                   const char* ea, const char* eb, A a, B b) {
    if (a == b) { ++_tf_pass; return; }
    char m[256];
    snprintf(m, sizeof(m), "%s == %s", ea, eb);
    _tf_record(file, line, m);
}

inline void _tf_near(const char* file, int line,
                     const char* ea, const char* eb,
                     float a, float b, float eps) {
    if (std::fabs(a - b) <= eps) { ++_tf_pass; return; }
    char m[256];
    snprintf(m, sizeof(m), "|%s - %s| = %.6g  (tolerance %.6g)",
             ea, eb, (double)std::fabs(a - b), (double)eps);
    _tf_record(file, line, m);
}

inline void _tf_str_eq(const char* file, int line,
                       const char* ea, const char* eb,
                       const char* a, const char* b) {
    if (a && b && std::strcmp(a, b) == 0) { ++_tf_pass; return; }
    char m[256];
    snprintf(m, sizeof(m), "%s == %s  got \"%s\" vs \"%s\"",
             ea, eb, a ? a : "(null)", b ? b : "(null)");
    _tf_record(file, line, m);
}

// ── public macros ──────────────────────────────────────────────────────────

#define CHECK(cond) \
    ((cond) ? (void)++_tf_pass \
            : _tf_record(__FILE__, __LINE__, #cond))

#define CHECK_EQ(a, b) \
    _tf_eq(__FILE__, __LINE__, #a, #b, (a), (b))

#define CHECK_NEAR(a, b, eps) \
    _tf_near(__FILE__, __LINE__, #a, #b, (float)(a), (float)(b), (float)(eps))

#define CHECK_STR_EQ(a, b) \
    _tf_str_eq(__FILE__, __LINE__, #a, #b, (a), (b))

#define CHECK_NOT_NULL(p) \
    CHECK((p) != nullptr)

// Run one test function; print "pass" or "FAIL" followed by any failure detail.
#define RUN(fn) do { \
    _tf_buf_pos = 0; _tf_buf[0] = '\0'; \
    int _f0 = _tf_fail; \
    (fn)(); \
    if (_tf_fail == _f0) { \
        printf("  pass  " #fn "\n"); \
    } else { \
        printf("  FAIL  " #fn "  (%d)\n", _tf_fail - _f0); \
        fputs(_tf_buf, stdout); \
    } \
} while(0)

// Call at the end of main(); prints totals and returns exit code.
inline int test_summary() {
    int total = _tf_pass + _tf_fail;
    printf("\n%d/%d assertions passed", _tf_pass, total);
    if (_tf_fail) printf("  --  %d FAILED", _tf_fail);
    printf("\n");
    return _tf_fail ? 1 : 0;
}
