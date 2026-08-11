#include "morse_generator.h"
#include "display_model.h"
#include "display_task.h"
#include "Log.h"
#ifndef UNIT_TEST
#include <Arduino.h>
#else
#include "../test/mocks/Arduino.h"
#endif
#include <cmath>
#include <cstring>
// NOTE: MorseGenerator (player, e.g. P-key "Hello Morse!" playback) is
// intentionally NOT wired to KeyEventBus in this change. The future
// Winkey-compatible player will emit KeyEventBus::keyDown() / keyUp()
// here at every dit/dah boundary so on-air transmissions can be
// triggered by stored text. See docs/keyer.md §"Behavior".
//
// The screensaver-wake-up signal (`DisplayTask::wakeFromScreensaver()`)
// IS bumped on every key-down element below so the screen unblanks
// during stored-text or WinKey-emulation playback — the same way the
// paddle ISR bumps it for manual keying. See docs/winkey.md § 16.9.

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
MorseGenerator::MorseGenerator(KeyEnvelop* env, int wpm)
    : _env(env)
    , _encoder(wpm)
    , _wpm(wpm)
{
}

// ---------------------------------------------------------------------------
// Debug: dump envelope parameters and first/last samples to Serial
// ---------------------------------------------------------------------------
void MorseGenerator::debugDumpEnvelope() const {
    if (!_env) { Log::write("[MG] debugDumpEnvelope: no env\r\n"); return; }
    int sr        = _env->sampleRate();
    int ditSamp   = _env->ditLengthSamples();
    int rampSamp  = (int)_env->rampLengthSamples();
    size_t ditEnvSz = _env->envelopeSize(KeyEnvelop::Element::DIT);
    size_t dahEnvSz = _env->envelopeSize(KeyEnvelop::Element::DAH);

    Log::write("[MG] --- envelope dump (wpm=%d sr=%d) ---\n", _env->wpm(), sr);
    Log::write("[MG]   dit_samples=%d  ramp_samples=%d\n", ditSamp, rampSamp);
    Log::write("[MG]   dit_env_size=%u  dah_env_size=%u\n",
                  (unsigned)ditEnvSz, (unsigned)dahEnvSz);
    Log::write("[MG]   dit total ms=%.2f  dah total ms=%.2f\n",
                  ditEnvSz * 1000.0f / sr, dahEnvSz * 1000.0f / sr);

    // Print first 8 and last 8 samples of DIT envelope to verify ramp shape
    const float* ditEnv = _env->envelope(KeyEnvelop::Element::DIT);
    Log::write("[MG]   DIT env[0..7]:  ");
    for (int i = 0; i < 8 && i < (int)ditEnvSz; ++i)
        Log::write("%.3f ", ditEnv[i]);
    Log::write("\r\n");
    Log::write("[MG]   DIT env[-8..-1]: ");
    for (int i = (int)ditEnvSz - 8; i < (int)ditEnvSz; ++i)
        Log::write("%.3f ", i >= 0 ? ditEnv[i] : 0.0f);
    Log::write("\r\n");

    // Verify silence timing (no ramp correction needed — envelope is exactly 1 unit long)
    int elSpaceSamp   = ditSamp * 1;
    int charSpaceSamp = ditSamp * 3;
    int wordSpaceSamp = ditSamp * 7;
    Log::write("[MG]   silence ELEMENT_SPACE=%d CHAR_SPACE=%d WORD_SPACE=%d samples\n",
                  elSpaceSamp, charSpaceSamp, wordSpaceSamp);
    Log::write("[MG] --- end dump ---\r\n");
}

