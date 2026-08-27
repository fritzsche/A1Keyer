# TX Buffer

A1Keyer's **TX buffer** lets the operator compose a free-form CW
message and transmit it under explicit control. Two entry points share
the same buffer:

- **Web UI** — point a browser at the device's IP, type into the
  **TX buffer** card, click **TX**. Three regions render the live
  progress: *sent* (locked, grey), *in-flight* (locked, accent), and
  *pending* (editable input).
- **WinKey host** — send `ADMIN_TX_BUFFER_LOAD`, stream bytes ≥ 0x20,
  send `ADMIN_TX_BUFFER_START`. The host sees the same buffer the web
  UI does. WK2 status byte bit 4 reports "buffer has unsent chars".

After the buffer drains, the device auto-returns to RX. The buffer is
**session-only** — cleared on reboot. The architecture mirrors the
mental model of fldigi's outgoing-text pane and MMTTY/N1MM's TX
buffer: edit-before-send, immutable-when-sent, auto-RX on completion.

This document is both the development plan (architecture, files,
verification) and the operator reference (entry points, behaviour).
Sections 1–3 are operator-facing; sections 4–9 are developer-facing.

---

## Contents

1. [Overview](#1-overview)
2. [Operator reference](#2-operator-reference)
3. [Storage](#3-storage)
4. [Playback paths](#4-playback-paths)
5. [WinKey protocol additions](#5-winkey-protocol-additions)
6. [Edit boundary rules](#6-edit-boundary-rules)
7. [Concurrency and cross-core notes](#7-concurrency-and-cross-core-notes)
8. [Files and module map](#8-files-and-module-map)
9. [Verification](#9-verification)
10. [Future work](#10-future-work)

---

## 1. Overview

- **Function:** session-only text compose buffer. Operator types or
  streams text, clicks TX (or sends `ADMIN_TX_BUFFER_START`), and the
  buffer streams through sidetone, the WinKey bridge (so any attached
  host sees per-byte K1EL echoes), and the radio-keying output (gated
  by the operator's `KEYING` setting).
- **Two entry points, single buffer.** The web UI's `<input>` and the
  WinKey bridge's text bytes both write into `MorseModel::_txBuffer`.
  Either side can fill the buffer; either side can start playback.
- **Edit-while-sending:** the operator can keep typing into the
  pending tail at any time during playback, including while the last
  chunk is being keyed. New chars land in pending and become a new
  chunk after the current one drains. This is the "TX already close,
  append a small fix" scenario.
- **Sent text is immutable.** Sent chars (whose marks have been keyed)
  are locked. The current chunk's unkeyed chars are also locked — they
  are mid-flight in the generator and can't be edited without
  producing audio artifacts. Edits land only in `pending`.
- **Persistence:** none. Buffer is wiped on reboot.

## 2. Operator reference

### 2.1 Web UI

Open the device's IP in any browser. The **TX buffer** card sits
between the **Live decode** and **Settings** cards. The display shows
three regions stacked horizontally:

```
[g rey — sent chars] [a ccent — current chunk] [editable input — pending]
                                  [TX] [Stop] [Clear]
```

Each region derives from one source:

| Region | Source indices in `_txBuffer` | Rendered in | Editable? |
|---|---|---|---|
| Sent | `[0 .. txSent-1]` | grey `<span id="txSent">` | no |
| In-flight | `[txSent .. txChunkStart+txChunkLen-1]` while chunk in flight | accent `<span id="txFlight">` with blinking caret | no |
| Pending | `[txChunkStart+txChunkLen .. txLen-1]` | white `<input id="txPending">` | **yes** |

When no chunk is playing (between chunks, or after completion), the
in-flight region is empty and `txEditableStart == txSent`.

**Buttons:**

| Button | When shown | Action |
|---|---|---|
| **TX** | `_txActive == false` AND `_txLen > _txSent` | POST `/api/tx/start` |
| **Stop** | `_txActive == true` | POST `/api/tx/stop` |
| **Clear** | always (with `confirm()` prompt) | POST `/api/tx/clear` |

**Editing in the pending field** is identical to a normal `<input>`:
type to append, Backspace to delete the last char. Each keystroke
POSTs to `/api/tx/edit`. The browser's `dataset.dirty` flag protects
against races where a `/state` poll arrives mid-typing — the polled
state is reconciled only when the user isn't actively typing.

### 2.2 WinKey host

Three new admin sub-commands and a status bit:

| Code | Mnemonic | Behaviour |
|---|---|---|
| `0x00 0x0C` | `ADMIN_TX_BUFFER_LOAD` | Enter LOAD mode. Future text bytes (≥ 0x20) accumulate in `_txBuffer`. WK 0x08 / 0x0A target `_txBuffer` while in this mode. |
| `0x00 0x0D` | `ADMIN_TX_BUFFER_START` | Exit LOAD mode and begin playback of the accumulated buffer. |
| `0x00 0x0E` | `ADMIN_TX_BUFFER_CLEAR` | Wipe `_txBuffer` (works in both LOAD and live modes). |

WK2 status byte **bit 4** (`0x10`) reports "TX buffer has unsent
chars". A WK2 host can poll `REQ_STATUS (0x15)` and check this bit to
know when the operator has fresh content to send.

Live-stream mode (the default, and the WK2 behaviour all shipping
hosts expect) is **unchanged** — bytes that arrive without a preceding
`ADMIN_TX_BUFFER_LOAD` continue to flow through `WinkeyBuffer` and key
immediately as today.

### 2.3 Edit-while-sending

The "TX already close" scenario from the brief is supported:

> Type "LAST", click TX. While the "S" of LAST is being keyed, type
> "+1". After LAST finishes, "+1" keys as a follow-up chunk.

The mechanism: typing into pending appends to `_txBuffer[txLen]`, which
is always past the current chunk's end. `TxBuffer::poll()` sees the new
chars in pending and pushes them as the next chunk once the current
one drains.

The same applies to host-driven entry: bytes arriving after
`ADMIN_TX_BUFFER_START` (which exits LOAD mode) go to the live stream
*unless* the host re-enters LOAD. Bytes arriving WHILE a chunk is
keying AND the bridge is in LOAD mode are appended to `_txBuffer`
positions `>= txChunkStart+txChunkLen` (the editable tail) and become
a follow-up chunk.

## 3. Storage

- **Location:** `MorseModel::_txBuffer`, in RAM.
- **Capacity:** 256 bytes including terminator → max 255 usable chars.
- **Persistence:** none. Cleared on reboot.
- **Backing NVS namespace:** none.
- **Why no persistence?** Operators typically only need a TX buffer
  during a single operating session. Avoiding NVS writes on every
  keystroke also avoids flash wear. fldigi and MMTTY do not persist
  the outgoing-text pane by default.

```
// MorseModel::_txBuffer — single source of truth
char _txBuffer[kTxBufLen];      // 256 bytes, NUL-terminated at _txLen
std::atomic<size_t> _txLen;     // current length 0..255
std::atomic<size_t> _txSent;    // chars whose marks have completed
std::atomic<size_t> _txHead;    // best-effort caret position
std::atomic<bool>   _txActive;  // a TX session is in progress
std::atomic<size_t> _txChunkStart; // buffer index of the current chunk's first char
std::atomic<size_t> _txChunkLen;   // length of the current chunk (0 when idle)
```

## 4. Playback paths

```
                 ┌─────────────────────┐
                 │  MorseModel._txBuffer│  (single source of truth)
                 └──────────┬──────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
   Web UI HTTP          WinKeyBridge         Direct / future
   /api/tx/edit         LOAD-mode feed       on-device UI
   appendTxChar()       cbTxBufferFeed       (none yet)
   backspaceTx()        (winkey.cpp)
        │                   │
        └───────────────────┘
                            │
                            ▼
                  TxBuffer::poll() (main loop, every tick)
                            │
                            ▼
              MorseGenerator::playText(chunk)
                            │
                            ▼
        AudioEngine → sidetone + KeyEventBus → RadioKeyer (GPIO4)
                            │
                            ▼
         appendDecodedChar(c, fromPlayer=true) on the audio task
                            │
                            ▼
                  ┌─────────────────────┐
                  │ MorseModel._txSent  │  advances by 1 per char
                  │ MorseModel._txHead  │  best-effort caret
                  └─────────────────────┘
```

### 4.1 Web UI → MorseModel

Every keystroke in the `<input>` field POSTs `/api/tx/edit` with
`{op:"append", c:"X"}`. The handler validates the character
(ASCII printable, room in the buffer), calls
`MorseModel::appendTxChar(c)`, and returns the updated TX state. The
browser's next `/state` poll confirms.

The TX button POSTs `/api/tx/start` → `TxBuffer::beginSession()` →
`MorseModel::startTx()`. Stop POSTs `/api/tx/stop` →
`MorseModel::stopTx()`. Clear POSTs `/api/tx/clear` →
`MorseModel::clearTx()`.

### 4.2 WinKey host → MorseModel

When `WinkeyBridge::_txBufferLoadMode == true`:

- Text bytes (≥ 0x20) → `WinkeyBridge::appendText()` →
  `_cb.txBufferFeed(c, ctx)` → `MorseModel::appendTxChar(c)` (in
  `winkey.cpp::cbTxBufferFeed`).
- WK 0x08 backspace → `_cb.txBufferBackspace(ctx)` →
  `MorseModel::backspaceTx()`.
- WK 0x0A clear → `_cb.txBufferClear(ctx)` → `MorseModel::clearTx()`.
- `ADMIN_TX_BUFFER_START` → bridge exits LOAD mode →
  `_cb.txBufferStart(ctx)` → `TxBuffer::beginSession()` (which calls
  `MorseModel::startTx()`).
- `ADMIN_TX_BUFFER_CLEAR` → `_cb.txBufferClear(ctx)` → `MorseModel::clearTx()`.
  Works in both LOAD and live modes.

When the host exits LOAD via START, the host can also re-enter LOAD
and stream more text; the bytes go into the SAME `_txBuffer` (which
now contains whatever was just played plus whatever the host is now
appending). The chunk-on-word-boundary driver picks up the new pending
chars after the current chunk drains.

### 4.3 TxBuffer driver → MorseGenerator

`TxBuffer::poll()` runs every loop iteration. State machine:

```
case !txActive              → return
case txActive && gen playing → update _txHead, return
case txActive && !gen playing:
    if chunkLen > 0:
        bumpTxSentBy(chunkLen)  // chunk drained; reset chunk boundary
    if txSent >= txLen:
        clearTxActive()         // session complete
        return
    else:
        // Push the next chunk. Walk buffer[txSent..txLen-1] up to and
        // including the next space (or end of buffer).
        pushNextChunk()
```

Each chunk is one word (plus its trailing space). The MorseEncoder
already emits a WORD_SPACE between words, so the audio gap is clean.
Single-word chunks with no trailing space (e.g. an operator's "+1"
correction that lands in pending without a space) push as one chunk
with a CHAR_SPACE appended internally by the encoder.

## 5. WinKey protocol additions

### 5.1 Admin sub-commands

Three new A1Keyer-specific admin sub-commands are reserved within the
WK2 spec's admin range (0x00-0x0F). Hosts that don't know about them
treat them as silently-accepted admin sub-commands (the bridge's
`handleAdmin` default branch is accept-and-ignore), so backwards
compatibility is preserved.

| Sub-command | Value | Host action | Device effect |
|---|---|---|---|
| `ADMIN_TX_BUFFER_LOAD` | `0x0C` | Enter LOAD mode | `_txBufferLoadMode = true`. Future text bytes / `0x08` / `0x0A` target `_txBuffer` instead of the live `WinkeyBuffer`. |
| `ADMIN_TX_BUFFER_START` | `0x0D` | Begin playback | `_txBufferLoadMode = false`. Calls `TxBuffer::beginSession()` which calls `MorseModel::startTx()`. |
| `ADMIN_TX_BUFFER_CLEAR` | `0x0E` | Wipe buffer | Calls `MorseModel::clearTx()`. Works in both LOAD and live modes. |

### 5.2 Status bit

The WK2 status byte's lower bits are documented in § 12.2 of the K1EL
WK2 datasheet:

- Bit 7: status tag (1)
- Bit 6: status tag (1)
- Bit 5: status tag (0)
- Bit 4: WAIT — defined as the "wait for XOFF to clear" flag, set
  in combination with bit 0 (XOFF) when the live buffer hits 2/3
  full.
- Bit 3: BREAKIN
- Bit 2: BUSY (chars pending in live buffer)
- Bit 1: reserved
- Bit 0: XOFF

A1Keyer reuses **bit 4** as a vendor extension: when `_txBuffer` has
unsent chars (`txLen() > txSent()`), bit 4 is set. The
`WAIT + XOFF` combination for the live buffer is unaffected — bit 4
in that context is only set when XOFF (bit 0) is also set, and our
new bit 4 is purely the TX-buffer-non-empty flag. K3NG-style hosts
that don't decode bit 4 see no change.

The status byte is read by hosts via `WK_REQ_STATUS (0x15)`. The host
can poll bit 4 to know when the operator has fresh text waiting in
the buffer.

### 5.3 0x08 / 0x0A in LOAD mode

While `_txBufferLoadMode == true`:

- `WK_BACKSPACE (0x08)` removes the last char from `_txBuffer` (calls
  `MorseModel::backspaceTx()`, which respects `txEditableStart`).
- `WK_CLEAR_BUF (0x0A)` wipes `_txBuffer` (calls
  `MorseModel::clearTx()`).

These target the TX buffer instead of the live `WinkeyBuffer`. The
live buffer is unaffected.

While `_txBufferLoadMode == false` (default and after START), `0x08`
and `0x0A` keep their existing behaviour (pop/clear the live buffer).

## 6. Edit boundary rules

The TX buffer has one editable window — `txEditableStart()` — that
shifts based on playback state:

```
txEditableStart =
    (txChunkLen > 0)
        ? txChunkStart + txChunkLen   // chunk in flight: editable = after the chunk
        : txSent                       // idle: editable = everything not yet keyed
```

This means:

- **No chunk in flight, no chars sent yet** → `txEditableStart == 0`.
  The whole buffer is editable.
- **No chunk in flight, some chars sent** → `txEditableStart == txSent`.
  Everything not yet keyed is editable.
- **Chunk in flight** → `txEditableStart == txChunkStart + txChunkLen`.
  Only the post-chunk tail is editable. The chunk itself is locked
  even though some of its chars haven't been keyed yet — editing
  mid-chunk would require re-issuing `playText()` on the in-flight
  chunk, which produces audible audio artifacts.

The buffer's append/backspace methods enforce this boundary:

```cpp
bool MorseModel::backspaceTx() {
    size_t len = _txLen.load(relaxed);
    size_t editStart = txEditableStart();
    if (len <= editStart) return false;   // nothing editable
    _txBuffer[len - 1] = '\0';
    _txLen.store(len - 1, relaxed);
    return true;
}
```

The web UI and the WinKey host both go through these methods, so the
boundary is enforced uniformly regardless of which entry point
writes. The HTTP handlers additionally validate the JSON payload so
invalid characters can't slip past the UI layer.

## 7. Concurrency and cross-core notes

### 7.1 Single-writer / multi-reader on `_txBuffer`

The buffer's byte storage is plain `char[]`. Writers:

- HTTP handler (Core 0, when the WebServer receives a request).
- WinkeyBridge callbacks (Core 0, from `WinkeyBridge::feed()` called
  by `Winkey::poll()`).
- `MorseModel::appendTxChar`/`backspaceTx`/`setTxText`/`clearTx` —
  all of which write through `_txBuffer[idx] = c` followed by
  `_txLen.store(...)`.

Readers:

- `TxBuffer::poll()` (Core 0) — reads `_txBuffer` to build chunks.
- `/state` handler (Core 0) — emits `_txBuffer` to JSON.

The single-writer invariant holds because all writers are on Core 0
and run synchronously through `MorseModel`'s methods. Relaxed-atomic
`_txLen` is the publish-after-write barrier: writers write the byte,
write the terminator, then `_txLen.store(len)`. Readers always check
`_txLen` first and never iterate past it.

This is the same pattern the existing decoded-text ring buffer uses
(`_textBuf` plain char[], `_textLen` atomic). Verified correct on
ESP32-S3 across many existing use cases.

### 7.2 `_charIdx` (MorseGenerator) is now atomic

`MorseGenerator::_charIdx` is now `std::atomic<size_t>` with relaxed
ordering. `TxBuffer::poll()` (Core 0) reads it via `charIndex()` to
update `_txHead` (the caret visual), while the audio task (Core 1)
writes it via `advanceToNextElement()`. This is the same
relaxed-atomic pattern as `MorseModel::_encoderChar`. The race is
benign — the caret visual is best-effort.

The authoritative "char is sent" path goes through
`MorseModel::appendDecodedChar(c, fromPlayer=true)`, which is called
from `MorseGenerator::advanceToNextElement()` (Core 1) and runs on
the audio task. `appendDecodedChar` advances `_txSent` and `_txHead`
atomically, with `_changeCounter` bumped as the publish barrier.

### 7.3 `_txSent` advance race

`_txSent` advances in two places:

1. `MorseModel::appendDecodedChar(c, fromPlayer=true)` (Core 1) —
   one char per player-char landed in the decoded text buffer.
2. `MorseModel::bumpTxSentBy(n)` (Core 0, called from
   `TxBuffer::poll()`) — `n` chars at once, when a chunk fully
   drains.

Path (1) is the authoritative incremental source. Path (2) is the
driver's reset of the chunk boundary (sets `_txSent += chunkLen` and
resets `_txChunkStart/_txChunkLen`). They cannot race in practice:
path (2) only fires when `gen->isPlaying() == false` (no audio task
is appending decoded chars), so by the time the driver bumps `_txSent`,
the audio task is silent. Even if a race did happen, the seq-cst
`_changeCounter` semantics at every increment give the renderer
visibility into the latest state.

### 7.4 Reset semantics

`resetParams()` and `resetForTest()` in `WinkeyBridge` both clear
`_txBufferLoadMode = false`. Live-stream mode is the default; the
host has to re-issue `ADMIN_TX_BUFFER_LOAD` to re-enter.

The TX buffer itself is NOT cleared by bridge resets — it's owned by
`MorseModel` and survives a `WinkeyBridge::resetForTest()`. This
matches the expectation that the operator's draft text survives a
defensive WK reset from the host.

## 8. Files and module map

### 8.1 New files

| Path | Purpose |
|---|---|
| `src/tx_buffer.h` | Public interface for the chunk-on-word-boundary playback driver. |
| `src/tx_buffer.cpp` | Implementation. Stateless on the module level — all state lives in `MorseModel`. |
| `test/test_tx_buffer/test_tx_buffer.cpp` | 17 host tests covering model ops, edit boundary, driver state machine, and WinKey LOAD-mode protocol. |

### 8.2 Modified files

- **`src/display_model.h`** — added `_txBuffer` and seven atomic
  counters, public read-side accessors, public write-side methods
  (`setTxText`, `appendTxChar`, `backspaceTx`, `clearTx`, `startTx`,
  `stopTx`, plus the internal `bumpTxSentBy`, `setTxChunk`,
  `setTxHead`, `clearTxActive`).
- **`src/display_model.cpp`** — added the method bodies; updated
  `appendDecodedChar(fromPlayer=true)` to advance `_txSent`/`_txHead`
  atomically when the source is a player char during a TX session.
- **`src/morse_generator.h`** — made `_charIdx` atomic; added
  `charIndex()` inline accessor.
- **`src/morse_generator.cpp`** — converted all `_charIdx`
  reads/writes to atomic loads/stores/fetch-adds.
- **`src/winkey_bridge.h`** — added 5 new callbacks (`txBufferFeed`,
  `txBufferBackspace`, `txBufferClear`, `txBufferLoad`,
  `txBufferStart`) plus 1 query (`txBufferHasPending`). Public
  `txBufferLoadMode()` accessor and `setTxBufferLoadMode(bool)`
  setter.
- **`src/winkey_bridge.cpp`** — added 3 admin sub-commands (0x0C,
  0x0D, 0x0E); branched `appendText`, `WK_BACKSPACE`,
  `WK_CLEAR_BUF` on `_txBufferLoadMode`; added bit 4 to
  `statusByte()`; clear the load mode in `resetParams()`.
- **`src/winkey.h`** / **`src/winkey.cpp`** — added 5 device-side
  callbacks (`cbTxBufferFeed`, `cbTxBufferBackspace`,
  `cbTxBufferClear`, `cbTxBufferLoad`, `cbTxBufferStart`) and
  `cbTxBufferHasPending` query, wired in `Winkey::begin()`. Added
  public `Winkey::beginTxSession()` hook.
- **`src/main.cpp`** — added `TxBuffer::poll()` next to `Winkey::poll()`
  in the main loop.
- **`src/web_ui.cpp`** — added 5 `/api/tx/*` routes
  (`/api/tx/text`, `/api/tx/edit`, `/api/tx/start`, `/api/tx/stop`,
  `/api/tx/clear`); added the TX-buffer card markup, CSS, and JS
  (`renderTx()`, `bindTx()`) in the inline HTML.
- **`src/console_server.cpp`** — added 7 TX-buffer fields to the
  `/state` JSON output: `txBuffer`, `txLen`, `txSent`, `txHead`,
  `txChunkStart`, `txChunkLen`, `txActive`.
- **`CMakeLists.txt`** — added `src/tx_buffer.cpp` to `LIB_SOURCES`;
  added `test_tx_buffer` to the test suite list.

## 9. Verification

### 9.1 Host tests

```
cmake -B build && cmake --build build && ctest --test-dir build -V
```

Targeted:

```
ctest --test-dir build -R test_tx_buffer       -V
ctest --test-dir build -R test_winkey_bridge    -V   # regression — admin parser
ctest --test-dir build -R test_winkey_playback  -V   # regression — live-stream path
ctest --test-dir build -R test_morse_generator  -V   # regression — charIndex() accessor
ctest --test-dir build -R test_display_model    -V   # regression — TX fields in append path
ctest --test-dir build -R test_wabun            -V   # regression — UTF-8 table lookup
```

The TX-buffer suite has 17 tests:

```
test_set_tx_text
test_append_tx_char
test_append_tx_char_cap
test_backspace_tx
test_backspace_tx_at_empty_is_noop
test_clear_tx
test_editable_start_when_idle
test_start_tx_requires_pending
test_stop_tx_preserves_buffer
test_poll_pushes_first_chunk
test_backspace_blocked_when_chunk_in_flight
test_poll_completion_clears_active
test_winkey_load_mode_routes_to_model
test_winkey_load_mode_start_arms_session
test_winkey_live_mode_unchanged
test_status_byte_bit4_tx_buffer_non_empty
test_winkey_clear_works_in_both_modes
```

### 9.2 Device build

```
pio run -e esp32s3_cardputer
```

Clean compile expected.

### 9.3 On-device smoke test

1. **Cold boot with empty buffer.** Web UI shows empty TX field, TX
   enabled, Stop hidden.
2. **Type "CQ CQ DE W1AW K "** into the pending field. Each keystroke
   appears; no audio.
3. **Click TX.** Audio + (with KEYING) GPIO4 toggling. Sent region
   grows by word. In-flight region shows the rest of the current word.
   Pending empty after last char.
4. **Mid-TX append:** while "DE " is keying, type "TEST " into
   pending. After "DE " finishes, "TEST " keys as the next chunk.
5. **Auto-completion:** buffer drains → status reads "complete —
   16 chars sent". TX button re-enabled. Buffer preserved.
6. **Click TX again** → only the appended tail sends.
7. **Click Stop mid-word.** Audio cuts. Sent chars stay grey. Pending
   preserved. Click TX → resumes.
8. **Edit boundary:** try to delete a sent char via the UI (impossible
   — input doesn't show sent chars) or via direct API (rejected with
   HTTP 400).
9. **Refresh mid-TX** → state re-renders from `/state`.
10. **WinKey-driven path:** connect a host (e.g. `wk2ping`,
    RUMlogNG), send `0x00 0x0C` (LOAD), stream `"ABC"`, send
    `0x00 0x0D` (START). Web UI shows "ABC" in the buffer, then
    drains. WK2 status byte bit 4 (`0x10`) reflects buffer state.
11. **WinKey backspace in LOAD mode:** send LOAD, stream `"ABCDE"`,
    send `0x08` twice, then START. Web UI shows "ABC", drains "ABC".
12. **WinKey CLEAR in LOAD mode:** send LOAD, stream `"X"`, send
    `0x00 0x0E`, then START. Buffer empty, no audio.
13. **Live mode unchanged:** with no LOAD command, stream `"TEST"`
    directly. Audio keys "TEST" immediately, web UI buffer unchanged.
14. **Cap:** type 256 chars into the pending field → 256th rejected
    with HTTP 400.

## 10. Future work

- **Pause/resume.** Stop is the only pause today. A real pause would
  halt the MorseGenerator mid-element and resume cleanly. Risk: the
  generator's existing `stop()` resets internal state, so a true
  pause requires a new `pause()` method that just halts the audio fill
  without resetting element pointers.
- **On-device keyboard overlay.** A `T` key from DECODER could open
  a TX editor screen with the same three-region visual and TX/Stop
  buttons. Mirrors the Wi-Fi passphrase screen pattern.
- **Wabun / UTF-8 input.** Currently limited to ASCII printable. The
  Wabun MorseTable supports katakana; a UTF-8-aware text input would
  unlock that. Risk: the WebServer's JSON handlers would need to
  handle multi-byte sequences; the input field's `maxlength` is a
  byte count which would diverge from a glyph count.
- **Persistence toggle.** A NVS key could store the unsent buffer
  across power-cycles. Useful for contest operation but adds NVS
  wear. fldigi/MMTTY do not do this by default.
- **Macro expansion.** `{MYCALL}` substitution from a `name` table.
  Already partially supported by the memory-keyer feature.
- **Multi-stream.** Two parallel TX buffers (e.g. one for contest
  exchange, one for free-form chat). Adds UX complexity for marginal
  benefit in the typical use case.
