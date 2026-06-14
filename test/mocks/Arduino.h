#pragma once
// Minimal stub so morse_generator.cpp and morse_encoder.cpp compile
// on the native test host without any ESP/Arduino SDK.

#include <cstdio>
#include <cmath>     // M_PI
#include <atomic>
#include <cstdarg>
#include <time.h>

#define HIGH 1
#define LOW  0
#define INPUT 0
// UNIT_TEST is defined via -D cmake flag; do not redefine here

struct gpio_num_t { int num; };
constexpr gpio_num_t GPIO_NUM_1 = {1};
constexpr gpio_num_t GPIO_NUM_2 = {2};

// millis stub
inline uint32_t millis() {
    static uint32_t s_millis = 1000;
    return s_millis++;
}

// esp_timer stub
inline int64_t esp_timer_get_time() {
    static int64_t s_us = 0;
    return s_us += 100;
}

struct _SerialStub {
    void print(const char*) {}
    void print(char) {}
    void println(const char*) {}
    void println() {}
    int printf(const char* fmt, ...) { return 0; }
    template<typename T> void print(T) {}
    int vprintf(const char* fmt, va_list) { return 0; }
};

inline _SerialStub Serial;

// driver/gpio stub
inline int gpio_get_level(gpio_num_t) { return 0; }
inline void gpio_set_direction(gpio_num_t, int) {}
inline void gpio_pullup_en(gpio_num_t) {}
inline void gpio_set_intr_type(gpio_num_t, int) {}
inline void gpio_install_isr_service(int) {}
inline void gpio_isr_handler_add(gpio_num_t, void (*)(void*), void*) {}

// freertos/FreeRTOS.h stub
inline void vTaskDelay(int) {}
inline void portENABLE_INTERRUPTS() {}
inline void portDISABLE_INTERRUPTS() {}

// Atomic alias
using std::atomic;
using std::atomic_load;
using std::atomic_store;
using std::memory_order_relaxed;