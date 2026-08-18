#pragma once
// Uncomment to enable per-element timing diagnostics on Serial.
// #define MORSE_DEBUG_ENVELOPE

/**
 * MorseGenerator — async morse code player.
 *
 * Encodes text via MorseEncoder, shapes marks with KeyEnvelop, and produces
 * signed 16-bit stereo samples ready for I2S DMA.
 *
 * Thread-safe: playText() can be called from setup() or loop(); all sample
 * generation happens inside fillSamples() which is called from the audio task.
 *
 * Usage:
 *   MorseGenerator morse(keyEnvelop, 20);   // 20 WPM
 *   morse.playText("CQ DE DJ5CQ");
 *   // later in the audio task loop:
 *   morse.fillSamples(left, right, stereoFrames, toneHz, amplitude);
 */

#include "key_envelop.h"
#include "morse_encoder.h"
#include "fast_math.h"
#include <cstdint>
#include <string>

// Forward declaration — MorseModel is defined in display_model.h which
// transitively includes audio_engine.h (needs ESP-IDF headers).
// morse_generator.cpp includes display_model.h directly; the .cpp file
// can call MorseModel::instance(). This avoids pulling ESP-IDF headers
// into every TU that includes morse_generator.h.
class MorseModel;

class MorseGenerator {
public:
    /**
     * @param env       Pointer to a KeyEnvelop instance (shared, must outlive this).
     * @param wpm       Initial words-per-minute.
     */
    explicit MorseGenerator(KeyEnvelop* env, int wpm = 20);

    /** Change WPM at any time. */
    void setWPM(int wpm) {
        _wpm = wpm;
        _encoder.setWPM(wpm);
        _env->setWPM(wpm);
    }

    int wpm() const { return _wpm; }

    /**
     * Start playing a text string as morse code.
     * Non-blocking: playback proceeds asynchronously in fillSamples().
     *
     * @param text              NUL-terminated string.
     * @param isContinuation    True if this `text` is a continuation of a
     *                          chunked playback — i.e. the previous chunk's
     *                          audio just finished and the encoder does not
     *                          emit a trailing inter-character silence, so a
     *                          CHAR_SPACE must be prepended to preserve the
     *                          spec's 3-unit inter-character gap. The
     *                          WinKeyBridge sets this for every chunk after
     *                          the first in a multi-chunk session. Default
     *                          `false` is for FRESH, INDEPENDENT playback
     *                          (e.g. operator presses digit-N on the keypad,
     *                          or taps ▶ in the web UI) — consecutive
     *                          independent playbacks must NOT prepend, or
     *                          the user's memory-slot text lands on the
     *                          display one char late ("cqc qj j1qpb/1j…"
     *                          instead of "cq cq jj1qpb/1…"). See
     *                          docs/winkey.md § 13.6 and the regression
     *                          test test_back_to_back_independent_playback_no_prepend.
     */
    void playText(const char* text, bool isContinuation = false);

    /**
     * Stop current playback immediately.
     */
    void stop();

    /** Dump envelope shape and timing to Serial for click debugging. */
    void debugDumpEnvelope() const;

    /** @return true if morse is currently being played. */
    bool isPlaying() const { return _state != State::IDLE; }

    /**
     * @return The character currently being played (decoded from the text).
     * Returns 0 when idle.
     */
    char currentChar() const { return _currentChar; }

    // ----- Sample generation (called from audio task / loop) -----

    /**
     * Fill a stereo sample buffer with the next chunk of morse audio.
     *
     * @param left      Pointer to left-channel int16_t buffer (interleaved).
     * @param right     Pointer to right-channel int16_t buffer (same size).
     * @param frames    Number of stereo frames to fill.
     * @param toneHz    Tone frequency in Hz (e.g. 500).
     * @param amp       Peak amplitude (int16_t, e.g. 16384 = 50 % FS).
     */
    void fillSamples(int16_t* left, int16_t* right,
                     size_t frames, float toneHz, int16_t amp);