// ---------------------------------------------------------------------------
// Start playing a text string
// ---------------------------------------------------------------------------
void MorseGenerator::playText(const char* text) {
    // COPY the text — never store the caller's pointer. The WinKey
    // bridge hands us a stack-local chunk in WinkeyBridge::poll();
    // after that call returns the chunk is gone and the audio task
    // would read garbage. The std::string member keeps the text alive
    // for the whole playback. See docs/winkey.md "Text playback and
    // the audio click bug".
    if (text) {
        _playText.assign(text);
    } else {
        _playText.clear();
    }
    _charIdx = 0;
    _elements = _encoder.encode(_playText.c_str());

    // Preserve inter-character silence across chunk boundaries.
    // If the previous chunk ended naturally (not via stop()) AND
    // did not already emit a trailing CHAR/WORD_SPACE, prepend a
    // CHAR_SPACE so the host's split-streaming does not eat the
    // gap. Skip when the encoder produced no elements (empty
    // input) or when the first element is itself a silence
    // (would compound an existing gap). See docs/winkey.md § 13.6.
    if (_wasPlaying && !_endedWithBoundarySilence
        && !_elements.empty() && _elements.front().keyDown) {
        MorseEncoder::Element boundary(MorseEncoder::Element::CHAR_SPACE, 3, false);
        _elements.insert(_elements.begin(), boundary);
    }
    _wasPlaying               = true;
    _endedWithBoundarySilence = false;  // recomputed below in advanceToNextElement

    _elIdx = 0;
    _elSamplePos = 0;
    _state = State::PLAYING;
    _currentChar = _playText.empty() ? '\0' : _playText[0];
    _phase = 0.0f;  // reset sine phase so next tone starts at zero
    _phaseInc = 0.0f;
    Log::write("[MG] playText: text=\"%s\" elements=%zu\n",
                  _playText.c_str(), (unsigned)_elements.size());
    MorseModel::instance().resetPlayerHead();  // fresh session, reset player color tracking
    // Do NOT clear the buffer — append to existing keyer text
    advanceToNextElement();
    Log::write("[MG] after advance: currentChar='%c'(%d) isPlaying=%d\n",
        (int)_currentChar >= 32 ? (int)_currentChar : '?',
        (int)(unsigned char)_currentChar,
        (int)(_state != State::IDLE));
}

// ---------------------------------------------------------------------------
// Stop playback
// ---------------------------------------------------------------------------
void MorseGenerator::stop() {
    _state = State::IDLE;
    _elIdx = 0;
    _elSamplePos = 0;
    _elKeyDown = false;
    _phase = 0.0f;
    // stop() is an intentional reset — next playText() starts
    // fresh without a synthetic leading CHAR_SPACE.
    _wasPlaying = false;
    _endedWithBoundarySilence = false;
}

// ---------------------------------------------------------------------------
// Advance to the next element and set up its envelope/sample info
// ---------------------------------------------------------------------------
void MorseGenerator::advanceToNextElement() {
    Log::write("[MG] advanceToNext: elIdx=%zu size=%zu charIdx=%zu/%zu char='%c'(%d)\n",
        (unsigned)_elIdx, (unsigned)_elements.size(),
        (unsigned)_charIdx, _playText.size(),
        _charIdx < _playText.size() ? _playText[_charIdx] : '?',
        (unsigned char)(_charIdx < _playText.size() ? _playText[_charIdx] : 0));
    if (_elIdx >= _elements.size()) {
        // All elements exhausted — append all remaining characters.
        // Every character that was played as a mark but had no trailing CHAR_SPACE
        // needs to be appended here. These are exactly the characters from
        // _charIdx onwards (each was advanced past but never had a CHAR_SPACE).
        while (_charIdx < _playText.size()) {
            char c = _playText[_charIdx];
            Log::write("[MG] boundary: appending char='%c' at idx=%zu\n",
                (unsigned char)c >= 32 ? (unsigned char)c : '?', (unsigned)_charIdx);
            MorseModel::instance().appendDecodedChar(c, true);
            ++_charIdx;
        }
        // Record whether the last element of THIS chunk was a
        // boundary silence (CHAR_SPACE or WORD_SPACE). The encoder
        // emits these only *between* characters within a single
        // encode() call; a trailing one means the chunk ended at
        // a character/word boundary, so the next playText() does
        // not need to prepend another. See docs/winkey.md § 13.6.
        if (!_elements.empty()) {
            auto lastType = _elements.back().type;
            _endedWithBoundarySilence =
                (lastType == MorseEncoder::Element::CHAR_SPACE
              || lastType == MorseEncoder::Element::WORD_SPACE);
        }
        _state = State::IDLE;
        _elKeyDown = false;
        _currentChar = '\0';
        return;
    }

    const MorseEncoder::Element& el = _elements[_elIdx];
    _elKeyDown = el.keyDown;
    _elSamplePos = 0;

    // Screensaver wake-up: mirror the paddle-ISR behaviour
    // (`MorseKey::isrDit` / `isrDah` rising-edge wakeup at
    // src/morse_key.cpp:33, 48). Any CW element that goes key-down
    // counts as activity — the screen unblanks during stored-text
    // playback (Cardputer P-key) and during WinKey text playback
    // (RUMlogNG / N1MM typing into the outgoing-CW field). Cheap
    // enough to call on every mark boundary: it's a single
    // volatile-bool write, ISR-safe, and the display task drains
    // it once per 50 ms tick. See docs/winkey.md § 16.9.
    if (_elKeyDown) {
        DisplayTask::wakeFromScreensaver();
    }

    if (_elKeyDown) {
        auto elType = (el.type == MorseEncoder::Element::DIT) ? KeyEnvelop::Element::DIT
                                               : KeyEnvelop::Element::DAH;
        _currentEnv = _env->envelope(elType);
        _currentEnvSize = _env->envelopeSize(elType);
        _elRampSamples = static_cast<int>(_env->rampLengthSamples());
        _elTotalSamples = static_cast<int>(_currentEnvSize);
        // Always update _currentChar to the character whose mark we're playing.
        // This ensures the right char is captured at the boundary.
        _currentChar = _playText[_charIdx];
        Log::write("[MG] mark: charIdx=%zu/%zu char='%c'(%d) elType=%d\n",
            (unsigned)_charIdx, _playText.size(),
            (unsigned char)_currentChar >= 32 ? (unsigned char)_currentChar : '?',
            (unsigned char)_currentChar,
            (int)elType);
    } else {
        // Silence element: duration = units × dit length.
        int unitCount = (el.type == MorseEncoder::Element::WORD_SPACE)    ? 7
                     : (el.type == MorseEncoder::Element::CHAR_SPACE)     ? 3
                     : 1;  // ELEMENT_SPACE
        _elTotalSamples = _encoder.ditLengthSamples(_env->sampleRate()) * unitCount;
        _currentEnv = nullptr;
        _currentEnvSize = 0;
        _elRampSamples = 0;

        // WORD_SPACE or CHAR_SPACE marks end of current character
        if (el.type == MorseEncoder::Element::WORD_SPACE || el.type == MorseEncoder::Element::CHAR_SPACE) {
            if (_playText[_charIdx] != '\0') {
                // Letter finished — append to shared decoded text buffer
                uint32_t now = millis();
                Log::write("[MG] APPEND t=%u char='%c' playPos=%zu/%zu\n",
                    now,
                    (unsigned char)_currentChar >= 32 ? (unsigned char)_currentChar : '?',
                    (unsigned)_charIdx, _playText.size());
                MorseModel::instance().appendDecodedChar(_currentChar, true);
                ++_charIdx;
                _currentChar = _playText[_charIdx];
            }
        }
    }

    ++_elIdx;
}

