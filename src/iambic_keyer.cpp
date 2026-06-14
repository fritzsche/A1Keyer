/**
 * iambic_keyer.cpp — Lock-free iambic B paddle keyer implementation.
 *
 * Runs entirely in the audio task (Core 1). Communicates with GPIO ISRs
 * via std::atomic flags only — no mutexes, no semaphores.
 *
 * ISR (any core):
 *   on DIT rising edge:  s_keyState.memory[idx(DIT)].store(SET)
 *   on DIT any edge:     s_keyState.state[idx(DIT)].store(pressed ? SET : UNSET)
 *   (same for DAH)
 *
 * Audio task (fillSamples):
 *   reads s_keyState.memory[] to decide what to play
 *   clears s_keyState.memory[own] when element ends AND paddle released
 *
 * Iambic B logic:
 *   - Start DIT or DAH when current_element == NONE and respective memory is set
 *   - On element end: clear own memory if paddle released
 *   - On element end: if opposite memory set → play opposite element immediately (iambic swap)
 *   - On element end: if own memory still set → auto-repeat with 1-dit inter-element space
 *   - On element end: if no memory set → return to NONE, mark last_element_end_frame
 *
 * Envelope design (cmorse-compatible):
 *   DIT envelope = 2 × ditLen samples: [ramp_up | flat | ramp_down | silence]
 *   DAH envelope = 4 × ditLen samples: [ramp_up | flat(3×) | ramp_down | silence]
 *   The trailing silence provides the intra-character gap.
 */

#include "iambic_keyer.h"
#include "key_envelop.h"
#include "Log.h"
#include <cstring>
#ifndef UNIT_TEST
#include "morse_key.h"  // for PIN_KEY_DIT, PIN_KEY_DAH, gpio_get_level
#endif

KeyState s_keyState;

// ---------------------------------------------------------------------------
// Public methods
// ---------------------------------------------------------------------------

/**
 * Initialize the keyer.
 *
 * Builds the sine LUT, resets all state, and inherits WPM from env.
 *
 * @param env Shared KeyEnvelop instance. Must outlive IambicKeyer.
 *            The same instance is used by MorseGenerator.
 */
void IambicKeyer::begin(KeyEnvelop* env) {
    _env = env;
    _wpm = env->wpm();
    fastMathInit();
    _currentElement = IAMBIC_ELEMENT_NONE;
    _elementSamplePos = 0;
    _elementTotalSamples = 0;
    _currentEnv = nullptr;
    _currentEnvSize = 0;
    _phase = 0.0f;
    _phaseInc = 0.0f;
    _totalSamplesRendered = 0;
    _lastElementEndFrame = 0;
    _spaceWrittenInIdle = false;
    _interElementSilenceSamples = 0;
    _rbHead = 0;
    _rbTail = 0;
}

/**
 * Update WPM on both the keyer and the shared KeyEnvelop.
 *
 * Called from UI task while audio may be active. The new WPM takes
 * effect at the next element boundary (no mutex needed — element
 * boundaries act as the synchronization point).
 *
 * @param wpm Words per minute (Paris standard: 1 WPM = 1 dit/sec = 1.2/WPM seconds per dit)
 */
void IambicKeyer::setWPM(int wpm) {
    _wpm = wpm;
    if (_env) _env->setWPM(wpm);
}

/**
 * Begin playing a DIT or DAH element.
 *
 * Resets envelope playback position, clears inter-element silence counter,
 * and fetches the correct envelope from KeyEnvelop. If WPM has changed,
 * the KeyEnvelop lazy-regenerates its buffers at this point.
 *
 * @param idx DIT_IDX (0) or DAH_IDX (1)
 */
void IambicKeyer::startElement(int idx) {
    _currentElement = idx;
    _elementSamplePos = 0;
    _interElementSilenceSamples = 0;
    if (_env && _env->wpm() != _wpm) {
        _wpm = _env->wpm();
    }
    if (_env) {
        auto el = (idx == DIT_IDX) ? KeyEnvelop::Element::DIT : KeyEnvelop::Element::DAH;
        _currentEnv = _env->envelope(el);
        _currentEnvSize = _env->envelopeSize(el);
        _elementTotalSamples = static_cast<int>(_currentEnvSize);
    }
}

/**
 * Drain inter-element silence one sample at a time.
 *
 * When the counter reaches zero, the loop continues to the IDLE check
 * rather than immediately starting the next element. This defers the
 * element start to the NEXT fillSamples call, ensuring the 1-dit gap
 * between consecutive same-element repetitions is preserved.
 *
 * @param sampleRate Audio sample rate (used for phase accumulation during silence)
 */
void IambicKeyer::checkWordSpace(int sampleRate) {
    if (_lastElementEndFrame == 0) return;
    int ditSamples = ditLengthSamples(sampleRate);
    if (_totalSamplesRendered - _lastElementEndFrame > MorseConstants::WORD_SPACE_DITS * (uint64_t)ditSamples) {
        rbWrite(DECODER_SPACE_CHAR);
        _lastElementEndFrame = _totalSamplesRendered;
    }
}