    /**
     * Same as above but for a mono buffer (L and R written with same data).
     */
    void fillSamplesMono(int16_t* mono,
                         size_t frames, float toneHz, int16_t amp);

private:
    enum class State { IDLE, PLAYING, END };

    void updatePhaseIncrement(float toneHz);
    void advanceToNextElement();

    // Shared resources (not owned)
    KeyEnvelop*  _env;
    MorseEncoder _encoder;

    int _wpm;

    // Playback state
    State _state = State::IDLE;
    std::vector<MorseEncoder::Element> _elements;
    size_t _elIdx = 0;       // index into _elements
    int    _elSamplePos = 0; // current position within the element's envelope
    int    _elTotalSamples = 0; // total samples for current element
    char   _currentChar = 0;   // current character being played (for display)
    // _playText is COPIED into the generator, not borrowed by pointer.
    // The WinKey bridge hands the player a stack-local buffer in
    // WinkeyBridge::poll(); if we just stored that pointer, the audio
    // task would walk into freed stack memory and the decoder would
    // see junk bytes ('#', '?', NUL) instead of the real text. See
    // docs/winkey.md "Text playback and the audio click bug".
    std::string _playText;
    size_t _charIdx = 0;    // current character index in _playText

    // Current element info
    bool   _elKeyDown = false;
    int    _elRampSamples = 0;

    // Chunk-boundary tracking for inter-character silence. The
    // MorseEncoder only emits CHAR_SPACE *between* characters within
    // a single encode() call — it never emits a trailing CHAR_SPACE
    // after the last character. When the bridge streams text in
    // multiple bursts (RUMlogNG sends "T" then later "U"), each
    // burst becomes a separate playText() call; without the fix
    // below, "U" would start immediately after T's DAH with no
    // inter-character silence, producing "TU" as "T·U" (shorter
    // than the spec's 3-unit CHAR_SPACE). See docs/winkey.md § 13.6.
    //   _wasPlaying               — true after the first playText()
    //                                that started a fresh playback.
    //                                Reset by stop() and by the
    //                                constructor; only set true
    //                                inside playText().
    //   _endedWithBoundarySilence — true when the last element of
    //                                the *previous* chunk was a
    //                                CHAR_SPACE or WORD_SPACE (i.e.
    //                                already includes the inter-
    //                                character / inter-word gap).
    //                                When true, the next playText()
    //                                does NOT prepend another.
    bool   _wasPlaying = false;
    bool   _endedWithBoundarySilence = false;

    // Edge-detect state for the KeyEventBus bridge: tracks the
    // previous element's key-down flag so advanceToNextElement can
    // fire keyDown() on a 0→1 transition and keyUp() on a 1→0.
    // Not atomic: only one core advances the generator at a time —
    // playText() does its single advance on Core 0 before publishing
    // _state = PLAYING, and from then on only fillSamplesMono() (Core 1)
    // drives advanceToNextElement. See morse_generator.cpp § Cross-
    // core invariant in the wiring block.
    bool   _wasElKeyDown = false;

    // When the bridge streams text in multiple chunks and the
    // prepend fires, the prepended CHAR_SPACE is purely an audio
    // bridge — it preserves the inter-character gap that the
    // encoder omits at chunk tail. It must NOT trigger a
    // decoded-char append, because no character has just finished
    // keying (chunk2 hasn't started yet) and chunk1's last char
    // was already appended by chunk1's exhausted branch. Setting
    // this flag tells advanceToNextElement's silence branch to
    // consume the prepend's silence WITHOUT calling
    // appendDecodedChar(). The flag is cleared after the prepend's
    // silence element is processed.
    bool   _suppressNextSilenceAppend = false;

    // Sine phase for tone generation
    float  _phase = 0.0f;
    float  _phaseInc = 0.0f;

    // Cached envelope pointers
    const float* _currentEnv = nullptr;
    size_t       _currentEnvSize = 0;

    static float silenceEnvelope(float phase);
};