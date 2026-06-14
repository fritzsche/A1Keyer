#pragma once
// Minimal Arduino mock for host-run unit tests.
// Included by source files compiled under UNIT_TEST (via test/mocks include path).
// No guard needed — this file is only in the test/mocks directory.

// Provide millis, delay, Serial etc. from the Arduino.h stub in test/mocks
#include "Arduino.h"