#include "display_model.h"
#include "Log.h"
#include "text_input.h"
#include "morse_encoder.h"
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

bool MorseModel::polarityReversed() const {
    return _polarityReversed.load(std::memory_order_relaxed);
}

void MorseModel::setPolarityReversed(bool reversed) {
    bool prev = _polarityReversed.exchange(reversed, std::memory_order_relaxed);
    if (prev == reversed) return;
#ifndef UNIT_TEST
    // Only the iambic keyer is polarity-aware; the straight key has no dit/dah
    // levers to swap and is deliberately left alone.
    if (auto keyer = AudioEngine::keyer()) keyer->setReversed(reversed);
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

MorseTableMode MorseModel::morseTableMode() const {
    return _morseTableMode.load(std::memory_order_relaxed);
}

void MorseModel::setMorseTableMode(MorseTableMode mode) {
    MorseTableMode prev = _morseTableMode.exchange(mode, std::memory_order_relaxed);
    if (prev == mode) return;
    const MorseTable* table = &kInternationalMorseTable;
    if (mode == MorseTableMode::WABUN_KATAKANA || mode == MorseTableMode::WABUN_HIRAGANA) {
        table = &kWabunMorseTable;
    }
    MorseEncoder::setTable(table);
    // Clear the decoded text buffer so old International-mode characters
    // are not misinterpreted as multi-byte UTF-8 sequences.
    clearDecodedText();
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
        // TX-buffer sync: when a player char lands in the decoded text
        // during an active TX session, advance _txSent by 1 and update
        // _txHead to the char just keyed (the caret visual). This is
        // what eventually flips txActive=false at session completion.
        // The atomic incrementChangeCounter at the bottom already
        // notifies the web UI via /state polling.
        if (_txActive.load(std::memory_order_relaxed)) {
            _txSent.fetch_add(1, std::memory_order_relaxed);
            _txHead.fetch_add(1, std::memory_order_relaxed);
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

int MorseModel::wifiMode() const { return _wifiMode.load(std::memory_order_relaxed); }
void MorseModel::setWifiMode(int m) {
    if (_wifiMode.load(std::memory_order_relaxed) == m) return;
    _wifiMode.store(m, std::memory_order_relaxed);
    incrementChangeCounter();
}

int MorseModel::wifiApStations() const { return _wifiApStations.load(std::memory_order_relaxed); }
void MorseModel::setWifiApStations(int n) {
    if (n < 0) n = 0;
    if (_wifiApStations.load(std::memory_order_relaxed) == n) return;
    _wifiApStations.store(n, std::memory_order_relaxed);
    incrementChangeCounter();
}

int MorseModel::wifiApMaxStations() const { return _wifiApMaxStations.load(std::memory_order_relaxed); }
void MorseModel::setWifiApMaxStations(int n) {
    if (n <= 0) n = 1;
    if (_wifiApMaxStations.load(std::memory_order_relaxed) == n) return;
    _wifiApMaxStations.store(n, std::memory_order_relaxed);
}

bool MorseModel::wifiNetConfirmForget() const {
    return _wifiNetConfirmForget.load(std::memory_order_relaxed);
}
void MorseModel::setWifiNetConfirmForget(bool v) {
    bool cur = _wifiNetConfirmForget.load(std::memory_order_relaxed);
    if (cur == v) return;
    _wifiNetConfirmForget.store(v, std::memory_order_relaxed);
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
    setWifiNetConfirmForget(false);
}

// ─── Memory-keyer accessors ──────────────────────────────────────────────────
//
// The TextInput editor is bound to a separate buffer (_memoryEditorBuf),
// NOT to _memory[slot]. This mirrors the wifi password buffer pattern:
// edits live in their own storage and are written to the slot on commit
// (Enter). Pressing ESC discards whatever is in the editor and leaves
// the persisted text untouched. Persistence to NVS happens in main.cpp's
// setup() (load) and on Enter from MEMORY_EDIT (save).

TextInput* MorseModel::memoryInput() {
    if (!_memoryInput) {
        _memoryInput = new TextInput(_memoryEditorBuf, kMemLen, '*');
    }
    return _memoryInput;
}

void MorseModel::memoryClearEditor() {
    if (_memoryInput) _memoryInput->clear();
    else {
        int slot = _memoryEditingSlot.load(std::memory_order_acquire);
        if (slot >= 0 && slot < static_cast<int>(kMemSlots)) {
            _memory[slot][0] = '\0';
        }
    }
}

int MorseModel::memoryEditingSlot() const {
    return _memoryEditingSlot.load(std::memory_order_acquire);
}

void MorseModel::setMemoryEditingSlot(int slot) {
    if (slot < -1 || slot >= static_cast<int>(kMemSlots)) return;
    _memoryEditingSlot.store(slot, std::memory_order_release);
    incrementChangeCounter();
}

int MorseModel::memoryPickSlot() const {
    return _memoryPickSlot.load(std::memory_order_acquire);
}

void MorseModel::setMemoryPickSlot(int slot) {
    if (slot < -1 || slot >= static_cast<int>(kMemSlots)) return;
    _memoryPickSlot.store(slot, std::memory_order_release);
    incrementChangeCounter();
}

const char* MorseModel::getMemory(uint8_t slot) const {
    if (slot >= kMemSlots) return "";
    return _memory[slot];
}

void MorseModel::setMemory(uint8_t slot, const char* text) {
    if (slot >= kMemSlots) return;
    memCopyStr(_memory[slot], kMemLen, text);
    incrementChangeCounter();
}

void MorseModel::copyMemoryBank(const MemoryBank& bank) {
    for (uint8_t i = 0; i < kMemSlots; ++i) {
        memCopyStr(_memory[i], kMemLen, bank.slot[i]);
    }
    incrementChangeCounter();
}

// ─── TX-buffer accessors ─────────────────────────────────────────────────────
//
// Session-only text compose + transmit buffer. Shared between the web UI
// (HTTP /api/tx/*) and the WinKey host interface (ADMIN_TX_BUFFER_LOAD +
// ADMIN_TX_BUFFER_START). Single source of truth lives in _txBuffer; the
// TxBuffer::poll() driver feeds chunks from it into MorseGenerator.
//
// Edit boundary (txEditableStart):
//   - No chunk in flight → txSent (everything not yet keyed is editable).
//   - Chunk in flight   → txChunkStart + txChunkLen (the entire chunk is
//     locked once pushed to the generator; only chars after the chunk
//     are editable). This means: even when the LAST chunk is being keyed,
//     the operator can still append to the pending tail — that scenario
//     becomes a new chunk and plays once the current one drains. Matches
//     the "TX already close, append a small fix" use case from the brief.

namespace {
// Helper: copy `src` into the TX buffer at position 0, truncating at
// kTxBufLen-1 chars. Always NUL-terminates.
size_t copyIntoTxBuf(char* dst, size_t cap, const char* src) {
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    size_t n = 0;
    while (n + 1 < cap && src[n] != '\0') {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = '\0';
    return n;
}
}  // namespace

void MorseModel::setTxText(const char* text) {
    // Full-buffer replace. Resets txSent=0, clears txActive, stops the
    // generator if running. Always NUL-terminates.
    size_t n = copyIntoTxBuf(_txBuffer, kTxBufLen, text);
    _txLen.store(n, std::memory_order_relaxed);
    _txSent.store(0, std::memory_order_relaxed);
    _txHead.store(0, std::memory_order_relaxed);
    _txChunkStart.store(0, std::memory_order_relaxed);
    _txChunkLen.store(0, std::memory_order_relaxed);
#ifndef UNIT_TEST
    // Stop the generator if a session was in progress.
    if (auto gen = AudioEngine::morseGen()) {
        gen->stop();
    }
#endif
    _txActive.store(false, std::memory_order_relaxed);
    incrementChangeCounter();
}

bool MorseModel::appendTxChar(char c) {
    size_t len = _txLen.load(std::memory_order_relaxed);
    if (len + 1 >= kTxBufLen) {
        // Cap-exceeded (need room for the terminator). Silent reject.
        return false;
    }
    _txBuffer[len] = c;
    _txBuffer[len + 1] = '\0';
    _txLen.store(len + 1, std::memory_order_relaxed);
    incrementChangeCounter();
    return true;
}

bool MorseModel::backspaceTx() {
    size_t len = _txLen.load(std::memory_order_relaxed);
    size_t editStart = txEditableStart();
    if (len <= editStart) {
        return false;
    }
    _txBuffer[len - 1] = '\0';
    _txLen.store(len - 1, std::memory_order_relaxed);
    incrementChangeCounter();
    return true;
}

void MorseModel::clearTx() {
    _txBuffer[0] = '\0';
    _txLen.store(0, std::memory_order_relaxed);
    _txSent.store(0, std::memory_order_relaxed);
    _txHead.store(0, std::memory_order_relaxed);
    _txChunkStart.store(0, std::memory_order_relaxed);
    _txChunkLen.store(0, std::memory_order_relaxed);
#ifndef UNIT_TEST
    if (auto gen = AudioEngine::morseGen()) {
        gen->stop();
    }
#endif
    _txActive.store(false, std::memory_order_relaxed);
    incrementChangeCounter();
}

void MorseModel::startTx() {
    // Idempotent. No-op if already active or nothing to send.
    if (_txActive.load(std::memory_order_relaxed)) return;
    size_t len = _txLen.load(std::memory_order_relaxed);
    size_t sent = _txSent.load(std::memory_order_relaxed);
    if (len <= sent) return;
    _txActive.store(true, std::memory_order_relaxed);
    _txHead.store(sent, std::memory_order_relaxed);
    incrementChangeCounter();
}

void MorseModel::stopTx() {
    // Cancel in-flight chunk + clear _txActive. Pending text preserved.
#ifndef UNIT_TEST
    if (auto gen = AudioEngine::morseGen()) {
        gen->stop();
    }
#endif
    _txChunkStart.store(0, std::memory_order_relaxed);
    _txChunkLen.store(0, std::memory_order_relaxed);
    _txActive.store(false, std::memory_order_relaxed);
    _txHead.store(_txSent.load(std::memory_order_relaxed), std::memory_order_relaxed);
    incrementChangeCounter();
}

void MorseModel::bumpTxSentBy(size_t n) {
    if (n == 0) return;
    _txSent.fetch_add(n, std::memory_order_relaxed);
    _txChunkStart.store(_txSent.load(std::memory_order_relaxed), std::memory_order_relaxed);
    _txChunkLen.store(0, std::memory_order_relaxed);
    _txHead.store(_txSent.load(std::memory_order_relaxed), std::memory_order_relaxed);
    incrementChangeCounter();
}

void MorseModel::setTxChunk(size_t start, size_t len) {
    _txChunkStart.store(start, std::memory_order_relaxed);
    _txChunkLen.store(len, std::memory_order_relaxed);
    incrementChangeCounter();
}

void MorseModel::setTxHead(size_t head) {
    _txHead.store(head, std::memory_order_relaxed);
}

void MorseModel::clearTxActive() {
    _txActive.store(false, std::memory_order_relaxed);
    _txChunkStart.store(0, std::memory_order_relaxed);
    _txChunkLen.store(0, std::memory_order_relaxed);
    _txHead.store(_txSent.load(std::memory_order_relaxed), std::memory_order_relaxed);
    incrementChangeCounter();
}