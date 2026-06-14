cmake_minimum_required(VERSION 3.14)
# Test-only CMake build — the ESP32 firmware is built via PlatformIO (platformio.ini).
#
# Usage (from the repo root):
#   cmake -B build
#   cmake --build build
#   ctest --test-dir build -V
project(A1Keyer_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Platform-independent library sources (no Arduino / ESP-IDF headers)
# ---------------------------------------------------------------------------
set(LIB_SOURCES
    src/fast_math.cpp
    src/morse_encoder.cpp
    src/key_envelop.cpp
    src/morse_generator.cpp
    src/iambic_keyer.cpp
    src/straight_keyer.cpp
    src/morse_decoder.cpp
    src/display_model.cpp
    src/MorseTable.cpp
)

# ---------------------------------------------------------------------------
# Test suites
# ---------------------------------------------------------------------------
enable_testing()

foreach(SUITE IN ITEMS
    test_morse_encoder
    test_key_envelop
    test_morse_generator
    test_iambic_keyer
    test_morse_decoder
    test_display_model
)
    add_executable(${SUITE}
        test/${SUITE}/${SUITE}.cpp
        ${LIB_SOURCES}
    )
    target_include_directories(${SUITE} PRIVATE
        src           # library headers
        test/mocks    # Arduino.h stub (satisfies #include <Arduino.h> in morse_encoder.cpp)
        test          # test_framework.h
    )
    target_compile_definitions(${SUITE} PRIVATE UNIT_TEST)

    # M_PI on MSVC / MinGW requires this define
    if(WIN32)
        target_compile_definitions(${SUITE} PRIVATE _USE_MATH_DEFINES)
    endif()

    # libm is a separate link target on Linux
    if(UNIX AND NOT APPLE)
        target_link_libraries(${SUITE} PRIVATE m)
    endif()

    add_test(NAME ${SUITE} COMMAND ${SUITE})
endforeach()
