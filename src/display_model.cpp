#include "display_model.h"
#include "Log.h"
#include "text_input.h"
#ifndef UNIT_TEST
#include "audio_engine.h"
#include "radio_keyer.h"
#include <Arduino.h>
#else
#include "../test/mocks/arduino_mock.h"
#endif
#include <cstring>

MorseModel& MorseModel::instance() {
    static MorseModel inst;
    return inst;
}

DisplayScreen MorseModel::screen() const {
    return _screen.load(std::memory_order_relaxed);
}

void MorseModel::setScreen(DisplayScreen s) {
    _screen.store(s, std::memory_order_relaxed);
    incrementChangeCounter();
}

KeyerMode MorseModel::mode() const {
    return _mode.load(std::memory_order_relaxed);
}

void MorseModel::setMode(KeyerMode m) {
    _mode.store(m, std::memory_order_relaxed);
    incrementChangeCounter();
}

KeyerType MorseModel::keyerType() const {
    return _keyerType.load(std::memory_order_relaxed);
}

void MorseModel::setKeyerType(KeyerType t) {
    _keyerType.store(t, std::memory_order_relaxed);
    incrementChangeCounter();
}

bool MorseModel::radioKeyingEnabled() const {
    return _radioKeyingEnabled.load(std::memory_order_relaxed);
}

void MorseModel::setRadioKeyingEnabled(bool enabled) {
    bool prev = _radioKeyingEnabled.exchange(enabled, std::memory_order_relaxed);
    if (prev == enabled) return;
#ifndef UNIT_TEST
    // Delegate physical side-effects to RadioKeyer so GPIO policy stays
    // in one place. On disable this forces the line LOW even if a
    // keyer is mid-element.
    RadioKeyer::setEnabled(enabled);
#endif
    incrementChangeCounter();
}

bool MorseModel::winkeyMode() const {
    return _winkeyMode.load(std::memory_order_relaxed);
}

void MorseModel::setWinkeyMode(bool on) {
    bool prev = _winkeyMode.exchange(on, std::memory_order_relaxed);
    if (prev == on) return;
    incrementChangeCounter();
}

const char* MorseModel::decodedText() const {
    return _textBuf;
}

size_t MorseModel::decodedTextLen() const {
    return _textLen.load(std::memory_order_relaxed);
}

void MorseModel::appendDecodedChar(char c) {
    appendDecodedChar(c, false);
}

void MorseModel::appendDecodedChar(char c, bool fromPlayer) {
    uint32_t now = millis();
    size_t head = _textHead.load(std::memory_order_relaxed);
    _textBuf[head] = c;
    _textAttr[head] = fromPlayer ? ATTR_PLAYER : ATTR_KEYER;
    size_t newHead = (head + 1) % TEXT_BUF_SIZE;
    _textHead.store(newHead, std::memory_order_relaxed);

    size_t len = _textLen.load(std::memory_order_relaxed);
    if (len < TEXT_BUF_SIZE) {
        _textLen.store(len + 1, std::memory_order_relaxed);
    } else {
        // Buffer full — advance tail (oldest character drops out)
        _textTail.store((_textTail.load(std::memory_order_relaxed) + 1) % TEXT_BUF_SIZE,
                        std::memory_order_relaxed);
    }
    if (fromPlayer) {
        _lastCharFromPlayer.store(true, std::memory_order_relaxed);
        _playerTail.store(head, std::memory_order_relaxed);
        size_t prevHead = _playerHead.load(std::memory_order_relaxed);
        if (prevHead == SIZE_MAX) {
            _playerHead.store(head, std::memory_order_relaxed);
        }
    } else {
        _lastCharFromPlayer.store(false, std::memory_order_relaxed);
        _playerHead.store(SIZE_MAX, std::memory_order_relaxed);
    }
    uint32_t newCounter = _changeCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    Log::debug("[DM] APPEND char='%c' fromPlayer=%d counter=%u->%u head=%zu len=%zu",
        c, (int)fromPlayer, newCounter - 1, newCounter,
        (size_t)newHead, (size_t)(len < TEXT_BUF_SIZE ? len + 1 : TEXT_BUF_SIZE));
}

void MorseModel::clearDecodedText() {
    _textHead.store(0, std::memory_order_relaxed);
    _textTail.store(0, std::memory_order_relaxed);
    _textLen.store(0, std::memory_order_relaxed);
    _playerHead.store(SIZE_MAX, std::memory_order_relaxed);  // reset player tracking
    memset(_textBuf, 0, TEXT_BUF_SIZE);
    memset(_textAttr, 0, TEXT_BUF_SIZE);
    incrementChangeCounter();
}

