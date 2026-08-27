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
// regenerate — free old buffers FIRST, then allocate new ones.
//
// To avoid OOM at low WPM (where envelopes are large), we release the
// old buffers before allocating new ones. The sequence is:
//
//   1. Set _dirty = false  ← audio task stops trying to regenerate
//   2. Set _regenerating = true  ← guard against concurrent calls
//   3. Free old buffers (valid data survives in _ditEnv until step 4)
//   4. Allocate new buffers
//   5. If OOM: set _regenerating = false, return (nothing changed)
//   6. Fill new envelopes
//   7. Swap _ditEnv / _dahEnv to new buffers
//   8. Free old buffers
//   9. Set _regenerating = false
//
// Between steps 1 and 7, the audio task sees _dirty=false and reads
// the OLD _ditEnv (valid until freed in step 8). After step 7, it reads
// the NEW buffers. No reader ever sees nullptr.
// ---------------------------------------------------------------------------
void KeyEnvelop::regenerate() {
    ++_generationVersion;
    const bool wasDirty = _dirty.load();

    // Guard against concurrent calls from two cores.
    bool expected = false;
    if (!_regenerating.compare_exchange_strong(expected, true)) {
        Log::debug("[KE] regenerate SKIP: already regenerating");
        return;
    }

    // Clear dirty flag NOW — the audio task stops trying to regenerate
    // and will read the old (still-valid) buffers until the swap below.
    _dirty = false;

    Log::debug("[KE] regenerate ENTER: wpm=%d", _wpm);

    // Compute target sizes
    size_t nDit  = static_cast<size_t>(std::round(_sampleRate * 1.2f / _wpm));
    size_t nRamp = static_cast<size_t>(std::round(_rampTimeSec * _sampleRate));
    if (nRamp > nDit / 4) nRamp = nDit / 4;
    if (nRamp < 1) nRamp = 1;
    size_t nDitEnv = nDit * 2;
    size_t nDahEnv = nDit * 4;

    // Save old state before freeing
    float* oldD = _ditEnv;
    float* oldH = _dahEnv;
    size_t oldDitLen = _ditLen;
    size_t oldDahLen = _dahLen;
    size_t oldRampLen = _rampLen;

    // Free OLD buffers first to reduce peak memory (prevents OOM at
    // low WPM where old+new would exceed heap).
    _ditEnv = nullptr;
    _dahEnv = nullptr;
    _ditLen = 0;
    _dahLen = 0;
    _rampLen = 0;

    // Allocate the BIGGEST buffers FIRST so OOM is detected early.
    // Original code allocated (ramp→ramp2→dEnv→dhEnv) — smallest first —
    // which wasted heap on small allocations when the big one would fail.
    float* dhEnv = new (std::nothrow) float[nDahEnv]();  // () zero-inits
    float* dEnv  = dhEnv ? new (std::nothrow) float[nDitEnv]() : nullptr;
    float* ramp  = dEnv  ? new (std::nothrow) float[nRamp] : nullptr;
    float* ramp2 = ramp  ? new (std::nothrow) float[nRamp] : nullptr;

    if (!dhEnv || !dEnv || !ramp || !ramp2) {
        Log::warning("[KE] OOM: dhEnv=%p dEnv=%p ramp=%p ramp2=%p  ← keep old",
            (void*)dhEnv, (void*)dEnv, (void*)ramp, (void*)ramp2);
        delete[] dhEnv; delete[] dEnv; delete[] ramp; delete[] ramp2;
        _ditEnv = oldD;
        _dahEnv = oldH;
        _ditLen = oldDitLen;
        _dahLen = oldDahLen;
        _rampLen = oldRampLen;
        Log::warning("[KE]   restored old buffers dit=%zu dah=%zu",
            oldDitLen, oldDahLen);
        _regenerating.store(false);
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

    // Swap new buffers in
    _ditEnv = dEnv;
    _dahEnv = dhEnv;
    _ditLen = nDit;
    _dahLen = nDit * 3;
    _rampLen = nRamp;

    Log::debug("[KE] regenerate DONE: ditEnv=%p(%zu) dahEnv=%p(%zu) rampLen=%zu",
        (void*)_ditEnv, _ditLen * 2, (void*)_dahEnv, _ditLen * 4, _rampLen);

    // Free old buffers — safe now: _dirty=false and _ditEnv points to
    // the new buffers, so the audio task reads the new data.
    delete[] oldD;
    delete[] oldH;

    _regenerating.store(false);
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
