#pragma once
/**
 * iambic_keyer.h — Lock-free iambic B paddle keyer.
 *
 * Runs entirely in the audio task (Core 1). Communicates with GPIO ISRs
 * via std::atomic flags only — no mutexes, no semaphores.
 *
 * ISR (any core):
 *   on DIT rising edge:  s_keyState.memory[idx(DIT)].store(SET)
 *   on DIT any edge:      s_keyState.state[idx(DIT)].store(pressed ? SET : UNSET)
 *   (same for DAH)
 *
 * Audio task (fillBuffer):
 *   reads s_keyState.memory[] to decide what to play
 *   clears s_keyState.memory[own] when element ends AND paddle released
 *
 * Iambic B logic:
 *   - Start DIT or DAH when current_element == NONE and respective memory is set
 *   - On element end: clear own memory if paddle released
 *   - On element end: if opposite memory set → play opposite element immediately
 *   - On element end: if own memory still set → auto-repeat with 1-dit inter-element space
 *   - On element end: if no memory set → return to NONE, mark last_element_end_frame
 */

#include <cstdint>
#include <atomic>
#include "fast_math.h"
#include "morse_constants.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int DIT_IDX = 0;
static constexpr int DAH_IDX = 1;
static constexpr int MEMORY_SET = 1;
static constexpr int MEMORY_UNSET = 0;

static constexpr int IAMBIC_ELEMENT_DIT = 0;
static constexpr int IAMBIC_ELEMENT_DAH = 1;
static constexpr int IAMBIC_ELEMENT_NONE = 2;

// ---------------------------------------------------------------------------
// KeyState — shared between ISR and audio thread
// ---------------------------------------------------------------------------
struct KeyState {
    // Memory: ISR sets on rising edge (press), audio clears when element ends + paddle released
    std::atomic<int> memory[2];
    // Physical paddle state: ISR sets on every edge
    std::atomic<int> state[2];
};
extern KeyState s_keyState;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class KeyEnvelop;

// ---------------------------------------------------------------------------
// IambicKeyer
// ---------------------------------------------------------------------------
/**
 * Lock-free iambic B paddle keyer.
 *
 * Runs entirely in the audio task (Core 1). Communicates with GPIO ISRs
 * via std::atomic flags only — no mutexes, no semaphores.
 *
 * ISR (any core):
 *   on DIT rising edge:  s_keyState.memory[DIT].store(SET)
 *   on DIT any edge:     s_keyState.state[DIT].store(pressed ? SET : UNSET)
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
 */
class IambicKeyer {
public:
    static constexpr int DIT = 0;
    static constexpr int DAH = 1;

    /** Decoder symbol aliases — see MorseConstants for definitions. */
    static constexpr char DECODER_END_OF_CHAR = MorseConstants::END_OF_CHAR;
    static constexpr char DECODER_SPACE_CHAR   = MorseConstants::SPACE_CHAR;
    static constexpr char DECODER_DIT_SYMBOL   = MorseConstants::DIT_SYMBOL;
    static constexpr char DECODER_DAH_SYMBOL   = MorseConstants::DAH_SYMBOL;

    /** Size of the lock-free decoder ring buffer. */
    static constexpr size_t RB_SIZE = 256;

    /**
     * Initialize the keyer with a shared KeyEnvelop instance.
     * @param env KeyEnvelop (shared with MorseGenerator). Must outlive IambicKeyer.
     */
    void begin(KeyEnvelop* env);

    /**
     * Update WPM on both the keyer and the shared KeyEnvelop.
     * Safe to call from UI task while audio is active — applied at next element boundary.
     * @param wpm Words per minute (Paris standard: 1 WPM = 1 dit/sec)
     */
    void setWPM(int wpm);

    /**
     * Fill a mono sample buffer with CW audio for iambic keyer.
     *
     * Call this when s_keyState.memory[DIT/DAH] indicates paddle activity.
     * Generates sine-wave CW shaped by the KeyEnvelop Blackman-Harris ramp.
     *
     * @param mono      Output buffer (must hold frames samples)
     * @param frames    Number of stereo frames to fill
     * @param toneHz    Sidetone frequency in Hz (e.g. 500.0f)
     * @param amp       Output amplitude (0..32767)
     * @param sampleRate Audio sample rate in Hz (e.g. 48000)
     * @return Number of frames actually filled (always == frames, unless no memory set)
     */
    size_t fillSamples(int16_t* mono, size_t frames,
                       float toneHz, int16_t amp, int sampleRate);

