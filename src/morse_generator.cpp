#include "morse_generator.h"
#include "display_model.h"
#ifndef UNIT_TEST
#include <Arduino.h>
#else
#include "../test/mocks/Arduino.h"
#endif
#include <cmath>
#include <cstring>

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
    if (!_env) { Serial.println("[MG] debugDumpEnvelope: no env"); return; }
    int sr        = _env->sampleRate();
    int ditSamp   = _env->ditLengthSamples();
    int rampSamp  = (int)_env->rampLengthSamples();
    size_t ditEnvSz = _env->envelopeSize(KeyEnvelop::Element::DIT);
    size_t dahEnvSz = _env->envelopeSize(KeyEnvelop::Element::DAH);

    Serial.printf("[MG] --- envelope dump (wpm=%d sr=%d) ---\n", _env->wpm(), sr);
    Serial.printf("[MG]   dit_samples=%d  ramp_samples=%d\n", ditSamp, rampSamp);
    Serial.printf("[MG]   dit_env_size=%u  dah_env_size=%u\n",
                  (unsigned)ditEnvSz, (unsigned)dahEnvSz);
    Serial.printf("[MG]   dit total ms=%.2f  dah total ms=%.2f\n",
                  ditEnvSz * 1000.0f / sr, dahEnvSz * 1000.0f / sr);

    // Print first 8 and last 8 samples of DIT envelope to verify ramp shape
    const float* ditEnv = _env->envelope(KeyEnvelop::Element::DIT);
    Serial.print("[MG]   DIT env[0..7]:  ");
    for (int i = 0; i < 8 && i < (int)ditEnvSz; ++i)
        Serial.printf("%.3f ", ditEnv[i]);
    Serial.println();
    Serial.print("[MG]   DIT env[-8..-1]: ");
    for (int i = (int)ditEnvSz - 8; i < (int)ditEnvSz; ++i)
        Serial.printf("%.3f ", i >= 0 ? ditEnv[i] : 0.0f);
    Serial.println();

    // Verify silence timing (no ramp correction needed — envelope is exactly 1 unit long)
    int elSpaceSamp   = ditSamp * 1;
    int charSpaceSamp = ditSamp * 3;
    int wordSpaceSamp = ditSamp * 7;
    Serial.printf("[MG]   silence ELEMENT_SPACE=%d CHAR_SPACE=%d WORD_SPACE=%d samples\n",
                  elSpaceSamp, charSpaceSamp, wordSpaceSamp);
    Serial.println("[MG] --- end dump ---");
}

// ---------------------------------------------------------------------------
// Start playing a text string
// ---------------------------------------------------------------------------
void MorseGenerator::playText(const char* text) {
    _playText = text;
    _charIdx = 0;
    _elements = _encoder.encode(text);
    _elIdx = 0;
    _elSamplePos = 0;
    _state = State::PLAYING;
    _currentChar = text[0];
    _phase = 0.0f;  // reset sine phase so next tone starts at zero
    _phaseInc = 0.0f;
    Serial.printf("[MG] playText: text=\"%s\" elements=%zu\n", text, (unsigned)_elements.size());
    MorseModel::instance().resetPlayerHead();  // fresh session, reset player color tracking
    // Do NOT clear the buffer — append to existing keyer text
    advanceToNextElement();
    Serial.printf("[MG] after advance: currentChar='%c'(%d) isPlaying=%d\n",
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
}

// ---------------------------------------------------------------------------
// Advance to the next element and set up its envelope/sample info
// ---------------------------------------------------------------------------
void MorseGenerator::advanceToNextElement() {
    Serial.printf("[MG] advanceToNext: elIdx=%zu size=%zu charIdx=%zu/%zu char='%c'(%d)\n",
        (unsigned)_elIdx, (unsigned)_elements.size(),
        (unsigned)_charIdx, strlen(_playText),
        _charIdx < strlen(_playText) ? _playText[_charIdx] : '?',
        (unsigned char)(_charIdx < strlen(_playText) ? _playText[_charIdx] : 0));
    if (_elIdx >= _elements.size()) {
        // All elements exhausted — append all remaining characters.
        // Every character that was played as a mark but had no trailing CHAR_SPACE
        // needs to be appended here. These are exactly the characters from
        // _charIdx onwards (each was advanced past but never had a CHAR_SPACE).
        while (_charIdx < strlen(_playText)) {
            char c = _playText[_charIdx];
            Serial.printf("[MG] boundary: appending char='%c' at idx=%zu\n",
                (unsigned char)c >= 32 ? (unsigned char)c : '?', (unsigned)_charIdx);
            MorseModel::instance().appendDecodedChar(c, true);
            ++_charIdx;
        }
        _state = State::IDLE;
        _elKeyDown = false;
        _currentChar = '\0';
        return;
    }

    const MorseEncoder::Element& el = _elements[_elIdx];
    _elKeyDown = el.keyDown;
    _elSamplePos = 0;

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
        Serial.printf("[MG] mark: charIdx=%zu/%zu char='%c'(%d) elType=%d\n",
            (unsigned)_charIdx, strlen(_playText),
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
                Serial.printf("[MG] APPEND t=%u char='%c' playPos=%zu/%zu\n",
                    now,
                    (unsigned char)_currentChar >= 32 ? (unsigned char)_currentChar : '?',
                    (unsigned)_charIdx, strlen(_playText));
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