/**
 * Core audio generation loop.
 *
 * Called from AudioEngine::fillBuffer when s_keyState.memory indicates paddle
 * activity. Produces CW audio shaped by the KeyEnvelop Blackman-Harris ramp.
 *
 * State machine:
 *   IDLE (currentElement == NONE):
 *     - Check word-space gap → write ' ' to decoder if gap > 7 dit-lengths
 *     - If memory[DIT] set → start DIT
 *     - If memory[DAH] set → start DAH
 *     - Else → fill with silence, return
 *
 *   PLAYING (element active):
 *     - Generate sine × envelope sample
 *     - Advance phase and envelope position
 *     - At element boundary:
 *         - Write '.' or '-' to decoder
 *         - Clear own memory if paddle released (state == UNSET)
 *         - If memory[opp] set → SWAP to opposite element immediately
 *         - Else if memory[own] set → set interElementSilenceSamples (autorepeat)
 *         - Else → end of character, set currentElement = NONE, write '*'
 *
 * @param mono       Output mono buffer (must hold frames samples)
 * @param frames     Number of samples to fill
 * @param toneHz     Sidetone frequency in Hz (e.g. 500.0f)
 * @param amp        Output amplitude multiplier (0..32767)
 * @param sampleRate Audio sample rate in Hz (e.g. 48000)
 * @return Number of frames filled (always == frames, unless IDLE with no memory)
 */
size_t IambicKeyer::fillSamples(int16_t* mono, size_t frames,
                                 float toneHz, int16_t amp, int sampleRate) {
    _phaseInc = toneHz / static_cast<float>(sampleRate);
    int ditSamples = ditLengthSamples(sampleRate);
    size_t frameIdx = 0;

    while (frameIdx < frames) {

        // IDLE — check if a new element should start
        if (_currentElement == IAMBIC_ELEMENT_NONE) {
            // DBG: report gap every 1000th call in IDLE so we can see _totalSamplesRendered growing
            static uint64_t dbgCtr = 0;
            ++dbgCtr;
            if (_lastElementEndFrame != 0 && !_spaceWrittenInIdle) {
                uint64_t gap = _totalSamplesRendered - _lastElementEndFrame;
                uint64_t thresh = MorseConstants::WORD_SPACE_DITS * (uint64_t)ditSamples;
                if (gap > thresh) {
                    rbWrite(DECODER_SPACE_CHAR);
                    _spaceWrittenInIdle = true;
                    _lastElementEndFrame = _totalSamplesRendered;  // reset so next gap=0
                }
            }
            
#ifndef UNIT_TEST
            // Validate: if memory says pressed but GPIO says released, clear it.
            // Catches spurious rising edges or lost ISR transitions.
            if (atomic_load(&s_keyState.memory[DIT_IDX])) {
                if (gpio_get_level(PIN_KEY_DIT) != 0) {
                    atomic_store(&s_keyState.memory[DIT_IDX], MEMORY_UNSET);
                    Log::debug("[KEYER] ################IDLE VALIDATE: cleared memory[DIT] gpio=RELEASED");
                }
            }
            if (atomic_load(&s_keyState.memory[DAH_IDX])) {
                if (gpio_get_level(PIN_KEY_DAH) != 0) {
                    atomic_store(&s_keyState.memory[DAH_IDX], MEMORY_UNSET);
                    Log::debug("[KEYER] ############IDLE VALIDATE: cleared memory[DAH] gpio=RELEASED");
                }
            }
#endif

            if (atomic_load(&s_keyState.memory[DIT_IDX])) {
                _spaceWrittenInIdle = false;  // reset on new paddle press
                startElement(DIT_IDX);
            } else if (atomic_load(&s_keyState.memory[DAH_IDX])) {
                _spaceWrittenInIdle = false;
                startElement(DAH_IDX);
            } else {
                for (; frameIdx < frames; ++frameIdx) mono[frameIdx] = 0;
                _totalSamplesRendered += frames;
                // checkWordSpace is NOT called here — IDLE branch above already handles
                // word-space detection and resets _lastElementEndFrame after writing.
                // Calling checkWordSpace would write duplicate spaces.
                return frames;
            }
        }

        // Generate one element sample
        float envVal = 0.0f;
        if (_currentEnv && _elementSamplePos < (int)_currentEnvSize) {
            envVal = _currentEnv[_elementSamplePos];
        }

        float s = envVal * fastSinNormalized(_phase);
        mono[frameIdx] = static_cast<int16_t>(s * amp);

        _phase += _phaseInc;
        if (_phase >= 1.0f) _phase -= 1.0f;
        ++_elementSamplePos;
        ++_totalSamplesRendered;
        ++frameIdx;

        // --- Element boundary ---
        if (_elementSamplePos >= _elementTotalSamples) {
            char sym = (_currentElement == IAMBIC_ELEMENT_DIT) ? DECODER_DIT_SYMBOL : DECODER_DAH_SYMBOL;

            int ownIdx = _currentElement;
            int oppIdx = (ownIdx == DIT_IDX) ? DAH_IDX : DIT_IDX;

            // Track end of every element — needed for correct word-space detection
            _lastElementEndFrame = _totalSamplesRendered;
            rbWrite(sym);
            // Clear own memory if paddle has been released
            if (!atomic_load(&s_keyState.state[ownIdx])) {
                atomic_store(&s_keyState.memory[ownIdx], MEMORY_UNSET);
            }
/*
#ifndef UNIT_TEST
            // Validate: if memory says pressed but GPIO says released, clear it.
            // Catches lost falling edges where ISR didn't fire.
            if (atomic_load(&s_keyState.memory[ownIdx])) {
                gpio_num_t pin = (ownIdx == DIT_IDX) ? PIN_KEY_DIT : PIN_KEY_DAH;
                if (gpio_get_level(pin) != 0) {
                    atomic_store(&s_keyState.memory[ownIdx], MEMORY_UNSET);
                    Log::debug("[KEYER] VALIDATE: cleared memory[%d] gpio=RELEASED", ownIdx);
                }
            }
            if (atomic_load(&s_keyState.memory[oppIdx])) {
                gpio_num_t pin = (oppIdx == DIT_IDX) ? PIN_KEY_DIT : PIN_KEY_DAH;
                if (gpio_get_level(pin) != 0) {
                    atomic_store(&s_keyState.memory[oppIdx], MEMORY_UNSET);
                    Log::debug("[KEYER] VALIDATE: cleared memory[%d] gpio=RELEASED", oppIdx);
                }
            }
#endif
*/
            _currentElement = IAMBIC_ELEMENT_NONE;

            // Iambic B decision — three independent branches:
            if (atomic_load(&s_keyState.memory[oppIdx])) {
                // SWAP: opposite paddle memory set → play opposite immediately
//                Log::debug("[KEYER] SWAP -> startElement(%d)", oppIdx);
                startElement(oppIdx);
            } else if (atomic_load(&s_keyState.memory[ownIdx])) {
                // AUTOREPEAT: same paddle still held → start same element again
             //   Log::debug("[KEYER] AUTOREPEAT -> startElement(%d)", ownIdx);
                startElement(ownIdx);
            } else {
                // END OF CHARACTER: neither paddle has memory set
                rbWrite(DECODER_END_OF_CHAR);
                _spaceWrittenInIdle = false;  // reset so word space detection can fire next time
                _lastElementEndFrame = _totalSamplesRendered;  // must be non-zero for word-space detection
                Log::debug("[KEYER] END_CHAR → lastElemEnd=%llu", (unsigned long long)_lastElementEndFrame);
            }
        }
    }

    return frames;
}

