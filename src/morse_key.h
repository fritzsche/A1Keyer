#pragma once
/**
 * morse_key.h
 *
 * Iambic paddle key input for M5Stack Morse Trainer.
 *
 * Uses GPIO interrupts for minimum latency key detection (<1 µs).
 * Supports both DIT (dot) and DAH (dash) paddles independently.
 * No software debouncing — rely on the physical quality of your contacts;
 * bounce artefacts are handled downstream by the keyer state machine.
 *
 * Pin assignments:
 *   Board          DIT paddle       DAH paddle    Notes
 *   Cardputer ADV  GPIO1 (white)    GPIO2 (yellow)  Grove connector, active LOW, internal pull-up
 *   Tab5           GPIO_NUM_1       GPIO_NUM_2      Expansion connector, active LOW, internal pull-up
 *
 * Communication with IambicKeyer (audio thread) via std::atomic:
 *   - s_keyState.memory[DIT/DAH]: ISR sets SET on rising edge; audio clears
 *   - s_keyState.state[DIT/DAH]: ISR sets SET/UNSET on every edge
 */

#include "iambic_keyer.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
// Per-device pin assignments — adjust for your hardware wiring
// ---------------------------------------------------------------------------
#if defined(BOARD_CARDPUTER)
// Cardputer ADV: Grove connector (Port.A)
// Pin 1 = white = GPIO1 (SCL), Pin 2 = yellow = GPIO2 (SDA), Pin 3 = GND
static constexpr gpio_num_t PIN_KEY_DIT = GPIO_NUM_1;  ///< Dit (dot)  paddle — Grove white
static constexpr gpio_num_t PIN_KEY_DAH = GPIO_NUM_2;  ///< Dah (dash) paddle — Grove yellow

#elif defined(BOARD_TAB5)
// Tab5: expansion connector
static constexpr gpio_num_t PIN_KEY_DIT = GPIO_NUM_1;  ///< Dit (dot)  paddle
static constexpr gpio_num_t PIN_KEY_DAH = GPIO_NUM_2;  ///< Dah (dash) paddle

#else
#error "BOARD_TAB5 or BOARD_CARDPUTER must be defined"
#endif

// ---------------------------------------------------------------------------
// MorseKey
// ---------------------------------------------------------------------------
class MorseKey {
public:
    /**
     * Initialise GPIO pins with interrupts.
     * Must be called from setup().
     */
    static void begin();

    /** Returns true while the DIT paddle is pressed. */
    static bool isDitPressed();

    /** Returns true while the DAH paddle is pressed. */
    static bool isDahPressed();

    /** Clear all key memory and state (used on keyer-type switch). */
    static void clearMemory();

private:
    static void IRAM_ATTR isrDit(void* arg);
    static void IRAM_ATTR isrDah(void* arg);
};