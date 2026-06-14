/**
 * straight_keyer.cpp — Lock-free straight key (single-contact) keyer.
 *
 * Operator timing determines element type:
 *   Short press (~1 dit) → DIT
 *   Long press (~3 dits) → DAH
 *   Threshold: dit if < _framesPerDit * 3/2, else dah
 *
 * Envelope: 4-term Blackman-Harris raised-cosine via KeyEnvelop helpers.
 *   Rise:   BH step response 0→1 over rampSamples
 *   Hold:   env = 1.0
 *   Fall:   BH step response 1→0 over rampSamples (reversed rise ramp)
 *
 * Audio path: sine × envelope. Phase is continuous across state transitions.
 * Decoder path: ring buffer (_rb[1024]) holding timestamped key events.
 *
 * Timing-based decoding: key up/down transitions are tracked in loop() (Core 0)
 * via decodeFromLoop(), with sample-frame timestamps to compute element duration
 * and silence gap. This replaces the sample-counting approach in fillSamples.
 *
 * Reference: extern/main.c straight_key_callback + straight_decoder_thread
 */

#include "straight_keyer.h"
#include "Log.h"
#include "morse_encoder.h"
#include "display_model.h"
#ifndef UNIT_TEST
#include "morse_key.h"    // for PIN_KEY_DIT (gpio pin assignment)
#include "audio_engine.h" // for AUDIO_SAMPLE_RATE
#else
static constexpr uint32_t AUDIO_SAMPLE_RATE = 48000; // test fallback
#endif
#ifdef UNIT_TEST
#include "arduino_mock.h"
#else
#include <esp_timer.h>
#endif
#include <cstring>

// ---------------------------------------------------------------------------
// Public methods
// ---------------------------------------------------------------------------

void StraightKeyer::begin()
{
    _state = State::IDLE;
    _rampSamples = 0;
    _rampPos = 0;
    _phase = 0.0f;
    _phaseInc = 0.0f;
    _totalSamplesRendered = 0;
    _lastElementEndFrame = 0;
    rbHead.store(0, std::memory_order_relaxed);
    rbTail.store(0, std::memory_order_relaxed);

    KeyEnvelop::buildRiseRamp(_riseRamp, RAMP_LEN);
    KeyEnvelop::buildFallRamp(_fallRamp, RAMP_LEN);
}

void StraightKeyer::setWPM(int wpm)
{
    _wpm = wpm;
    _framesPerDit = static_cast<int>(AUDIO_SAMPLE_RATE * 1.2f / wpm);
}

// ---------------------------------------------------------------------------
// Audio path
// ---------------------------------------------------------------------------