    /**
     * True when the keyer is active — playing an element, draining inter-element
     * silence, or has pending paddle memory that will start a new element.
     * Also true when a character has ended and the word-space gap has NOT yet
     * been detected (so the audio engine keeps calling fillSamples to grow
     * _totalSamplesRendered and detect the gap). Once the space is written,
     * this returns false until a new element starts.
     */
    bool isActive() const {
        return _currentElement != IAMBIC_ELEMENT_NONE
            || _interElementSilenceSamples > 0
            || atomic_load(&s_keyState.memory[DIT_IDX])
            || atomic_load(&s_keyState.memory[DAH_IDX])
            || (_lastElementEndFrame != 0 && !_spaceWrittenInIdle);
    }

    /**
     * Non-blocking read from the decoder ring buffer.
     * Call this from loop() to receive decoded symbols.
     * @param out Pointer to char that receives the symbol ('.', '-', '*', ' ')
     * @return true if a symbol was read, false if buffer is empty
     */
    bool decoderRead(char* out);

    /**
     * @return Number of symbols available in the decoder ring buffer.
     */
    size_t decoderAvailable() const;

    /**
     * @return Current ring buffer head index (for observation without consuming).
     */
    size_t rbHead() const { return _rbHead; }

    /**
     * @return Current ring buffer tail index (for observation without consuming).
     */
    size_t rbTail() const { return _rbTail; }

    /**
     * @param idx Ring buffer index (wraps via % RB_SIZE)
     * @return Symbol stored at ring buffer position idx.
     */
    char rbAt(size_t idx) const { return _rb[idx % RB_SIZE]; }

    /**
     * @return Sample count of the last element end frame.
     */
    uint64_t lastElementEndFrame() const { return _lastElementEndFrame; }

    /**
     * @return Total samples rendered so far.
     */
    uint64_t totalSamplesRendered() const { return _totalSamplesRendered; }

    /**
     * @return Number of samples in one dit at the current WPM.
     * @param sampleRate Audio sample rate in Hz
     */
    int ditLengthSamples(int sampleRate) const;

private:
    /**
     * Begin playing a DIT or DAH element.
     * Resets envelope position, clears inter-element silence, fetches the
     * correct envelope from KeyEnvelop (triggering lazy regeneration if needed).
     * @param idx DIT_IDX (0) or DAH_IDX (1)
     */
    void startElement(int idx);

    /**
     * Write a symbol to the decoder ring buffer (producer side).
     * Called by fillSamples at element boundaries and when word-space is detected.
     * @param c Symbol: '.', '-', '*', or ' '
     */
    void rbWrite(char c);

    /**
     * Check if the gap since lastElementEndFrame exceeds 7 dit-lengths and
     * therefore qualifies as a word space. If so, writes DECODER_SPACE_CHAR.
     * Called during idle when memory is set but before starting a new element.
     * @param sampleRate Audio sample rate for dit-length calculation
     */
    void checkWordSpace(int sampleRate);

    // Element generation state
    int _currentElement = IAMBIC_ELEMENT_NONE;
    int _elementSamplePos = 0;
    int _elementTotalSamples = 0;
    const float* _currentEnv = nullptr;
    size_t _currentEnvSize = 0;

    // Inter-element silence: 1-dit pause between auto-repeat elements
    int _interElementSilenceSamples = 0;

    float _phase = 0.0f;
    float _phaseInc = 0.0f;

    // Shared envelope
    KeyEnvelop* _env = nullptr;
    int _wpm = 20;
    uint32_t _envVersion = 0;
    uint32_t _lastEnvVersion = 0;

    // Timing
    uint64_t _totalSamplesRendered = 0;
    uint64_t _lastElementEndFrame = 0;
    bool _spaceWrittenInIdle = false;  // guards against writing multiple spaces per idle period

    // Decoder ring buffer (producer: audio, consumer: loop())
    volatile char _rb[RB_SIZE];
    volatile size_t _rbHead = 0;
    volatile size_t _rbTail = 0;
};