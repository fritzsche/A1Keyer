#pragma once
/**
 * KeyEnvelop — Blackman-Harris envelope shaping for click-free CW keying.
 *
 * Generates shaped DIT (1 unit) and DAH (3 units) envelopes with
 * raised-cosine ramps at both ends (configurable ramp time).
 *
 * The envelope is a float [0..1] multiplier applied per-sample to the
 * tone waveform.  It ensures zero-crossing at key-up / key-down to
 * eliminate switch-closure clicks.
 *
 * WPM can be changed at any time via setWPM() — buffers are re-generated
 * on the next call that needs them.
 */

#include <cstddef>
#include <cstdint>
#include <atomic>

class KeyEnvelop {
public:
    /** Element duration in morse units: DIT = 1, DAH = 3. */
    enum class Element { DIT, DAH };

    /**
     * @param wpm             Initial words-per-minute (Paris standard: 1 WPM = 1 dit/sec).
     * @param rampTimeSec     Raised-cosine ramp duration in seconds (default 5 ms).
     * @param sampleRate      Audio sample rate in Hz.
     */
    explicit KeyEnvelop(int wpm = 20, float rampTimeSec = 0.005f, int sampleRate = 48000);

    /** Regenerate envelopes when WPM changes. */
    void setWPM(int wpm);

    /** Regenerate envelopes when ramp time changes. */
    void setRampTime(float seconds);

    /** @return Envelope sample count for the given element.
     *  Cmorse-compatible: DIT = 2*ditLen (tone + intra-character silence),
     *  DAH = 4*ditLen (3*tone + intra-character silence).
     *  Ramps are fully contained inside; no extra silence correction needed. */
    size_t envelopeSize(Element el) const;

    /** @return Ramp length in samples. Capped to ditLen/4 to keep flat-top ≥ ditLen/2. */
    size_t rampLengthSamples() const;

    /** @return Pointer to envelope samples for the given element. */
    const float* envelope(Element el) const;

    /** @return Duration of one dit in seconds. */
    float ditLengthSec() const { return 1.2f / static_cast<float>(_wpm); }

    /** @return Duration of one dit in samples (rounded up). */
    int ditLengthSamples() const;

    /** @return Current WPM. */
    int wpm() const { return _wpm; }

    /** @return Current ramp time in seconds. */
    float rampTime() const { return _rampTimeSec; }

    /** @return Sample rate. */
    int sampleRate() const { return _sampleRate; }

    /** @return Generation version — incremented each time envelopes are rebuilt. */
    uint32_t generationVersion() const { return _generationVersion; }

    // --- Public static helpers (usable by any keyer) -----------------------
    /** 4-term Blackman-Harris window. x in [0,1]. Used for ramp shaping. */
    static float blackmanHarrisWindow(float x);

    /** Build ramp from BH window step response, normalised to [0,1]. */
    static void buildStepResponse(float* out, int len);

    /** Rise ramp array (BH step response, 0→1). Caller owns the buffer. */
    static void buildRiseRamp(float* out, int rampLen);

    /** Fall ramp array (reversed rise ramp, 1→0). Caller owns the buffer. */
    static void buildFallRamp(float* out, int rampLen);

private:
    void regenerate();  // rebuild all envelopes from current _wpm / _rampTimeSec

    int    _wpm;
    float  _rampTimeSec;
    int    _sampleRate;

    float* _ditEnv  = nullptr;
    float* _dahEnv  = nullptr;
    size_t _ditLen  = 0;
    size_t _dahLen  = 0;
    size_t _rampLen = 0;

    std::atomic<bool>  _dirty = true;       // set when WPM/rampTime changes
    uint32_t _generationVersion = 0;
};