/**
 * audio_engine.cpp — Ultra-low latency I2S audio for M5Stack Tab5.
 *
 * Uses the ESP-IDF 5.x i2s_std.h API (i2s_new_channel /
 * i2s_channel_init_std_mode / i2s_channel_write).  The legacy
 * driver/i2s.h API does NOT exist on ESP32-P4.
 *
 * Audio path:
 *   ESP32-P4  ──I2S──►  ES8388 DAC  ──analog──►  Speaker / headphone
 *
 * Amplifier enable:
 *   PI4IOE5V6408 GPIO expander (I2C 0x43), register 0x05 bit 1.
 */
#include "audio_engine.h"
#include "Log.h"
#include "fast_math.h"
#include "morse_generator.h"
#include "key_envelop.h"
#include "iambic_keyer.h"
#include "straight_keyer.h"
#include "display_model.h"
#include <M5Unified.h>
#include <M5Unified.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <cmath>

// ---------------------------------------------------------------------------
// Static member definitions
// ---------------------------------------------------------------------------
volatile bool  AudioEngine::s_toneActive     = false;
float          AudioEngine::s_toneFrequency   = 600.0f;
float          AudioEngine::s_phase          = 0.0f;
float          AudioEngine::s_phaseIncrement = 0.0f;
TaskHandle_t   AudioEngine::s_audioTaskHandle = nullptr;
void*          AudioEngine::s_i2sTxHandle    = nullptr;
MorseGenerator* AudioEngine::s_morseGen      = nullptr;
IambicKeyer*   AudioEngine::s_keyer        = nullptr;
StraightKeyer* AudioEngine::s_straightKeyer = nullptr;
int            AudioEngine::s_volumePercent   = DEFAULT_VOLUME_PERCENT;
// Cached linear amplitude — updated by setVolumePercent(), read by audio task.
// Declared volatile so the compiler does not cache the value across the task boundary.
volatile int16_t AudioEngine::s_cachedAmplitude = AUDIO_TONE_AMPLITUDE_MAX / 2;

// ---------------------------------------------------------------------------
// Volume control
// ---------------------------------------------------------------------------
int16_t AudioEngine::computeAmplitude(int percent) {
    if (percent <= 0)  return 0;
    if (percent >= 100) return AUDIO_TONE_AMPLITUDE_MAX;
    // Logarithmic: dB = 20×log10(percent/100), linear = max × 10^(dB/20) = max × percent/100
    // Simplifies to a linear scale — but perceived loudness is logarithmic so we keep
    // the dB calculation so that 50% volume sounds half as loud as 100%.
    float db  = 20.0f * std::log10f(static_cast<float>(percent) / 100.0f);
    float amp = AUDIO_TONE_AMPLITUDE_MAX * std::powf(10.0f, db / 20.0f);
    return static_cast<int16_t>(amp + 0.5f);
}

void AudioEngine::setVolumePercent(int percent) {
    s_volumePercent  = (percent < 0) ? 0 : (percent > 100) ? 100 : percent;
    // Pre-compute and cache amplitude so the audio task never calls transcendental
    // functions in the real-time path.
    s_cachedAmplitude = computeAmplitude(s_volumePercent);
}

int AudioEngine::getVolumePercent() {
    return s_volumePercent;
}

float AudioEngine::getToneFrequency() {
    return s_toneFrequency;
}

void AudioEngine::setToneFrequency(float hz) {
    if (hz < 300.0f) hz = 300.0f;
    if (hz > 900.0f) hz = 900.0f;
    s_toneFrequency = hz;
    s_phaseIncrement = 2.0f * (float)M_PI * hz / (float)AUDIO_SAMPLE_RATE;
    s_phase = 0.0f;
}