// ---------------------------------------------------------------------------
// Update phase increment for a given tone frequency
// ---------------------------------------------------------------------------
void MorseGenerator::updatePhaseIncrement(float toneHz) {
    _phaseInc = toneHz / static_cast<float>(_env->sampleRate());
}

// ---------------------------------------------------------------------------
// Fill a stereo buffer with morse audio
// ---------------------------------------------------------------------------
void MorseGenerator::fillSamples(int16_t* left, int16_t* right,
                                 size_t frames, float toneHz, int16_t amp)
{
    fillSamplesMono(left, frames, toneHz, amp);
    // Right channel is identical (mono source)
    if (right != left) {
        ::memcpy(right, left, frames * sizeof(int16_t));
    }
}

void MorseGenerator::fillSamplesMono(int16_t* mono,
                                     size_t frames, float toneHz, int16_t amp)
{
    updatePhaseIncrement(toneHz);

    // Click detector: flag a large sample-to-sample jump (> 10% FS = 3276)
    static int16_t s_prevSample  = 0;
    static uint32_t s_clickCount = 0;

    for (size_t i = 0; i < frames; ++i) {
        float sample = 0.0f;

        if (_state == State::PLAYING) {
            if (_elKeyDown) {
                // Apply envelope to tone
                float envVal = 0.0f;
                if (_currentEnv && _elSamplePos < static_cast<int>(_currentEnvSize)) {
                    envVal = _currentEnv[_elSamplePos];
                }
                sample = envVal * fastSinNormalized(_phase);
            }
            // Advance counter for both tone and silence elements
            if (++_elSamplePos >= _elTotalSamples) {
                advanceToNextElement();
            }
        }

        // Advance sine phase continuously so the next tone starts at the right phase
        _phase += _phaseInc;
        if (_phase >= 1.0f) _phase -= 1.0f;

        int16_t s = static_cast<int16_t>(sample * amp);
                         
        s_prevSample = s;
        mono[i] = s;
    }
}