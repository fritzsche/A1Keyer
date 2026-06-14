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
     * @param text  NUL-terminated string.
     */
    void playText(const char* text);

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
    const char* _playText = "";  // original text being played (for display indexing)
    size_t _charIdx = 0;    // current character index in _playText

    // Current element info
    bool   _elKeyDown = false;
    int    _elRampSamples = 0;

    // Sine phase for tone generation
    float  _phase = 0.0f;
    float  _phaseInc = 0.0f;

    // Cached envelope pointers
    const float* _currentEnv = nullptr;
    size_t       _currentEnvSize = 0;

    static float silenceEnvelope(float phase);
};