// ---------------------------------------------------------------------------
// ES8388 DAC volume — sets digital attenuation registers directly.
// Reg 0x1A = LDACVOL, reg 0x1B = RDACVOL.  Each step = 0.5 dB attenuation.
// 0x00 = 0 dB (max), 0xFF = −127.5 dB (inaudible).
//
// NOTE: do NOT write reg 0x1C here.  Reg 0x1C is DACCONTROL4 (DACLRCKDIV /
// DACFSMODE).  Toggling it for a "ramp" is incorrect and causes a brief DAC
// reconfiguration glitch → audible click.  The ES8388 has no "click-free ramp"
// register for volume changes; click-free transitions are achieved in the
// digital domain by the software envelope (key_envelop.cpp).
// ---------------------------------------------------------------------------
void AudioEngine::applyCodecVolume() {
    uint8_t code = 0;
    if (s_volumePercent > 0) {
        float db = 20.0f * std::log10f(static_cast<float>(s_volumePercent) / 100.0f);
        int   c  = static_cast<int>(std::roundf(-2.0f * db));
        code = (c > 0xFF) ? 0xFF : static_cast<uint8_t>(c);
    } else {
        code = 0xFF;
    }
    codecWrite(0x1A, code);
    codecWrite(0x1B, code);
    Log::debug("[AudioEngine] codec vol: %d%% → reg 0x%02X", s_volumePercent, code);
}

// ---------------------------------------------------------------------------
// I2C helpers  (M5.In_I2C is initialised by M5.begin())
// ---------------------------------------------------------------------------
#ifdef BOARD_TAB5
void AudioEngine::codecWrite(uint8_t reg, uint8_t value) {
    uint8_t tries = 3;
    while (tries--) {
        if (M5.In_I2C.writeRegister(ES8388_I2C_ADDR, reg, &value, 1, 400000)) return;
        delay(1);
    }
    Log::error("[AE] codecWrite(0x%02X, 0x%02X) FAILED", reg, value);
}

void AudioEngine::pi4ioWrite(uint8_t reg, uint8_t value) {
    M5.In_I2C.writeRegister(PI4IO_I2C_ADDR, reg, &value, 1, 400000);
}

// ---------------------------------------------------------------------------
// Amplifier enable / disable via PI4IOE5V6408
// ---------------------------------------------------------------------------
void AudioEngine::ampEnable(bool on) {
    uint8_t cur = 0x00;
    M5.In_I2C.readRegister(PI4IO_I2C_ADDR, PI4IO_REG_OUTPUT, &cur, 1, 400000);
    // SPK_EN = bit 1, EXT5V_EN = bit 5 — both must be set for speaker audio
    uint8_t mask = PI4IO_AMP_BIT | (1u << 5);
    uint8_t next = on ? (cur | mask) : (cur & ~mask);
    pi4ioWrite(PI4IO_REG_OUTPUT, next);
    Log::info("[AudioEngine] amp %s (cur=0x%02X -> 0x%02X)", on ? "ON" : "OFF", cur, next);
}
#elif defined(BOARD_CARDPUTER)
// Cardputer has no GPIO expander — NS4168 amp is always on once I2S is running.
void AudioEngine::ampEnable(bool on) {
    (void)on;
    // NS4168 power is managed by its own enable pin tied to the ESP32's power rail.
    // No software control needed — I2S data on SD creates output automatically.
}
#endif

