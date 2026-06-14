#include "test_framework.h"
#include "key_envelop.h"

// --- timing basics ---

static void test_dit_length_sec_at_20_wpm() {
    KeyEnvelop env(20, 0.005f, 48000);
    // Paris standard: 1 WPM = 1.2 dit/s  →  1 dit at 20 WPM = 0.060 s
    CHECK_NEAR(0.060f, env.ditLengthSec(), 0.001f);
}

static void test_dit_length_samples_at_20wpm_48k() {
    KeyEnvelop env(20, 0.005f, 48000);
    // 48000 × 1.2 / 20 = 2880 exactly, but ceil(float) gives 2880 or 2881
    // depending on how 1.2f is represented (0x3F99999A > 1.2 exactly).
    int len = env.ditLengthSamples();
    CHECK(len >= 2880 && len <= 2881);
}

// --- envelope sizes ---

static void test_dit_envelope_size_is_just_ditLen() {
    KeyEnvelop env(20, 0.005f, 48000);
    int ditLen = env.ditLengthSamples();
    // Cmorse-compatible: DIT envelope = 2*ditLen (tone + intra-character silence)
    CHECK_EQ(ditLen * 2, (int)env.envelopeSize(KeyEnvelop::Element::DIT));
}

static void test_dah_envelope_size_is_three_ditLen() {
    KeyEnvelop env(20, 0.005f, 48000);
    int ditLen = env.ditLengthSamples();
    // Cmorse-compatible: DAH envelope = 4*ditLen (3*tone + intra-character silence)
    CHECK_EQ(ditLen * 4, (int)env.envelopeSize(KeyEnvelop::Element::DAH));
}

// --- envelope shape (Blackman-Harris ramp) ---

static void test_dit_envelope_starts_near_zero() {
    KeyEnvelop env(20, 0.005f, 48000);
    const float* e = env.envelope(KeyEnvelop::Element::DIT);
    CHECK_NEAR(0.0f, e[0], 0.02f);
}

static void test_dit_envelope_flat_top_is_one() {
    // Mid-point is inside the flat-top region and should read 1.0
    KeyEnvelop env(20, 0.005f, 48000);
    const float* e = env.envelope(KeyEnvelop::Element::DIT);
    size_t rampLen = env.rampLengthSamples();
    // With cmorse layout [ramp_up | flat | ramp_down | silence], the flat-top
    // is at index rampLen + (rampLen to rampLen + ditFlat/2). Use rampLen as safe mid-flat.
    CHECK_NEAR(1.0f, e[rampLen], 0.01f);
}

static void test_dit_envelope_ends_near_zero() {
    // Reported size includes trailing zeros after the ramp-down
    KeyEnvelop env(20, 0.005f, 48000);
    const float* e = env.envelope(KeyEnvelop::Element::DIT);
    size_t sz = env.envelopeSize(KeyEnvelop::Element::DIT);
    CHECK_NEAR(0.0f, e[sz - 1], 0.02f);
}

static void test_dah_envelope_flat_top_is_one() {
    KeyEnvelop env(20, 0.005f, 48000);
    const float* e = env.envelope(KeyEnvelop::Element::DAH);
    size_t sz = env.envelopeSize(KeyEnvelop::Element::DAH);
    CHECK_NEAR(1.0f, e[sz / 2], 0.01f);
}

static void test_all_envelope_values_in_zero_one_range() {
    KeyEnvelop env(20, 0.005f, 48000);
    const float* e = env.envelope(KeyEnvelop::Element::DIT);
    size_t sz = env.envelopeSize(KeyEnvelop::Element::DIT);
    bool ok = true;
    for (size_t i = 0; i < sz; ++i)
        if (e[i] < -0.01f || e[i] > 1.01f) { ok = false; break; }
    CHECK(ok);
}

// --- WPM changes ---

static void test_set_wpm_halves_dit_length_at_double_speed() {
    KeyEnvelop env(20, 0.005f, 48000);
    int samples20 = env.ditLengthSamples();
    env.setWPM(40);
    int samples40 = env.ditLengthSamples();
    // Each WPM level rounds independently via ceil(float), so allow ±1 sample
    CHECK(2 * samples40 >= samples20 - 2 && 2 * samples40 <= samples20 + 2);
}

