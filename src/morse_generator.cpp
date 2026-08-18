#include "morse_generator.h"
#include "display_model.h"
#include "display_task.h"
#include "key_event_bus.h"
#include "Log.h"
#ifndef UNIT_TEST
#include <Arduino.h>
#else
#include "../test/mocks/Arduino.h"
#endif
#include <cmath>
#include <cstring>
// MorseGenerator drives the audio pipeline (sidetone) AND is the
// on-air keying source for stored-text playback (P-key "Hello Morse!"
// from main.cpp, WinKey-emulated host playback from WinkeyBridge, and
// the memory-keyer feature). The edge-detect block below in
// advanceToNextElement() fires KeyEventBus::keyDown() / keyUp() at
// every dit/dah boundary so on-air transmission happens through the
// same central dispatcher that the paddle, straight key, and held-K
// use. The actual radio GPIO is gated inside RadioKeyer by its own
// _enabled flag, so the operator's KEYING setting remains the
// authoritative on/off for RF output. See docs/keyer.md §"Behavior".
//
// Screensaver-wake-up (`DisplayTask::wakeFromScreensaver()`) is
// bumped on every key-down element so the screen unblanks during
// stored-text and WinKey playback — the same way the paddle ISR
// bumps it for manual keying. See docs/winkey.md § 16.9.
//
// ─── Cross-core invariant ────────────────────────────────────────────────
// advanceToNextElement() is called from BOTH Core 0 (playText()'s
// single initial advance) and Core 1 (fillSamplesMono()'s loop). The
// shared _wasElKeyDown flag is touched only at the bottom of advance,
// and the existing ordering invariant established at playText() time
// (publish _state = PLAYING only AFTER the first advance, so the audio
// task that preempts mid-advance sees IDLE and bails) guarantees that
// during playback only ONE core touches advance. Outside of those,
// the public stop() transitions the state under Core 0 with no audio
// task running. So the bool needs no atomic protection.
// Cross-core reference: src/morse_generator.cpp:119-131 (the original
// race fix comment). See docs/memory.md for the memory-keyer plan.

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
void MorseGenerator::playText(const char* text, bool isContinuation) {
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
    // Only the WinKeyBridge calls playText() with isContinuation=true
    // (for chunks after the first in a multi-chunk host-driven
    // session); every other caller — keypad digit-N, web UI ▶, etc.
    // — leaves it at the default false. The OLD design used the
    // _wasPlaying flag (set on every previous playText, never reset
    // except by stop()) to gate the prepend, which caused the
    // back-to-back independent playback bug: after a keypad or web
    // playback the flag stayed true forever, so the next web-UI tap
    // prepended a CHAR_SPACE, shifting _charIdx by one and making the
    // decoded text land with every space one position to the right
    // ("cqc qj j1qpb/1j…" instead of "cq cq jj1qpb/1…"). The bridge
    // now passes isContinuation=true only for chunks whose audio
    // actually follows the previous chunk's audio (WinkeyBridge
    // tracks that state internally via _isContinuation).
    //
    // The prepend conditions are unchanged: only fire when the
    // previous chunk did not already end with a boundary silence
    // (which would compound the gap) and when the first element of
    // the new chunk is a mark (prepending before a silence would
    // stack a gap on top of an existing one).
    //
    // The prepended CHAR_SPACE uses units=2 to match what
    // MorseEncoder emits internally — the trailing 1 unit of
    // envelope silence from the previous chunk's last mark
    // completes the spec's 3-unit inter-character gap. Using
    // units=3 here would produce a 4-unit gap (1 too long).
    bool didPrepend = false;
    bool wasBoundaryBefore  = _endedWithBoundarySilence;
    if (isContinuation && !_endedWithBoundarySilence
        && !_elements.empty() && _elements.front().keyDown) {
        MorseEncoder::Element boundary(MorseEncoder::Element::CHAR_SPACE, 2, false);
        _elements.insert(_elements.begin(), boundary);
        didPrepend = true;
        // Mark the prepend's silence so advanceToNextElement skips the
        // decoded-char append when this silence element runs. Without
        // this, the prepend's silence branch appends _currentChar
        // (= chunk2's first char, because playText reset it) which is
        // not what just finished keying — chunk1's exhausted branch
        // already appended chunk1's last char. See regression
        // test_bridge_one_char_at_a_time_long_call_no_shift.
        _suppressNextSilenceAppend = true;
    }
    // _wasPlaying is preserved for any external code that wants to
    // know "has this generator ever been started since construction
    // or last stop()". It is NOT consulted by the prepend logic
    // anymore (see the regression test below). Set it true here so
    // stop() callers can still distinguish a started-but-stopped
    // generator from a never-started one if they need to.
    _wasPlaying               = true;
    _endedWithBoundarySilence = false;  // recomputed below in advanceToNextElement

    _elIdx = 0;
    _elSamplePos = 0;
    _currentChar = _playText.empty() ? '\0' : _playText[0];
    _wasElKeyDown = false;
    _radioElementKeyed = false;
    _phase = 0.0f;  // reset sine phase so next tone starts at zero
    _phaseInc = 0.0f;
    // CRITICAL ORDERING: set up the first element BEFORE flipping
    // `_state = PLAYING`. Otherwise the audio task (Core 1, priority 22)
    // can preempt playText() between `_state = PLAYING` and
    // `advanceToNextElement()` below, see `state == PLAYING` with
    // `_elKeyDown=0, _elTotalSamples=0`, call its own advance, and
    // start playing the first mark. When playText() resumes and calls
    // its own advance, it advances PAST the first mark into the
    // inter-character silence, and the audio task's already-running
    // fillSamplesMono now produces silence + U — i.e. T is inaudible.
    // Fix: do the advance FIRST, then publish PLAYING. Audio task that
    // preempts mid-advance will still see `state != PLAYING` (we haven't
    // published yet) and bail out via the IDLE/END branch.
    MorseModel::instance().resetPlayerHead();  // fresh session, reset player color tracking
    // Advance FIRST, before publishing PLAYING. This sets up _elKeyDown,
    // _elTotalSamples, _currentEnv, etc. atomically (from the audio task's
    // perspective) so a preemption between advance and publish still leaves
    // _state != PLAYING — the audio task sees IDLE and skips.
    advanceToNextElement();
    // After advance: if the encoder produced at least one element,
    // _elIdx was incremented past it. For empty input (playText(""))
    // advance hits the exhausted branch, sets _state = IDLE, and
    // _elIdx stays 0 — do NOT publish PLAYING in that case.
    if (!_elements.empty()) {
        _state = State::PLAYING;  // publish AFTER first element is set up
    }
    // (resetPlayerHead + advanceToNextElement moved above, BEFORE
    // `_state = PLAYING`, to close the audio-task race.)
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
    _radioElementKeyed = false;
    // Unkey the radio if we were mid-mark. Always safe — the bus
    // saturates the refcount at 0 so extra keyUp() calls are no-ops.
    if (_wasElKeyDown) {
        KeyEventBus::keyUp();
        _wasElKeyDown = false;
    }
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
        // Playback completed naturally — unkey the radio if the
        // last element was a mark. Symmetric with the stop() path.
        if (_wasElKeyDown) {
            KeyEventBus::keyUp();
            _wasElKeyDown = false;
        }
        return;
    }

    const MorseEncoder::Element& el = _elements[_elIdx];
    _elKeyDown = el.keyDown;
    _elSamplePos = 0;

    // Edge-detect the key-down flag into KeyEventBus. Symmetric with
    // the paddle ISR (`MorseKey::isrDit` / `isrDah` firing KeyEventBus
    // on every mark boundary), so on-air keying from a stored-text
    // playback behaves like physical paddles. The bus's atomic refcount
    // (`std::atomic<int>`, lock-free from any context) makes these
    // calls safe from whichever core is advancing.
    //
    // The edge detector itself is plain `bool` — see the cross-core
    // invariant comment block at the top of this file.
    if (_elKeyDown && !_wasElKeyDown) {
        KeyEventBus::keyDown();
    } else if (!_elKeyDown && _wasElKeyDown) {
        KeyEventBus::keyUp();
    }
    _wasElKeyDown = _elKeyDown;

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
        // Radio keying window: DIT = 1*ditLen, DAH = 3*ditLen. The envelope
        // continues for the trailing ramp-down + 1-unit silence (DIT total =
        // 2*ditLen, DAH total = 4*ditLen), but that trailing portion must
        // NOT keep the transmitter keyed. Matches iambic_keyer.cpp
        // `_elementKeyedSamples`. Without this, "OO" keys GPIO4 HIGH for the
        // entire 480 ms (3 DAH envelopes back-to-back, no inter-element gap
        // in the encoder), and the radio hears a single long dash instead
        // of three.
        int ditLen = _env->ditLengthSamples();
        _elKeyedSamples = (el.type == MorseEncoder::Element::DIT) ? ditLen : (3 * ditLen);
        _radioElementKeyed = false;  // arm the keyed-boundary keyUp() latch
                                       // for this new element
        // BUGFIX: the encoder emits one WORD_SPACE per ASCII space in the
        // source text. The silence branch only advances _charIdx by one
        // position per silence element, so _charIdx can still point at a
        // space when the next mark starts. Without this skip, _currentChar
        // would be set to ' ' (a space) instead of the next character, and
        // the subsequent CHAR_SPACE append would write ' ' instead of the
        // expected letter to the display — the user saw "CQ " (trailing
        // space) while the audio was keying the second C of "CQ CQ".
        while (_charIdx < _playText.size() && _playText[_charIdx] == ' ') {
            ++_charIdx;
        }
        // Always update _currentChar to the character whose mark we're playing.
        // This ensures the right char is captured at the boundary.
        _currentChar = (_charIdx < _playText.size()) ? _playText[_charIdx] : '\0';
        Log::write("[MG] mark: charIdx=%zu/%zu char='%c'(%d) elType=%d\n",
            (unsigned)_charIdx, _playText.size(),
            (unsigned char)_currentChar >= 32 ? (unsigned char)_currentChar : '?',
            (unsigned char)_currentChar,
            (int)elType);
    } else {
        // Silence element: duration = units × dit length.
        // CRITICAL: use the units value the encoder stored, NOT a hardcoded
        // 3/7. The encoder already accounts for the 1-unit trailing silence
        // the KeyEnvelop envelope provides at the end of the previous mark:
        //   MORSE_SPACE = 2 units  (CHAR_SPACE)  → 1 (env trailing) + 2 = 3 spec units
        //   MORSE_SPACE = 6 units  (WORD_SPACE)  → 1 (env trailing) + 6 = 7 spec units
        // Hardcoding 3/7 here would produce 4/8 units total — one unit too long.
        // That 1-unit excess has been observed to make the first play after a
        // power-cycle sound slightly wrong (inter-character gap too long blends
        // with the next mark's rise, "TU" can be heard as something other than
        // "TU"). See docs/winkey.md "Inter-character silence timing".
        int unitCount = (el.type == MorseEncoder::Element::WORD_SPACE)    ? el.units
                     : (el.type == MorseEncoder::Element::CHAR_SPACE)     ? el.units
                     : 1;  // ELEMENT_SPACE
        _elTotalSamples = _encoder.ditLengthSamples(_env->sampleRate()) * unitCount;
        _currentEnv = nullptr;
        _currentEnvSize = 0;
        _elRampSamples = 0;
        _elKeyedSamples = 0;  // silence: radio stays unkeyed

        // WORD_SPACE or CHAR_SPACE marks end of current character
        if (el.type == MorseEncoder::Element::WORD_SPACE || el.type == MorseEncoder::Element::CHAR_SPACE) {
            // When the bridge streams text in multiple chunks and the
            // prepend fires, the prepended CHAR_SPACE is purely an
            // audio bridge — it must NOT trigger a decoded-char
            // append, because no character has just finished keying
            // (chunk2 hasn't started yet; chunk1's last char was
            // already appended by chunk1's exhausted branch).
            // Consume the flag and fall through to the
            // _currentChar/_charIdx advance below (we still need to
            // move past this silence element so the next mark's
            // skip-spaces loop sees the right _charIdx).
            const bool suppressAppend = _suppressNextSilenceAppend;
            _suppressNextSilenceAppend = false;
            if (_playText[_charIdx] != '\0') {
                if (!suppressAppend) {
                    // Letter finished — append to shared decoded text buffer
                    uint32_t now = millis();
                    Log::write("[MG] APPEND t=%u char='%c' playPos=%zu/%zu\n",
                        now,
                        (unsigned char)_currentChar >= 32 ? (unsigned char)_currentChar : '?',
                        (unsigned)_charIdx, _playText.size());
                    MorseModel::instance().appendDecodedChar(_currentChar, true);
                    // For WORD_SPACE, append the inter-word space separator
                    // so the device / web UI display shows the gap during
                    // the WS silence — in sync with the audio gap. Without
                    // this, the space only appears after the next word's
                    // audio (the mark branch reassigns _currentChar to the
                    // next char, the next CHAR_SPACE picks up the space,
                    // and the user sees the space land one word late).
                    //
                    // Two guards keep the leading/trailing-space behaviour
                    // unchanged:
                    //   - _currentChar != ' '   : leading-space WS shouldn't
                    //                              count as a word boundary.
                    //   - peek past trailing spaces : the WS at the end of
                    //                              the source is followed by
                    //                              only spaces (or nothing);
                    //                              those trailing spaces are
                    //                              appended by the exhausted
                    //                              branch below.
                    if (el.type == MorseEncoder::Element::WORD_SPACE
                        && _currentChar != ' ') {
                        size_t peek = _charIdx + 1;
                        while (peek < _playText.size() && _playText[peek] == ' ') {
                            ++peek;
                        }
                        if (peek < _playText.size()) {
                            MorseModel::instance().appendDecodedChar(' ', true);
                        }
                    }
                    // Advance past this silence element so subsequent
                    // CHAR_SPACEs know which char was just keyed.
                    ++_charIdx;
                    _currentChar = _playText[_charIdx];
                }
                // suppressAppend path (prepend): do NOT advance
                // _charIdx or change _currentChar. The prepend's
                // CHAR_SPACE is purely an audio bridge — it provides
                // the inter-character gap the encoder omits at chunk
                // tail, but no character has just finished keying
                // (chunk2's first mark hasn't played yet). The next
                // mark's skip-spaces loop still runs from the same
                // _charIdx (0), finds the first non-space char at the
                // same source position playText() initialised it to,
                // and sets _currentChar correctly.
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
                // Radio keying: drop the line at the END of the keyed
                // portion of the element, NOT at the envelope boundary
                // (which includes the trailing ramp-down + 1-unit silence
                // that must NOT keep the transmitter keyed). Mirrors
                // iambic_keyer.cpp `_elementKeyedSamples`. Latched per
                // element: _wasElKeyDown is updated by advanceToNextElement
                // and would normally produce the same edge there, but the
                // envelope continues past the keyed window, so we fire
                // here instead.
                if (!_radioElementKeyed && _elKeyedSamples > 0
                    && _elSamplePos + 1 >= _elKeyedSamples) {
                    KeyEventBus::keyUp();
                    _radioElementKeyed = true;
                    // Mirror the radio state into _wasElKeyDown so the
                    // NEXT advanceToNextElement sees a proper edge when
                    // the next mark element starts (otherwise it would
                    // see _wasElKeyDown still true and skip the keyDown
                    // edge, leaving the radio LOW for the next mark).
                    _wasElKeyDown = false;
                }
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