#pragma once
/**
 * audio_engine.h
 *
 * Ultra-low latency I2S audio engine for M5Stack Tab5 Morse Trainer.
 *
 * Design:
 *   - Directly drives the I2S peripheral via ESP-IDF 5.x i2s_std.h API,
 *     bypassing the M5Unified Speaker layer to achieve <3 ms output latency.
 *   - Uses 2 DMA buffers x AUDIO_DMA_BUF_LEN stereo frames; worst-case
 *     latency = 2 * (AUDIO_DMA_BUF_LEN / AUDIO_SAMPLE_RATE) ≈ 2.9 ms.
 *   - A dedicated high-priority FreeRTOS task on Core 1 calls i2s_channel_write
 *     in a tight loop, filling one 64-frame block per iteration.
 *   - ES8388 audio codec + PI4IOE5V6408 GPIO expander (amp enable) are
 *     configured over the internal I2C bus.
 *   - Headphone detection via PI4IOE5V6408 pin 7; call isHeadphoneInserted()
 *     to poll headphone jack status.
 *
 * Usage:
 *   1. Call AudioEngine::begin() once in setup() after M5.begin().
 *   2. Call AudioEngine::setToneActive(true/false) to gate the 500 Hz tone.
 *      Safe to call from ISR context.
 *   3. Call AudioEngine::end() to release resources (optional cleanup).
 */

#ifdef UNIT_TEST
#include "../test/mocks/Arduino.h"
#else
#include <Arduino.h>
#ifndef UNIT_TEST
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#endif
#include "morse_generator.h"
#include "iambic_keyer.h"
#include "straight_keyer.h"
#include "morse_decoder.h"

// ---------------------------------------------------------------------------
// Tuneable constants
// ---------------------------------------------------------------------------

/// I2S sample rate in Hz.  ES8388 MCLK = sample_rate * 256 (register 0x18).
static constexpr uint32_t AUDIO_SAMPLE_RATE = 48000;

/// Number of STEREO frames per DMA buffer (callback block size).
/// 64 frames @ 44100 Hz = 1.451 ms per buffer => worst-case latency ~2.9 ms.
static constexpr size_t AUDIO_DMA_BUF_LEN = 64;

/// Number of DMA buffers in the ring (minimum 2 for glitch-free output).
static constexpr size_t AUDIO_DMA_BUF_COUNT = 2;

/// Peak amplitude for a 0 dB (100%) volume setting, 16-bit signed PCM (0..32767).
/// Actual amplitude is scaled by the current volume setting via currentAmplitude().
static constexpr int16_t AUDIO_TONE_AMPLITUDE_MAX = 32767;

/// Default volume level (0–100%).
/// 50% → -6 dB → half perceived loudness (matches human hearing).
static constexpr int DEFAULT_VOLUME_PERCENT = 50;

/// Volume change step per button press.
static constexpr int VOLUME_STEP = 10;

// ---------------------------------------------------------------------------
// M5Stack Tab5 hardware pin assignments  (confirmed from M5Unified source)
// ---------------------------------------------------------------------------
#ifdef BOARD_TAB5
// All audio signals share a single I2S bus connected to the ES8388 codec.

static constexpr gpio_num_t PIN_I2S_MCK   = GPIO_NUM_30; ///< Master clock
static constexpr gpio_num_t PIN_I2S_BCLK  = GPIO_NUM_27; ///< Bit clock
static constexpr gpio_num_t PIN_I2S_LRCLK = GPIO_NUM_29; ///< LR clock / WS
static constexpr gpio_num_t PIN_I2S_DOUT  = GPIO_NUM_26; ///< Data out (DAC)

// ---------------------------------------------------------------------------
// ES8388 codec — I2C address and key register map
// ---------------------------------------------------------------------------
static constexpr uint8_t ES8388_I2C_ADDR = 0x10; ///< 7-bit address

// ---------------------------------------------------------------------------
// PI4IOE5V6408 GPIO expander — amplifier enable
// ---------------------------------------------------------------------------
static constexpr uint8_t PI4IO_I2C_ADDR   = 0x43; ///< 7-bit address
static constexpr uint8_t PI4IO_REG_OUTPUT = 0x05; ///< Output register
static constexpr uint8_t PI4IO_AMP_BIT    = (1u << 1); ///< Bit 1 = amp enable
static constexpr uint8_t PI4IO_HP_DET_PIN = 7;       ///< Headphone detect on PI4IOE pin 7

#elif defined(BOARD_CARDPUTER)
// ---------------------------------------------------------------------------
// M5Stack Cardputer — NS4168 I2S amplifier (no codec, no GPIO expander)
// I2S on ESP32-S3: G41=BCLK, G42=SDATA, G43=LRCLK
// No MCLK needed — NS4168 derives its clock from BCLK.
// ---------------------------------------------------------------------------
static constexpr gpio_num_t PIN_I2S_MCK   = GPIO_NUM_NC;   // not used
static constexpr gpio_num_t PIN_I2S_BCLK  = GPIO_NUM_41;
static constexpr gpio_num_t PIN_I2S_LRCLK = GPIO_NUM_43;
static constexpr gpio_num_t PIN_I2S_DOUT  = GPIO_NUM_42;

