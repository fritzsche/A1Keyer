/** morse_key.cpp - Iambic paddle key input for M5Stack Morse Trainer
 *
 * GPIO interrupts provide sub-millisecond key detection latency.
 * No software debouncing — physical contact quality handles bounce;
 * downstream keyer state machine filters artefacts.
 *
 * Communication with IambicKeyer (audio thread) via std::atomic s_keyState:
 *   - ISR sets memory[DIT/DAH] = SET on rising edge (paddle press)
 *   - ISR sets state[DIT/DAH] = SET/UNSET on every edge
 *   - Audio task reads state and clears memory when element ends
 *
 * References:
 *   - extern/morse.h:42-48 — key_state_type struct
 *   - extern/main.c:379 — rising edge: state[dit] == SET && current_element == KEY_UP
 *   - extern/main.c:387 — falling edge: both state[] == UNSET && current_element == KEY_DOWN
 *   - extern/main.c:441 — paddle_key_callback idle check
 */
#include "morse_key.h"
#include "Log.h"
#include "display_task.h"

// s_keyState is defined in iambic_keyer.cpp
extern KeyState s_keyState;

void IRAM_ATTR MorseKey::isrDit(void*) {
    bool dit = !(gpio_get_level(PIN_KEY_DIT));  // active LOW → true=pressed

    // Detect rising edge using atomic exchange
    int prev = s_keyState.state[DIT_IDX].exchange(dit ? MEMORY_SET : MEMORY_UNSET, std::memory_order_relaxed);
    if (dit && !prev) {
        // Rising edge: DIT paddle just pressed → set element memory
        s_keyState.memory[DIT_IDX].store(MEMORY_SET, std::memory_order_relaxed);
        DisplayTask::wakeFromScreensaver();
    } else if (!dit && prev) {
        // Falling edge: DIT paddle released
        Log::debug("[MorseKey] DIT released");
    }
    // Memory is NOT cleared on release — audio thread clears it when element ends
}

void IRAM_ATTR MorseKey::isrDah(void*) {
    bool dah = !(gpio_get_level(PIN_KEY_DAH));  // active LOW → true=pressed
    int prev = s_keyState.state[DAH_IDX].exchange(dah ? MEMORY_SET : MEMORY_UNSET, std::memory_order_relaxed);
    if (dah && !prev) {
        // Rising edge: DAH paddle just pressed → set element memory
        s_keyState.memory[DAH_IDX].store(MEMORY_SET, std::memory_order_relaxed);
        // Wake display from screen-saver if active
        DisplayTask::wakeFromScreensaver();
    }
}

void MorseKey::begin() {
    gpio_install_isr_service(0);

    // Initialise atomic state to UNSET
    s_keyState.memory[DIT_IDX].store(MEMORY_UNSET, std::memory_order_relaxed);
    s_keyState.memory[DAH_IDX].store(MEMORY_UNSET, std::memory_order_relaxed);
    s_keyState.state[DIT_IDX].store(MEMORY_UNSET, std::memory_order_relaxed);
    s_keyState.state[DAH_IDX].store(MEMORY_UNSET, std::memory_order_relaxed);

    // DIT — white wire on Grove pin 1
    gpio_set_direction(PIN_KEY_DIT, GPIO_MODE_INPUT);
    gpio_pullup_en(PIN_KEY_DIT);
    gpio_set_intr_type(PIN_KEY_DIT, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_KEY_DIT, isrDit, nullptr);

    // DAH — yellow wire on Grove pin 2
    gpio_set_direction(PIN_KEY_DAH, GPIO_MODE_INPUT);
    gpio_pullup_en(PIN_KEY_DAH);
    gpio_set_intr_type(PIN_KEY_DAH, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_KEY_DAH, isrDah, nullptr);

    // Sample initial state
    bool dit = !(gpio_get_level(PIN_KEY_DIT));
    bool dah = !(gpio_get_level(PIN_KEY_DAH));
    s_keyState.state[DIT_IDX].store(dit ? MEMORY_SET : MEMORY_UNSET, std::memory_order_relaxed);
    s_keyState.state[DAH_IDX].store(dah ? MEMORY_SET : MEMORY_UNSET, std::memory_order_relaxed);

    Log::info("[MorseKey] DIT=GPIO%d DAH=GPIO%d  dit=%d dah=%d",
              (int)PIN_KEY_DIT, (int)PIN_KEY_DAH, dit, dah);
}

bool MorseKey::isDitPressed() {
    return std::atomic_load(&s_keyState.state[DIT_IDX]) == MEMORY_SET;
}

bool MorseKey::isDahPressed() {
    return std::atomic_load(&s_keyState.state[DAH_IDX]) == MEMORY_SET;
}

void MorseKey::clearMemory() {
    std::atomic_store(&s_keyState.memory[DIT_IDX], MEMORY_UNSET);
    std::atomic_store(&s_keyState.memory[DAH_IDX], MEMORY_UNSET);
    std::atomic_store(&s_keyState.state[DIT_IDX], MEMORY_UNSET);
    std::atomic_store(&s_keyState.state[DAH_IDX], MEMORY_UNSET);
}