// ---------------------------------------------------------------------------
// ES8388 initialisation  (DAC-only path, I2S slave 44.1 kHz 16-bit stereo)
// ---------------------------------------------------------------------------
bool AudioEngine::initCodec() {
    // --- Reset sequence (matches M5Unified enabled_bulk_data) ---
    codecWrite(0x00, 0x80); delay(10);  // RESET / CSM POWER ON
    codecWrite(0x00, 0x00); delay(5);   // clear reset
    codecWrite(0x00, 0x00); delay(5);   // settling
    codecWrite(0x00, 0x0E); delay(5);   // operational mode

    // --- Clock and power ---
    codecWrite(0x01, 0x00);            // CLKMANAGER 1
    codecWrite(0x02, 0x0A); delay(5);   // CHIP POWER: power up all analog blocks
    codecWrite(0x03, 0xFF);             // ADC POWER: power down all ADCs (DAC-only)

    // --- DAC power-up (BEFORE setting low power modes per M5Unified) ---
    codecWrite(0x04, 0x3C); delay(5);   // DAC POWER: power up, LOUT1/ROUT1/LOUT2/ROUT2 enable

    // --- Low power modes (AFTER DAC power per M5Unified) ---
    codecWrite(0x05, 0x00);             // ChipLowPower1
    codecWrite(0x06, 0x00);             // ChipLowPower2
    codecWrite(0x07, 0x7C);             // VSEL
    codecWrite(0x08, 0x00);             // I2S slave mode

    // --- I2S format (reg 0x17) and MCLK ratio (reg 0x18) ---
    codecWrite(0x17, 0x18);             // I2S format: 16-bit Philips
    codecWrite(0x18, 0x00);             // MCLK ratio: 128x (MCLK = Fs * 128)

    // --- DAC control ---
    codecWrite(0x19, 0x20);             // DAC unmute
    codecWrite(0x1A, 0x00);             // LDACVOL: 0 dB (max)
    codecWrite(0x1B, 0x00);             // RDACVOL: 0 dB (max)
    codecWrite(0x1C, 0x08);            // enable digital click-free power up/down
    codecWrite(0x1D, 0x00);             // reserved

    // --- Output mixer ---
    codecWrite(0x26, 0x00);             // DAC CTRL16
    codecWrite(0x27, 0xB8);             // LEFT Ch MIX: DAC left to mixer
    codecWrite(0x2A, 0xB8);             // RIGHT Ch MIX: DAC right to mixer
    codecWrite(0x2B, 0x08);            // ADC/DAC separate mode

    // --- VREF and pad registers ---
    codecWrite(0x2D, 0x00);             // VREF: 1.5k mode
    codecWrite(0x2E, 0x21);             // pad
    codecWrite(0x2F, 0x21);             // pad
    codecWrite(0x30, 0x21);             // pad
    codecWrite(0x31, 0x21);             // pad

    Log::info("[AudioEngine] ES8388 init OK");
    return true;
}

// ---------------------------------------------------------------------------
// I2S initialisation — ESP-IDF 5.x i2s_std.h (required for ESP32-P4)
// ---------------------------------------------------------------------------
bool AudioEngine::initI2S() {
    // 1. Allocate a TX channel on I2S_NUM_0
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = (int)AUDIO_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = (int)AUDIO_DMA_BUF_LEN;
    chan_cfg.auto_clear    = true; // silence on underrun

    i2s_chan_handle_t tx_handle = nullptr;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle, nullptr);
    if (err != ESP_OK) {
        Log::error("[AE] i2s_new_channel fail: %d", err);
        return false;
    }

    // 2. Configure standard I2S (Philips) mode matching M5Unified for ESP32P4
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg.sample_rate_hz = AUDIO_SAMPLE_RATE;
#if defined(BOARD_CARDPUTER)
    // ESP32-S3 uses PLL_160M as clock source — I2S_CLK_SRC_DEFAULT fails on S3
    // with error "esp_clk_tree_src_get_freq_hz: unknown clk src".
    std_cfg.clk_cfg.clk_src = i2s_clock_src_t::I2S_CLK_SRC_PLL_160M;
    std_cfg.clk_cfg.mclk_multiple = i2s_mclk_multiple_t::I2S_MCLK_MULTIPLE_128;
#else
    // ESP32-P4 uses the default clock source
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
#endif

    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_mode      = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width       = 16;
    std_cfg.slot_cfg.ws_pol         = false;
    std_cfg.slot_cfg.bit_shift      = true;   // M5Unified sets this (default is false)
    std_cfg.slot_cfg.left_align     = true;   // ESP32P4 path
    std_cfg.slot_cfg.big_endian     = false;
    std_cfg.slot_cfg.bit_order_lsb  = false;

    std_cfg.gpio_cfg.mclk = PIN_I2S_MCK;
    std_cfg.gpio_cfg.bclk = PIN_I2S_BCLK;
    std_cfg.gpio_cfg.ws   = PIN_I2S_LRCLK;
    std_cfg.gpio_cfg.dout = PIN_I2S_DOUT;
    std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv   = false;

    err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (err != ESP_OK) {
        Log::error("[AE] i2s_channel_init_std_mode fail: %d", err);
        i2s_del_channel(tx_handle);
        return false;
    }

    // Note: do NOT call i2s_channel_enable here — I2S clocks must NOT start
    // until after the ES8388 codec is configured (codec needs MCLK from I2S).
    // The channel will be enabled in begin() AFTER initCodec() completes.

    s_i2sTxHandle = (void*)tx_handle;
    Log::info("[AudioEngine] I2S configured (not yet enabled)");
    return true;
}