size_t StraightKeyer::fillSamples(int16_t *mono, size_t frames,
                                  float toneHz, int16_t amp, int sampleRate)
{
    if (_rampSamples == 0)
    {
        _rampSamples = static_cast<int>(0.005f * sampleRate);
        if (_rampSamples < 1)
            _rampSamples = 1;
    }

    const float phaseInc = toneHz / static_cast<float>(sampleRate);

#ifndef UNIT_TEST
    // GPIO-driven key edge detection is only meaningful on the real device.
    // The unit tests drive the state machine via s_keyState.memory[DIT_IDX]
    // directly, so this block is compiled out in the host test build.
    if (_state == State::IDLE)
    {
        // Key Pressed Down
        if (gpio_get_level(PIN_KEY_DIT) == 0)
        {
            rbWriteEvent(KeyEvent::KEY_DOWN, _totalSamplesRendered);
            _state = State::RISE;
            _rampPos = 0;
            _phase = 0.0f;
            _phaseInc = phaseInc;
        }
    }
    if (_state == State::HOLD)
    {
        // Key Released
        if (gpio_get_level(PIN_KEY_DIT) != 0)
        {
            rbWriteEvent(KeyEvent::KEY_UP, _totalSamplesRendered);
            std::atomic_store(&s_keyState.memory[DIT_IDX], MEMORY_UNSET);
            _state = State::FALL;
            _rampPos = 0;
        }
    }
#endif

    for (size_t i = 0; i < frames; ++i)
    {
        // ── IDLE: wait for key press ───────────────────────────────────────
        if (_state == State::IDLE)
        {
            // Transition from
            if (std::atomic_load(&s_keyState.memory[DIT_IDX]) == MEMORY_SET)
            {
                rbWriteEvent(KeyEvent::KEY_DOWN, _totalSamplesRendered);
                _state = State::RISE;
                _rampPos = 0;
                _phase = 0.0f;
                _phaseInc = phaseInc;
            }
            else
            {
                mono[i] = 0;
                ++_totalSamplesRendered;
                continue;
            }
        }

        // ── RISE: BH ramp 0→1 ─────────────────────────────────────────────
        if (_state == State::RISE)
        {
            float env = _riseRamp[_rampPos];
            float s = fastSinNormalized(_phase) * env * amp;
            mono[i] = static_cast<int16_t>(s);
            _phase += _phaseInc;
            if (_phase >= 1.0f)
                _phase -= 1.0f;
            ++_rampPos;
            if (_rampPos >= _rampSamples)
            {
                _state = State::HOLD;
                _rampPos = 0;
            }
        }
        // ── HOLD: full amplitude, waiting for key release ────────────────────
        else if (_state == State::HOLD)
        {
            if (std::atomic_load(&s_keyState.state[DIT_IDX]) == MEMORY_UNSET)
            {
                // Key is released
                rbWriteEvent(KeyEvent::KEY_UP, _totalSamplesRendered);
                std::atomic_store(&s_keyState.memory[DIT_IDX], MEMORY_UNSET);
                _state = State::FALL;
                _rampPos = 0;
                mono[i] = static_cast<int16_t>(fastSinNormalized(_phase) * amp);
                _phase += _phaseInc;
                if (_phase >= 1.0f)
                    _phase -= 1.0f;
                ++_rampPos;
            }
            else
            {
                float s = fastSinNormalized(_phase) * amp;
                mono[i] = static_cast<int16_t>(s);
                _phase += _phaseInc;
                if (_phase >= 1.0f)
                    _phase -= 1.0f;
            }
        }
        // ── FALL: BH ramp 1→0 ─────────────────────────────────────────────
        else if (_state == State::FALL)
        {
            float env = _fallRamp[_rampPos];
            float s = fastSinNormalized(_phase) * env * amp;
            mono[i] = static_cast<int16_t>(s);
            _phase += _phaseInc;
            if (_phase >= 1.0f)
                _phase -= 1.0f;
            ++_rampPos;
            if (_rampPos >= _rampSamples)
            {
                _lastElementEndFrame = _totalSamplesRendered;
                _state = State::IDLE;
                _rampPos = 0;

                // Discard stale ring buffer events
                rbHead.store(0, std::memory_order_relaxed);
                rbTail.store(0, std::memory_order_relaxed);
            }
        }
        ++_totalSamplesRendered;
    }

    return frames;
}

// ---------------------------------------------------------------------------
// Decoder ring buffer — single producer (audio, Core 1), single consumer (loop, Core 0)
// ---------------------------------------------------------------------------

bool StraightKeyer::decoderRead(KeyEvent *out)
{
    size_t head = rbHead.load(std::memory_order_acquire);
    size_t tail = rbTail.load(std::memory_order_relaxed);
    if (head == tail)
        return false;
    *out = _rb[tail];
    rbTail.store((tail + 1) % RB_SIZE, std::memory_order_relaxed);
    return true;
}

size_t StraightKeyer::decoderAvailable() const
{
    size_t head = rbHead.load(std::memory_order_acquire);
    size_t tail = rbTail.load(std::memory_order_relaxed);
    if (head >= tail)
        return head - tail;
    return RB_SIZE - (tail - head);
}

bool StraightKeyer::decoderRead(char *out)
{
    (void)out;
    return false;
}