int MorseModel::wpm() const {
    return _wpm.load(std::memory_order_relaxed);
}

void MorseModel::setWPM(int wpm) {
    if (wpm < 5) wpm = 5;
    if (wpm > 50) wpm = 50;
    int prev = _wpm.load(std::memory_order_relaxed);
    _wpm.store(wpm, std::memory_order_relaxed);
#ifndef UNIT_TEST
    // Propagate to audio engine
    if (auto keyer = AudioEngine::keyer()) keyer->setWPM(wpm);
    if (auto sk = AudioEngine::straightKeyer()) sk->setWPM(wpm);
    if (auto gen = AudioEngine::morseGen()) gen->setWPM(wpm);
    static int s_setWPMCallNo = 0;
    int callNo = ++s_setWPMCallNo;
    if (callNo <= 5) {
        Log::info("[MM-DIAG] setWPM#%d: %d -> %d (propagated to keyer/gen)",
            callNo, prev, wpm);
    }
#endif
    incrementChangeCounter();
}

void MorseModel::adjustWPM(int delta) {
    int cur = _wpm.load(std::memory_order_relaxed);
    setWPM(cur + delta);
}

float MorseModel::frequency() const {
    return _frequency.load(std::memory_order_relaxed);
}

void MorseModel::setFrequency(float hz) {
    if (hz < 300.0f) hz = 300.0f;
    if (hz > 900.0f) hz = 900.0f;
    _frequency.store(hz, std::memory_order_relaxed);
#ifndef UNIT_TEST
    AudioEngine::setToneFrequency(hz);
#endif
    incrementChangeCounter();
}

void MorseModel::adjustFrequency(float delta) {
    float cur = _frequency.load(std::memory_order_relaxed);
    setFrequency(cur + delta);
}

int MorseModel::volume() const {
    return _volume.load(std::memory_order_relaxed);
}

void MorseModel::setVolume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    _volume.store(vol, std::memory_order_relaxed);
#ifndef UNIT_TEST
    AudioEngine::setVolumePercent(vol);
#endif
    incrementChangeCounter();
}

void MorseModel::adjustVolume(int delta) {
    int cur = _volume.load(std::memory_order_relaxed);
    setVolume(cur + delta);
}

int MorseModel::keyerPatternPercent() const {
    return _keyerPct.load(std::memory_order_relaxed);
}

void MorseModel::setKeyerPatternPercent(int pct) {
    _keyerPct.store(pct, std::memory_order_relaxed);
    incrementChangeCounter();
}

uint32_t MorseModel::changeCounter() const {
    return _changeCounter.load(std::memory_order_relaxed);
}

void MorseModel::incrementChangeCounter() {
    _changeCounter.fetch_add(1, std::memory_order_relaxed);
}

uint32_t MorseModel::overlayStartMillis() const {
    return _overlayStart.load(std::memory_order_relaxed);
}

void MorseModel::setOverlayStartMillis(uint32_t ms) {
    _overlayStart.store(ms, std::memory_order_relaxed);
}

bool MorseModel::isDisplayActive() const {
    return _displayActive.load(std::memory_order_relaxed);
}

void MorseModel::setDisplayActive(bool on) {
    _displayActive.store(on, std::memory_order_relaxed);
    incrementChangeCounter();
}

uint32_t MorseModel::lastActivity() const {
    return _lastActivity.load(std::memory_order_relaxed);
}

void MorseModel::touch() {
    _lastActivity.store(millis(), std::memory_order_relaxed);
}

size_t MorseModel::textTail() const {
    return _textTail.load(std::memory_order_relaxed);
}

size_t MorseModel::textHead() const {
    return _textHead.load(std::memory_order_relaxed);
}

size_t MorseModel::textLen() const {
    return _textLen.load(std::memory_order_relaxed);
}

char MorseModel::textAt(size_t idx) const {
    if (idx >= TEXT_BUF_SIZE) return '\0';
    return _textBuf[idx];
}

char MorseModel::attrAt(size_t idx) const {
    if (idx >= TEXT_BUF_SIZE) return '\0';
    return _textAttr[idx];
}

char MorseModel::encoderChar() const {
    return _encoderChar.load(std::memory_order_relaxed);
}

void MorseModel::setEncoderChar(char c) {
    _encoderChar.store(c, std::memory_order_relaxed);
}

bool MorseModel::lastCharFromPlayer() const {
    return _lastCharFromPlayer.load(std::memory_order_relaxed);
}