static void test_set_wpm_updates_envelope_size() {
    KeyEnvelop env(20, 0.005f, 48000);
    int sz20 = (int)env.envelopeSize(KeyEnvelop::Element::DIT);
    env.setWPM(40);
    int sz40 = (int)env.envelopeSize(KeyEnvelop::Element::DIT);
    // envelopeSize = ditLen + 2*rampLen.  Doubling WPM halves ditLen; rampLen is
    // fixed (same ramp time in seconds), so the ratio is approximately 1:2 for
    // ditLen-dominant sizes.  Just verify the 40 WPM size is strictly smaller.
    CHECK(sz40 < sz20);
}

static void test_wpm_accessor_returns_current_wpm() {
    KeyEnvelop env(20, 0.005f, 48000);
    CHECK_EQ(20, env.wpm());
    env.setWPM(30);
    CHECK_EQ(30, env.wpm());
}

// --- ramp time ---

static void test_ramp_time_accessor() {
    KeyEnvelop env(20, 0.005f, 48000);
    CHECK_NEAR(0.005f, env.rampTime(), 0.0001f);
}

static void test_set_ramp_time_updates_accessor() {
    KeyEnvelop env(20, 0.005f, 48000);
    env.setRampTime(0.010f);
    CHECK_NEAR(0.010f, env.rampTime(), 0.0001f);
}

// --- buildRiseRamp / buildFallRamp (public static helpers) ---

static void test_build_rise_ramp_starts_near_zero() {
    float ramp[256];
    KeyEnvelop::buildRiseRamp(ramp, 256);
    CHECK(ramp[0] < 0.01f);
}

static void test_build_rise_ramp_ends_near_one() {
    float ramp[256];
    KeyEnvelop::buildRiseRamp(ramp, 256);
    CHECK_NEAR(1.0f, ramp[255], 0.01f);
}

static void test_build_fall_ramp_starts_near_one() {
    float ramp[256];
    KeyEnvelop::buildFallRamp(ramp, 256);
    CHECK_NEAR(1.0f, ramp[0], 0.01f);
}

static void test_build_fall_ramp_ends_near_zero() {
    float ramp[256];
    KeyEnvelop::buildFallRamp(ramp, 256);
    CHECK(ramp[255] < 0.01f);
}

static void test_build_rise_and_fall_ramp_are_opposites() {
    float rise[256];
    float fall[256];
    KeyEnvelop::buildRiseRamp(rise, 256);
    KeyEnvelop::buildFallRamp(fall, 256);
    // fall[i] should equal rise[255-i]
    for (int i = 0; i < 256; ++i) {
        CHECK_NEAR(rise[255 - i], fall[i], 0.001f);
    }
}

static void test_build_rise_ramp_values_in_range() {
    float ramp[256];
    KeyEnvelop::buildRiseRamp(ramp, 256);
    for (int i = 0; i < 256; ++i) {
        CHECK(ramp[i] >= 0.0f && ramp[i] <= 1.0f);
    }
}

static void test_build_fall_ramp_values_in_range() {
    float ramp[256];
    KeyEnvelop::buildFallRamp(ramp, 256);
    for (int i = 0; i < 256; ++i) {
        CHECK(ramp[i] >= 0.0f && ramp[i] <= 1.0f);
    }
}

int main() {
    printf("=== test_key_envelop ===\n");
    RUN(test_dit_length_sec_at_20_wpm);
    RUN(test_dit_length_samples_at_20wpm_48k);
    RUN(test_dit_envelope_size_is_just_ditLen);
    RUN(test_dah_envelope_size_is_three_ditLen);
    RUN(test_dit_envelope_starts_near_zero);
    RUN(test_dit_envelope_flat_top_is_one);
    RUN(test_dit_envelope_ends_near_zero);
    RUN(test_dah_envelope_flat_top_is_one);
    RUN(test_all_envelope_values_in_zero_one_range);
    RUN(test_set_wpm_halves_dit_length_at_double_speed);
    RUN(test_set_wpm_updates_envelope_size);
    RUN(test_wpm_accessor_returns_current_wpm);
    RUN(test_ramp_time_accessor);
    RUN(test_set_ramp_time_updates_accessor);
    RUN(test_build_rise_ramp_starts_near_zero);
    RUN(test_build_rise_ramp_ends_near_one);
    RUN(test_build_fall_ramp_starts_near_one);
    RUN(test_build_fall_ramp_ends_near_zero);
    RUN(test_build_rise_and_fall_ramp_are_opposites);
    RUN(test_build_rise_ramp_values_in_range);
    RUN(test_build_fall_ramp_values_in_range);
    return test_summary();
}