// ---------------------------------------------------------------------------
// Decoder ring buffer
// ---------------------------------------------------------------------------

/**
 * Write a symbol to the decoder ring buffer.
 *
 * Called by fillSamples at element boundaries and by checkWordSpace
 * when a word gap is detected. This is the producer side; loop() consumes
 * via decoderRead().
 *
 * @param c Symbol: '.' (DIT), '-' (DAH), '*' (end of character), ' ' (word space)
 */
void IambicKeyer::rbWrite(char c) {
    _rb[_rbHead] = c;
    _rbHead = (_rbHead + 1) % RB_SIZE;
}

/**
 * Non-blocking read from the decoder ring buffer.
 *
 * Call this from loop() to receive decoded symbols produced by fillSamples.
 * Uses a single-pointer atomic tail — safe for one consumer (loop()) and one
 * producer (audio task filling samples).
 *
 * @param out Pointer to char that receives the symbol
 * @return true if a symbol was read, false if buffer is empty
 */
bool IambicKeyer::decoderRead(char* out) {
    if (_rbHead == _rbTail) return false;
    *out = _rb[_rbTail];
    _rbTail = (_rbTail + 1) % RB_SIZE;
    return true;
}

/**
 * @return Number of symbols currently available in the decoder ring buffer.
 *         Used by isActive() to detect pending decoder output.
 */
size_t IambicKeyer::decoderAvailable() const {
    ssize_t diff = static_cast<ssize_t>(_rbHead) - static_cast<ssize_t>(_rbTail);
    if (diff >= 0) return static_cast<size_t>(diff);
    return RB_SIZE - static_cast<size_t>(-diff);
}

// ---------------------------------------------------------------------------
// Timing helpers
// ---------------------------------------------------------------------------

/**
 * @return Number of samples in one dit at the current WPM and given sample rate.
 *
 * Paris standard: dit duration = 1.2 / WPM seconds
 * Therefore: ditSamples = sampleRate × 1.2 / WPM
 *
 * @param sampleRate Audio sample rate in Hz (e.g. 48000)
 */
int IambicKeyer::ditLengthSamples(int sampleRate) const {
    return (int)(((float)sampleRate * 1.2f) / (float)_wpm + 0.5f);
}