// No ES8388 on Cardputer — NS4168 is a direct I2S power amp.
// No GPIO expander — the 74HC138 handles keyboard only.
static constexpr uint8_t ES8388_I2C_ADDR = 0xFF; // unused
static constexpr uint8_t PI4IO_I2C_ADDR   = 0xFF; // unused
static constexpr uint8_t PI4IO_REG_OUTPUT = 0xFF; // unused
static constexpr uint8_t PI4IO_AMP_BIT    = 0;    // unused
static constexpr uint8_t PI4IO_HP_DET_PIN = 0;    // unused (no headphone jack)

#endif

// ---------------------------------------------------------------------------
// AudioEngine class
// ---------------------------------------------------------------------------

// Forward declaration (MorseGenerator is defined in cmorse/)
class MorseGenerator;

class AudioEngine {
public:
    /**
     * Initialise I2S, ES8388 codec, and amplifier.
     * Must be called from setup() after M5.begin() (Wire already started).
     * @return true on success.
     */
    static bool begin();

    /**
     * Release I2S channel and delete the audio task.
     */
    static void end();

    /**
     * Enable or disable the 500 Hz sine tone.
     * Thread-safe: callable from any task or ISR.
     * @param active true = tone on, false = silence.
     */
    static void setToneActive(bool active);

    /**
     * Poll headphone detect pin on PI4IOE5V6408.
     * @return true if headphone jack has a plug inserted, false if open.
     */
    static bool isHeadphoneInserted();

    /**
     * Switch audio output to headphones or speaker.
     * When headphones are inserted, LOUT2/ROUT2 (speaker amp) is disabled.
     * When headphones are removed, speaker outputs are re-enabled.
     * @param hp true = headphone mode (speaker muted), false = speaker mode.
     */
    static void setHeadphoneMode(bool hp);

    static MorseGenerator* s_morseGen;  ///< owned morse generator
    static IambicKeyer* s_keyer;      ///< owned iambic B keyer
    static StraightKeyer* s_straightKeyer;  ///< owned straight keyer
    static MorseGenerator* morseGen() { return s_morseGen; }
    static IambicKeyer* keyer() { return s_keyer; }
    static StraightKeyer* straightKeyer() { return s_straightKeyer; }
    static void createMorseGen();
    static void deleteMorseGen();

    // Volume control — logarithmic scale (human perception is logarithmic).
    // percent: 0–100, where 50% sounds half as loud as 100%.

    /** Get current volume (0–100%). */
    static int getVolumePercent();

    /**
     * Set volume, clamp to [0,100], and pre-compute the cached amplitude.
     * Safe to call from loop(); the audio task reads s_cachedAmplitude directly.
     */
    static void setVolumePercent(int percent);

    /**
     * Apply volume to the ES8388 DAC digital attenuators (regs 0x1A/0x1B).
     * Call after setVolumePercent() to also update the codec hardware gain.
     */
    static void applyCodecVolume();

    // Sidetone frequency — updated by MorseModel when user changes frequency

    /**
     * Get the current sidetone frequency in Hz.
     */
    static float getToneFrequency();

    /**
     * Set the sidetone frequency and recompute s_phaseIncrement.
     * Called by MorseModel::setFrequency() to propagate frequency changes to audio.
     */
    static void setToneFrequency(float hz);

private:
    static void  audioTask(void* param);
    static void  fillBuffer(int16_t* buf, size_t stereoFrames);
    static bool  initI2S();
    static bool  initCodec();
    static void  codecWrite(uint8_t reg, uint8_t value);
    static void  pi4ioWrite(uint8_t reg, uint8_t value);
    static void  ampEnable(bool on);
    static int16_t computeAmplitude(int percent);  // pure, no side effects

    static volatile bool    s_toneActive;
    static float            s_toneFrequency;
    static float            s_phase;
    static float            s_phaseIncrement;
    static int              s_volumePercent;
    // Pre-computed linear amplitude for the audio task — updated by setVolumePercent(),
    // read every DMA fill.  volatile prevents the compiler caching across task boundary.
    static volatile int16_t s_cachedAmplitude;

    // i2s_chan_handle_t is a pointer type defined in driver/i2s_types.h.
    // Forward-declared here to avoid pulling the full header into every TU.
    static void*          s_i2sTxHandle; ///< cast to i2s_chan_handle_t when used

#ifndef UNIT_TEST
    static TaskHandle_t  s_audioTaskHandle;  // needs freertos/task.h
#endif
};