void StraightKeyer::rbWriteEvent(char event, uint64_t frame)
{
    // Write data BEFORE updating head (release ensures data is visible before head is visible)
    size_t head = rbHead.load(std::memory_order_relaxed);
    _rb[head] = {event, frame};
    std::atomic_thread_fence(std::memory_order_release);
    rbHead.store((head + 1) % RB_SIZE, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Decoder — loop() on Core 0
// ---------------------------------------------------------------------------

/**
 * Decode straight key timing from loop() (Core 0).
 *
 * Reads DOWN/UP events from the ring buffer, classifies elements as dit/dah,
 * detects gap type (intra-element / intra-character / word-space), and outputs
 * decoded characters directly to MorseModel.
 *
 * Reference: extern/main.c straight_decoder_thread (lines 186-288)
 */
void StraightKeyer::decodeFromLoop(int sampleRate)
{
    static int _loggedWpm = 0;
    if (_loggedWpm != _wpm)
    {
        _loggedWpm = _wpm;
        Log::debug("[SK-L] decodeFromLoop started: _wpm=%d _framesPerDit=%d",
                   _wpm, _framesPerDit);
    }
    KeyEvent evt;
    while (decoderRead(&evt))
    {
        if (evt.event == KeyEvent::KEY_DOWN)
        {
            // DOWN — new element starting: compute gap since last UP
            int64_t gap = 0;
            if (_lastUpFrame > 0)
            {
                gap = static_cast<int64_t>(evt.frame) - static_cast<int64_t>(_lastUpFrame);
            }

            if (_lastUpFrame > 0 && gap > 0)
            {
                int ditFrames = static_cast<int>(sampleRate * 1.2f / _wpm);
                int intraCharThreshold = INTRA_CHAR_DITS * ditFrames;
                int wordSpaceThreshold = WORD_SPACE_DITS * ditFrames;
                if (gap > wordSpaceThreshold)
                {
                    flushDitDahBuffer();
                    MorseModel::instance().appendDecodedChar(' ');
                    Log::debug("[SK-L] WORD SPACE");
                }
                else if (gap > intraCharThreshold)
                {
                    flushDitDahBuffer();
                }
            }

            _lastDownFrame = evt.frame;
            _lastUpFrame = 0; // clear so next UP knows this was a DOWN event
        }
        else if (evt.event == KeyEvent::KEY_UP)
        {
            // UP — element finished: classify as dit or dah
            if (_lastDownFrame > 0)
            {
                int64_t downLen = static_cast<int64_t>(evt.frame) - static_cast<int64_t>(_lastDownFrame);
                bool isDit = (downLen < (_framesPerDit * 3 / 2));
                char sym = isDit ? DECODER_DIT_SYMBOL : DECODER_DAH_SYMBOL;
                addDitDah(sym);
                _lastUpFrame = evt.frame;
                _charTimeoutFrame = evt.frame + 5 * _framesPerDit;
            }
        }
    }

    // Character timeout: if no new event within 5 dit-lengths after an UP, flush buffer
    if (_charTimeoutFrame > 0 && _totalSamplesRendered >= _charTimeoutFrame && !(_lastDownFrame > 0 && _lastUpFrame == 0))
    {
        flushDitDahBuffer();
        _charTimeoutFrame = 0;
    }
}

void StraightKeyer::addDitDah(char sym)
{
    if (_ditDahLen < sizeof(_ditDahBuf) - 1)
    {
        _ditDahBuf[_ditDahLen++] = sym;
    }
}

void StraightKeyer::flushDitDahBuffer()
{
    if (_ditDahLen == 0)
        return;
    _ditDahBuf[_ditDahLen] = '\0';
    const char *decoded = MorseEncoder::charFromMorse(_ditDahBuf);
    if (decoded)
    {
        for (const char *p = decoded; *p; ++p)
        {
            MorseModel::instance().appendDecodedChar(*p);
        }
    }
    else
    {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "<%s>", _ditDahBuf);
        Log::write("%s", errBuf);
        for (const char *p = errBuf; *p; ++p)
        {
            MorseModel::instance().appendDecodedChar(*p);
        }
    }
    _ditDahLen = 0;
    memset(_ditDahBuf, 0, sizeof(_ditDahBuf));
    _charTimeoutFrame = 0;
}

void StraightKeyer::reset()
{
    _state = State::IDLE;
    _rampPos = 0;
    _totalSamplesRendered = 0;
    _lastElementEndFrame = 0;
    _lastDownFrame = 0;
    _lastUpFrame = 0;
    _charTimeoutFrame = 0;
    // NOTE: do NOT reset _framesPerDit — it is calibrated to the current WPM
    // and must survive across mode switches. Only clear the decode buffer.
    _ditDahLen = 0;
    memset(_ditDahBuf, 0, sizeof(_ditDahBuf));
    rbHead.store(0, std::memory_order_relaxed);
    rbTail.store(0, std::memory_order_relaxed);
}