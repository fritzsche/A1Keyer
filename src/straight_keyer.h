#pragma once
/**
 * straight_keyer.h — Lock-free straight key (single-contact) keyer.
 *
 * Operator timing determines element type:
 *   Short press (~1 dit) → DIT
 *   Long press (~3 dits) → DAH
 *   Adaptive threshold classifies each release as DIT or DAH.
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
 * via decodeFromLoop(), with microsecond timestamps to compute element duration
 * and silence gap. This replaces the sample-counting approach in fillSamples.
 */

#include <cstdint>
#include <atomic>
#include <cmath>
#include "fast_math.h"
#include "morse_constants.h"
#include "iambic_keyer.h"
#include "key_envelop.h"

// ---------------------------------------------------------------------------
// StraightKeyer
// ---------------------------------------------------------------------------
class StraightKeyer {
public:
    /** Key event written to ring buffer — matches extern/main.c design.
     *  event: KEY_DOWN = 0 (key pressed), KEY_UP = 1 (key released)
     *  frame: sample count at time of event
     */
    struct KeyEvent {
        static constexpr char KEY_DOWN = 0;   // key pressed, new element starting
        static constexpr char KEY_UP   = 1;   // key released, element finished
        char event;   // KEY_DOWN or KEY_UP
        uint64_t frame;  // _totalSamplesRendered at time of event
    };

    static constexpr char DECODER_END_OF_CHAR = MorseConstants::END_OF_CHAR;
    static constexpr char DECODER_SPACE_CHAR   = MorseConstants::SPACE_CHAR;
    static constexpr char DECODER_DIT_SYMBOL   = MorseConstants::DIT_SYMBOL;
    static constexpr char DECODER_DAH_SYMBOL   = MorseConstants::DAH_SYMBOL;

    /** Ring buffer size — 1024 timestamped events (matching extern/main.c). */
    static constexpr size_t RB_SIZE = 1024;

    void begin();
    void setWPM(int wpm);

    size_t fillSamples(int16_t* mono, size_t frames,
                       float toneHz, int16_t amp, int sampleRate);

    bool isActive() const { return _state != State::IDLE; }

    /** Read next key event from ring buffer (for decodeFromLoop consumption). */
    bool decoderRead(KeyEvent* out);

    /** Read next symbol char from ring buffer (legacy — for MorseDecoder compatibility). */
    bool decoderRead(char* out);

    size_t decoderAvailable() const;
    std::atomic<size_t> rbHead = {0};
    std::atomic<size_t> rbTail = {0};

    /** Get event at ring buffer index (for unit tests). */
    KeyEvent rbAt(size_t idx) const { return _rb[idx % RB_SIZE]; }
    uint64_t lastElementEndFrame() const { return _lastElementEndFrame; }
    uint64_t totalSamplesRendered() const { return _totalSamplesRendered; }

    /** Called from loop() on Core 0 to decode straight key timing. */
    void decodeFromLoop(int sampleRate);

    void reset();

private:
    void rbWriteEvent(char event, uint64_t frame);
    void addDitDah(char sym);
    void flushDitDahBuffer();

    static float bhRamp(float t) {
        const float pi_t = M_PI * t;
        return 0.5f * (1.0f - std::cos(pi_t));
    }

    static float bhFallRamp(float t) {
        return 0.5f * (1.0f + std::cos(M_PI * t));
    }

    enum class State { IDLE, RISE, HOLD, FALL };

    State _state = State::IDLE;

    // ─── Decode state (for timestamp-based decoding) ────────────────────
    /** Word-space detection: gap must exceed this many dit-lengths. */
    static constexpr int WORD_SPACE_DITS = 5;
    /** Intra-character gap detection: gap must exceed this many dit-lengths. */
    static constexpr int INTRA_CHAR_DITS = 2;
    uint64_t _lastDownFrame = 0;   // frame of last DOWN event
    uint64_t _lastUpFrame = 0;     // frame of last UP event
    uint64_t _charTimeoutFrame = 0; // frame when character timeout fires
    int _framesPerDit = 0;          // frames per dit at current WPM (set via setWPM)
    char _ditDahBuf[16] = {0};      // accumulated dit/dah buffer
    size_t _ditDahLen = 0;          // current length of dit/dah buffer

    // ─── Envelope ────────────────────────────────────────────────────────────
    static constexpr int RAMP_LEN = 256;
    float _riseRamp[RAMP_LEN];
    float _fallRamp[RAMP_LEN];
    int _rampSamples = 0;
    int _rampPos = 0;

    // ─── Sine phase ─────────────────────────────────────────────────────────
    float _phase = 0.0f;
    float _phaseInc = 0.0f;

    // ─── Timing / samples ──────────────────────────────────────────────────
    uint64_t _totalSamplesRendered = 0;
    uint64_t _lastElementEndFrame = 0;

    // ─── WPM ───────────────────────────────────────────────────────────────
    int _wpm = 20;

    // ─── Decoder ring buffer (non-volatile, guarded by atomic head/tail) ────
    KeyEvent _rb[RB_SIZE];
};