void MorseModel::setLastCharFromPlayer(bool v) {
    _lastCharFromPlayer.store(v, std::memory_order_relaxed);
}

void MorseModel::resetPlayerHead() {
    _playerHead.store(SIZE_MAX, std::memory_order_relaxed);
}

// ─── Wi-Fi UI mirrors ────────────────────────────────────────────────────────

int MorseModel::wifiState() const { return _wifiState.load(std::memory_order_relaxed); }
void MorseModel::setWifiState(int s) {
    if (_wifiState.load(std::memory_order_relaxed) == s) return;
    _wifiState.store(s, std::memory_order_relaxed);
    incrementChangeCounter();
}

uint32_t MorseModel::wifiLocalIP() const { return _wifiLocalIP.load(std::memory_order_relaxed); }
void MorseModel::setWifiLocalIP(uint32_t ip) {
    if (_wifiLocalIP.load(std::memory_order_relaxed) == ip) return;
    _wifiLocalIP.store(ip, std::memory_order_relaxed);
    incrementChangeCounter();
}

bool MorseModel::wifiHasCredentials() const { return _wifiHasCredentials.load(std::memory_order_relaxed); }
void MorseModel::setWifiHasCredentials(bool v) {
    if (_wifiHasCredentials.load(std::memory_order_relaxed) == v) return;
    _wifiHasCredentials.store(v, std::memory_order_relaxed);
    incrementChangeCounter();
}

int MorseModel::wifiCredSource() const { return _wifiCredSource.load(std::memory_order_relaxed); }
void MorseModel::setWifiCredSource(int s) {
    if (_wifiCredSource.load(std::memory_order_relaxed) == s) return;
    _wifiCredSource.store(s, std::memory_order_relaxed);
    incrementChangeCounter();
}

uint32_t MorseModel::wifiSecondsUntilRetry() const {
    return _wifiSecondsUntilRetry.load(std::memory_order_relaxed);
}
void MorseModel::setWifiSecondsUntilRetry(uint32_t s) {
    _wifiSecondsUntilRetry.store(s, std::memory_order_relaxed);
}

int MorseModel::wifiScanCount() const { return _wifiScanCount.load(std::memory_order_relaxed); }
void MorseModel::setWifiScanCount(int n) {
    if (n < 0) n = 0;
    if (_wifiScanCount.load(std::memory_order_relaxed) == n) return;
    _wifiScanCount.store(n, std::memory_order_relaxed);
    incrementChangeCounter();
}

int MorseModel::wifiScanCursor() const { return _wifiScanCursor.load(std::memory_order_relaxed); }
void MorseModel::setWifiScanCursor(int idx) {
    if (_wifiScanCursor.load(std::memory_order_relaxed) == idx) return;
    _wifiScanCursor.store(idx, std::memory_order_relaxed);
    incrementChangeCounter();
}

int MorseModel::wifiScanTop() const { return _wifiScanTop.load(std::memory_order_relaxed); }
void MorseModel::setWifiScanTop(int top) {
    if (_wifiScanTop.load(std::memory_order_relaxed) == top) return;
    _wifiScanTop.store(top, std::memory_order_relaxed);
    incrementChangeCounter();
}

void MorseModel::wifiAdjustScanCursor(int delta) {
    const int total = _wifiScanCount.load(std::memory_order_relaxed);
    if (total <= 0) {
        setWifiScanCursor(0);
        setWifiScanTop(0);
        return;
    }
    int cur  = _wifiScanCursor.load(std::memory_order_relaxed);
    int top  = _wifiScanTop.load(std::memory_order_relaxed);
    int next = cur + delta;
    if (next < 0) next = 0;
    if (next >= total) next = total - 1;

    constexpr int kPage = 4;   // MUST match WifiMgr::kPageSize in network_manager.h
    if (next < top) top = next;
    if (next >= top + kPage) top = next - (kPage - 1);
    if (top < 0) top = 0;

    setWifiScanCursor(next);
    setWifiScanTop(top);
}

TextInput* MorseModel::passwordInput() {
    if (!_passwordInput) {
        _passwordInput = new TextInput(_passwordBuf, sizeof(_passwordBuf), '*');
    }
    return _passwordInput;
}

void MorseModel::wifiClearPassword() {
    if (_passwordInput) _passwordInput->clear();
    else _passwordBuf[0] = '\0';
}

void MorseModel::wifiResetUIState() {
    setWifiScanCursor(0);
    setWifiScanTop(0);
    wifiClearPassword();
}