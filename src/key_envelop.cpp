#include "key_envelop.h"
#include "Log.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <new>      // for std::nothrow

// ---------------------------------------------------------------------------
// Blackman-Harris window (4-term, minimum sidelobe)
// ---------------------------------------------------------------------------
float KeyEnvelop::blackmanHarrisWindow(float x) {
    constexpr float a0 = 0.35875f;
    constexpr float a1 = 0.48829f;
    constexpr float a2 = 0.14128f;
    constexpr float a3 = 0.01168f;
    const float two_pi_x  = 2.0f * M_PI * x;
    const float four_pi_x = 2.0f * two_pi_x;
    const float six_pi_x  = 3.0f * two_pi_x;
    return a0 - a1 * std::cos(two_pi_x) + a2 * std::cos(four_pi_x) - a3 * std::cos(six_pi_x);
}

// ---------------------------------------------------------------------------
// Accumulate BH window to get step response, normalise so ramp ends at 1.0
// ---------------------------------------------------------------------------
void KeyEnvelop::buildStepResponse(float* out, int len) {
    out[0] = blackmanHarrisWindow(0.0f);
    for (int i = 1; i < len; ++i) {
        out[i] = out[i - 1] + blackmanHarrisWindow(static_cast<float>(i) / len);
    }
    const float scale = 1.0f / out[len - 1];
    for (int i = 0; i < len; ++i) out[i] *= scale;
}

void KeyEnvelop::buildRiseRamp(float* out, int rampLen) {
    buildStepResponse(out, rampLen);
}

void KeyEnvelop::buildFallRamp(float* out, int rampLen) {
    float tmp[256];
    buildStepResponse(tmp, rampLen);
    for (int i = 0; i < rampLen; ++i) out[i] = tmp[rampLen - 1 - i];
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
KeyEnvelop::KeyEnvelop(int wpm, float rampTimeSec, int sampleRate)
    : _wpm(wpm)
    , _rampTimeSec(rampTimeSec)
    , _sampleRate(sampleRate)
    , _dirty(false)
{
    regenerate();
}

// ---------------------------------------------------------------------------
void KeyEnvelop::setWPM(int wpm) {
    if (wpm != _wpm) {
        Log::debug("[KE] setWPM: %d -> %d", _wpm, wpm);
        _wpm = wpm;
        _dirty = true;
    }
}

void KeyEnvelop::setRampTime(float seconds) {
    if (seconds != _rampTimeSec) {
        _rampTimeSec = seconds;
        _dirty = true;
    }
}

// ---------------------------------------------------------------------------
// regenerate — pre-allocate ALL new buffers before touching any state
// ---------------------------------------------------------------------------
void KeyEnvelop::regenerate() {
    ++_generationVersion;
    const bool wasDirty = _dirty.load();
    Log::debug("[KE] regenerate ENTER: wpm=%d dirty=%d", _wpm, wasDirty ? 1 : 0);

    // Compute target sizes up-front
    size_t nDit  = static_cast<size_t>(std::round(_sampleRate * 1.2f / _wpm));
    size_t nRamp = static_cast<size_t>(std::round(_rampTimeSec * _sampleRate));
    if (nRamp > nDit / 4) nRamp = nDit / 4;
    if (nRamp < 1) nRamp = 1;
    size_t nDitEnv = nDit * 2;
    size_t nDahEnv = nDit * 4;

    // Allocate all new buffers first (nothrow so OOM returns nullptr, no abort)
    float* ramp  = new (std::nothrow) float[nRamp];
    float* ramp2 = ramp ? new (std::nothrow) float[nRamp] : nullptr;
    float* dEnv  = ramp2 ? new (std::nothrow) float[nDitEnv]() : nullptr;  // () zero-inits
    float* dhEnv = dEnv  ? new (std::nothrow) float[nDahEnv]() : nullptr; // () zero-inits

    if (!ramp || !ramp2 || !dEnv || !dhEnv) {
        Log::warning("[KE] OOM: ramp=%p ramp2=%p dEnv=%p dhEnv=%p  ← keep old",
            (void*)ramp, (void*)ramp2, (void*)dEnv, (void*)dhEnv);
        delete[] ramp; delete[] ramp2; delete[] dEnv; delete[] dhEnv;
        Log::warning("[KE]   old kept: ditEnv=%p(%zu) dahEnv=%p(%zu)",
            (void*)_ditEnv, _ditLen * 2, (void*)_dahEnv, _ditLen * 4);
        _dirty = false;   // use old buffers, don't retry
        return;
    }

    // Build ramp tables
    buildStepResponse(ramp, static_cast<int>(nRamp));
    for (size_t i = 0; i < nRamp; ++i) ramp2[i] = ramp[nRamp - 1 - i];

    // DIT envelope: [ramp | flat(dit-2ramp) | ramp | silence(dit-ramp)]  = 2*dit
    size_t ditFlat = nDit - 2 * nRamp;
    std::memcpy(dEnv, ramp, nRamp * sizeof(float));
    for (size_t i = nRamp; i < nRamp + ditFlat; ++i) dEnv[i] = 1.0f;
    std::memcpy(dEnv + nRamp + ditFlat, ramp2, nRamp * sizeof(float));

    // DAH envelope: [ramp | flat(3*dit-2ramp) | ramp | silence(3*dit-ramp)] = 4*dit
    size_t dahFlat = nDit * 3 - 2 * nRamp;
    std::memcpy(dhEnv, ramp, nRamp * sizeof(float));
    for (size_t i = nRamp; i < nRamp + dahFlat; ++i) dhEnv[i] = 1.0f;
    std::memcpy(dhEnv + nRamp + dahFlat, ramp2, nRamp * sizeof(float));

    delete[] ramp;
    delete[] ramp2;

    // Atomic swap
    float* oldD = _ditEnv;
    float* oldH = _dahEnv;
    _ditEnv = dEnv;
    _dahEnv = dhEnv;
    _ditLen = nDit;
    _dahLen = nDit * 3;
    _rampLen = nRamp;
    _dirty = false;

    Log::debug("[KE] regenerate DONE: ditEnv=%p(%zu) dahEnv=%p(%zu) rampLen=%zu",
        (void*)_ditEnv, _ditLen * 2, (void*)_dahEnv, _ditLen * 4, _rampLen);

    delete[] oldD;
    delete[] oldH;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
size_t KeyEnvelop::envelopeSize(Element el) const {
    if (_dirty) const_cast<KeyEnvelop*>(this)->regenerate();
    return (el == Element::DIT) ? (_ditLen * 2) : (_ditLen * 4);
}

size_t KeyEnvelop::rampLengthSamples() const {
    if (_dirty) const_cast<KeyEnvelop*>(this)->regenerate();
    return _rampLen;
}

const float* KeyEnvelop::envelope(Element el) const {
    if (_dirty) const_cast<KeyEnvelop*>(this)->regenerate();
    return (el == Element::DIT) ? _ditEnv : _dahEnv;
}

int KeyEnvelop::ditLengthSamples() const {
    if (_dirty) const_cast<KeyEnvelop*>(this)->regenerate();
    return static_cast<int>(_ditLen);
}