// ---------------------------------------------------------------------------
// Sample generation — fills stereoFrames interleaved L/R int16 pairs
// ---------------------------------------------------------------------------
void AudioEngine::fillBuffer(int16_t* out, size_t stereoFrames) {
    static uint32_t count = 0;

    // Keyer takes priority if any memory flag is set (iambic B paddle active)
    // Reference: extern/main.c:441 — if current_element == NONE and memory[DIT/DAH] set → play
    bool keyerHasMemory =
        atomic_load(&s_keyState.memory[DIT_IDX]) ||
        atomic_load(&s_keyState.memory[DAH_IDX]);

    // Route based on keyer type
    KeyerType kt = MorseModel::instance().keyerType();

    if (kt == KeyerType::STRAIGHT && s_straightKeyer && !(s_morseGen && s_morseGen->isPlaying())) {
        // Straight key path — always called to keep sample counter advancing
        static int16_t mono[AUDIO_DMA_BUF_LEN];
        s_straightKeyer->fillSamples(mono, stereoFrames,
                                      s_toneFrequency, s_cachedAmplitude,
                                      AUDIO_SAMPLE_RATE);
        for (size_t i = 0; i < stereoFrames; ++i) {
            out[i * 2 + 0] = mono[i];
            out[i * 2 + 1] = mono[i];
        }
    } else if (kt != KeyerType::STRAIGHT && s_keyer && (keyerHasMemory || s_keyer->isActive())) {
        // Iambic B keyer path — takes priority over MorseGenerator
        static int dbg = 0;
        if (++dbg % 100 == 0) {
            Serial.printf("[AE] keyer path: hasMemory=%d isActive=%d\n", keyerHasMemory, s_keyer->isActive());
        }
        static int16_t mono[AUDIO_DMA_BUF_LEN];
        s_keyer->fillSamples(mono, stereoFrames,
                              s_toneFrequency, s_cachedAmplitude,
                              AUDIO_SAMPLE_RATE);
        for (size_t i = 0; i < stereoFrames; ++i) {
            out[i * 2 + 0] = mono[i];
            out[i * 2 + 1] = mono[i];
        }
    } else if (s_morseGen && s_morseGen->isPlaying()) {
        // MorseGenerator path — update encoder character for display
        static int dbg = 0;
        if (++dbg % 100 == 0) {
            Serial.printf("[AE] morseGen playing: char=%c\n", s_morseGen->currentChar());
        }
        MorseModel::instance().setEncoderChar(s_morseGen->currentChar());
        static int16_t mono[AUDIO_DMA_BUF_LEN];
        s_morseGen->fillSamplesMono(mono, stereoFrames,
                                    s_toneFrequency, s_cachedAmplitude);
        for (size_t i = 0; i < stereoFrames; ++i) {
            out[i * 2 + 0] = mono[i]; // L
            out[i * 2 + 1] = mono[i]; // R
        }
    } else {
        // Clear encoder char when not playing
        MorseModel::instance().setEncoderChar(0);
        // Raw sidetone (direct paddle control)
        const float inc    = s_phaseIncrement;
        const float twoPi  = 2.0f * (float)M_PI;
        float       phase  = s_phase;

        for (size_t i = 0; i < stereoFrames; ++i) {
            int16_t s = 0;
            if (s_toneActive) {
                s = (int16_t)(fastSinRadians(phase) * s_cachedAmplitude);
                phase += inc;
                if (phase >= twoPi) phase -= twoPi;
            }
            out[i * 2 + 0] = s; // L
            out[i * 2 + 1] = s; // R
        }
        s_phase = phase;
    }
#ifdef AUDIO_KEEP_WARM
    // keep the audio on — prevents amp shutdown clicks
    for (size_t i = 0; i < 2 * stereoFrames; ++i) {
        if (out[i] == 0) out[i] = 1;
    }
#endif

    // Track pattern percentage for display status bar.
    if (s_keyer) {
        static int s_ditCount = 0;
        static int s_dahCount = 0;
        static size_t s_lastRbHead = 0;

        size_t rbHead = s_keyer->rbHead();

        while (rbHead != s_lastRbHead) {
            size_t idx = s_lastRbHead % IambicKeyer::RB_SIZE;
            char c = s_keyer->rbAt(idx);
            if (c == MorseConstants::DIT_SYMBOL) ++s_ditCount;
            else if (c == MorseConstants::DAH_SYMBOL) ++s_dahCount;
            s_lastRbHead = (s_lastRbHead + 1) % IambicKeyer::RB_SIZE;
        }

        if (s_ditCount + s_dahCount >= 10) {
            int pct = (s_ditCount * 100) / (s_ditCount + s_dahCount);
            MorseModel::instance().setKeyerPatternPercent(pct);
            s_ditCount = s_dahCount = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Audio task — runs on Core 1, priority 22
// ---------------------------------------------------------------------------
void AudioEngine::audioTask(void*) {
    // Buffer: AUDIO_DMA_BUF_LEN stereo frames * 2 channels * 2 bytes = 256 B
    static int16_t buf[AUDIO_DMA_BUF_LEN * 2];
    i2s_chan_handle_t tx = (i2s_chan_handle_t)s_i2sTxHandle;

    Log::info("[AudioEngine] task started on Core 1");
    uint32_t iter = 0;
    while (true) {
        fillBuffer(buf, AUDIO_DMA_BUF_LEN);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool AudioEngine::begin() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 4),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    fastMathInit();
    s_phaseIncrement =
        2.0f * (float)M_PI * s_toneFrequency / (float)AUDIO_SAMPLE_RATE;
    s_phase      = 0.0f;
    s_toneActive = false;
    // Pre-compute amplitude cache from the default volume so the audio task
    // never calls transcendental functions in the real-time path.
    setVolumePercent(s_volumePercent);

#ifdef BOARD_TAB5
    if (!initI2S())    return false;
    if (!initCodec()) { i2s_del_channel((i2s_chan_handle_t)s_i2sTxHandle); return false; }
    // Sync the codec DAC attenuators to the current volume setting.
    applyCodecVolume();
#elif defined(BOARD_CARDPUTER)
    if (!initI2S()) return false;
    // NS4168 is a direct I2S power amp — no codec driver needed.
    // But ES8311 at I2C 0x18 needs the power-up sequence on cold boot.
    {
        constexpr uint8_t es8311_i2c_addr = 0x18;
        // Matches M5Unified _speaker_enabled_cb_cardputer_adv bulk_data
        constexpr uint8_t bulk_data[] = {
            2, 0x00, 0x80,  // RESET / CSM POWER ON
            2, 0x01, 0xB5,  // CLOCK_MANAGER / MCLK=BCLK
            2, 0x02, 0x18,  // CLOCK_MANAGER / MULT_PRE=3
            2, 0x0D, 0x01,  // SYSTEM / Power up analog circuitry
            2, 0x12, 0x00,  // SYSTEM / power-up DAC
            2, 0x13, 0x10,  // SYSTEM / Enable output to HP drive
            2, 0x32, 0xBF,  // DAC / DAC volume ±0 dB
            2, 0x37, 0x08,  // DAC / Bypass DAC equalizer
            0               // end marker
        };
        for (size_t idx = 0; bulk_data[idx] != 0; ) {
            uint8_t len = bulk_data[idx++];
            uint8_t reg = bulk_data[idx++];
            M5.In_I2C.writeRegister(es8311_i2c_addr, reg, &bulk_data[idx], len - 1, 400000);
            idx += len - 1;
        }
        Log::info("[AudioEngine] ES8311 codec init OK");
    }
#endif

    // Now that the ES8388 is configured, start I2S clocks and DMA
    i2s_chan_handle_t tx = (i2s_chan_handle_t)s_i2sTxHandle;
    esp_err_t err = i2s_channel_enable(tx);
    if (err != ESP_OK) {
        Log::error("[AE] i2s_channel_enable fail: %d", err);
        i2s_del_channel(tx);
        return false;
    }
    ampEnable(true);

    // Iambic keyer has its own KeyEnvelop (independent from MorseGenerator's)
    static KeyEnvelop keyerEnv(20, 0.005f, AUDIO_SAMPLE_RATE);  // 20 WPM, 5ms ramp
    s_keyer = new IambicKeyer();
    s_keyer->begin(&keyerEnv);
    s_keyer->setWPM(20);
    Log::info("[AudioEngine] keyer created (WPM=%d)", s_volumePercent);

    // Straight keyer has its own KeyEnvelop
    static KeyEnvelop straightEnv(20, 0.005f, AUDIO_SAMPLE_RATE);
    s_straightKeyer = new StraightKeyer();
    s_straightKeyer->begin();
    s_straightKeyer->setWPM(20);
    Log::info("[AudioEngine] straight keyer created (WPM=20)");

    BaseType_t rc = xTaskCreatePinnedToCore(
        audioTask, "audio", 4096, nullptr, 22,
        &s_audioTaskHandle, /*core=*/1);
    if (rc != pdPASS) {
        Log::error("[AE] task create failed");
        i2s_del_channel(tx);
        delete s_keyer;
        s_keyer = nullptr;
        return false;
    }
    // Let the audio task run a few iterations so buffers fill
    delay(50);
    Log::info("[AudioEngine] begin OK: toneActive=%d phase=%.2f",
              s_toneActive, s_phase);
    return true;
}

void AudioEngine::end() {
    if (s_audioTaskHandle) {
        vTaskDelete(s_audioTaskHandle);
        s_audioTaskHandle = nullptr;
    }
    if (s_i2sTxHandle) {
        i2s_channel_disable((i2s_chan_handle_t)s_i2sTxHandle);
        i2s_del_channel((i2s_chan_handle_t)s_i2sTxHandle);
        s_i2sTxHandle = nullptr;
    }
    ampEnable(false);
    delete s_keyer;
    s_keyer = nullptr;
    delete s_straightKeyer;
    s_straightKeyer = nullptr;
}

void AudioEngine::setToneActive(bool active) {
    s_toneActive = active;
}

// ---------------------------------------------------------------------------
// Headphone detection via PI4IOE5V6408 pin 7  (Tab5 only — Cardputer has no HP jack)
// ---------------------------------------------------------------------------
#ifdef BOARD_TAB5
bool AudioEngine::isHeadphoneInserted() {
    // HP_DET on PI4IOE5V6408 (I2C 0x43 = expander idx 0), pin 7
    return M5.getIOExpander(0).digitalRead(PI4IO_HP_DET_PIN);
}
#else
// Cardputer: no headphone jack
bool AudioEngine::isHeadphoneInserted() { return false; }
#endif

// ---------------------------------------------------------------------------
// Headphone / speaker mode switching  (Tab5 only)
// ---------------------------------------------------------------------------
#ifdef BOARD_TAB5
void AudioEngine::setHeadphoneMode(bool hp) {
    // EXT5V_EN (bit 5) stays ON in both modes — it powers the rail.
    // PI4IO_AMP_BIT (bit 1) controls the speaker power amplifier.
    // In headphone mode we turn the speaker amp OFF so only the headphone
    // output (LOUT2/ROUT2 via ES8388) drives audio.
    uint8_t cur = 0x00;
    M5.In_I2C.readRegister(PI4IO_I2C_ADDR, PI4IO_REG_OUTPUT, &cur, 1, 400000);
    uint8_t next;
    if (hp) {
        next = cur & ~PI4IO_AMP_BIT;  // mute speaker amp
    } else {
        next = cur | PI4IO_AMP_BIT;   // unmute speaker amp
        next |= (1u << 5);            // ensure EXT5V_EN also set
    }
    pi4ioWrite(PI4IO_REG_OUTPUT, next);
    Log::info("[AudioEngine] %s mode (PI4IO reg 0x05: 0x%02X -> 0x%02X)",
             hp ? "Headphone" : "Speaker", cur, next);
}
#else
// Cardputer: no headphone jack, speaker is always on
void AudioEngine::setHeadphoneMode(bool hp) { (void)hp; }
#endif

// ---------------------------------------------------------------------------
// MorseGenerator lifecycle
// ---------------------------------------------------------------------------
void AudioEngine::createMorseGen() {
    if (s_morseGen) return;  // already exists
    static KeyEnvelop env(20, 0.005f, AUDIO_SAMPLE_RATE);  // 20 WPM, 5ms ramp
    s_morseGen = new MorseGenerator(&env, 20);
}

void AudioEngine::deleteMorseGen() {
    delete s_morseGen;
    s_morseGen = nullptr;
}
