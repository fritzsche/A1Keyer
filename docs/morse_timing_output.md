# Morse Timing, Decode Display, and Chunk-Boundary Handling

This document describes the chain of components that turn typed text into
keyed CW on the A1Keyer (ESP32-S3 M5Stack Cardputer), explains why the
decoded-text display sometimes lags or shifts when the host sends text
faster or slower than the audio can drain, and captures the regressions
found and fixed during the 2026-08 work on long-call decode shifts.

## 1. Component chain

```
                host (RUMlogNG / N1MM / fldigi)
                       │ USB CDC serial
                       ▼
                ┌──────────────┐
                │ WinkeySerial │  raw byte stream
                └──────┬───────┘
                       │ feed(b)
                       ▼
                ┌──────────────┐
                │ WinkeyBridge │  protocol state machine, buffer, drain
                └──────┬───────┘
                       │ cbSendText(chunk, isContinuation)
                       ▼
                ┌──────────────┐
                │ MorseGen     │  encode → elements → audio + appendDecodedChar
                └──────┬───────┘
                       │ fillSamplesMono() + appendDecodedChar(c)
                       ▼
                ┌──────────────┐
                │ MorseModel   │  200-char ring buffer (decoded text)
                └──────┬───────┘
                       │ /state HTTP endpoint + DisplayTask
                       ▼
            web UI decoded line + device screen
```

The relevant timings:

| stage                | rate                                  |
|----------------------|---------------------------------------|
| host serial send     | bursty — one USB-CDC packet per line  |
| bridge poll() drain  | every loop() tick (a few ms)          |
| MorseGen audio       | one dit at 20 WPM = 60 ms             |
| MorseGen decode append | once per CHAR_SPACE / WORD_SPACE    |

The **host serial rate is typically faster than the audio rate**. A
single 31-char string like "CQ CQ DE DJ1TF DJ1TF DJ1TF PSE K" can be
fed into the bridge in a fraction of a second, but the audio takes ~12
seconds to key. So bytes accumulate in the bridge buffer while audio
plays.

## 2. The encoder's silence model

`MorseEncoder::encode` (`src/morse_encoder.cpp`) walks the source and
emits a stream of `Element`s. Three element types matter for timing:

| source position | element     | units | meaning                |
|-----------------|-------------|-------|------------------------|
| letter          | DIT         | 1     | key-down 1 unit        |
| letter          | DAH         | 3     | key-down 3 units       |
| between letters | CHAR_SPACE  | 2     | extra silence 2 units  |
| space char      | WORD_SPACE  | 6     | extra silence 6 units  |

Each mark element already includes a **1-unit trailing silence** baked
into the `KeyEnvelop` envelope. So a CHAR_SPACE produces a total of
1 (envelope trailing) + 2 = **3 units** of inter-character gap, and a
WORD_SPACE produces 1 + 6 = **7 units** of inter-word gap — exactly the
spec values.

The encoder is **chunk-aware**: it does NOT emit a trailing CHAR_SPACE
after the last character of a string, because there is no next char
within this `encode()` call. This is the correct behaviour for a
self-contained playback, but it creates a problem when a single
playback is split across multiple `playText()` calls (see § 4).

## 3. The bridge's drain / continuation lifecycle

`WinkeyBridge::poll()` is the bridge's per-tick housekeeping. It
decides whether to drain the accumulated text buffer into the
`cbSendText()` hook, based on the consumer's audio state. The full
state machine (2026-08-18 revision) is:

```
                  feed(b) per serial byte
                         │
                         ▼
              ┌──────────────────────┐
              │   _buffer (FIFO)     │   128-char ring
              └──────────┬───────────┘
                         │ poll() per loop() tick
                         ▼
        ┌────────────────────────────────┐
        │ 1. _cb.canAcceptText() ?       │   is consumer busy?
        │ 2. busy→idle edge this tick?   │   _prevConsumerBusy && !consumerBusy
        │ 3. _buffer non-empty?          │
        └────────────────┬───────────────┘
                         │
   buffer empty ─────────┴───────────── buffer non-empty
        │                                  │
        ▼                                  ▼
   edge seen? → latch                edge seen?
   _sawAudioEndEdge = true           + _prevBufferNonEmpty?
   _isContinuation = false             │
        │                              ├─ yes → chunked-stream:
        ▼                              │         _isContinuation = true
   return                                  (MorseGen prepends CHAR_SPACE)
                                         │
                                         ├─ no  → fresh playback:
                                         │         _isContinuation = false
                                         │
                                         ▼
                                    consumerBusy?
                                         │
                                         ├─ yes → do not drain
                                         │         (bytes accumulate)
                                         │
                                         └─ no  → drain all bytes
                                                  cbSendText(chunk)
                                                  _isContinuation = true
                                                  (next chunk that
                                                   arrives during audio
                                                   is a continuation)
```

The key invariant: **`_isContinuation` must be `true` ONLY for chunks
that follow a previous chunk's audio without any gap where bytes
weren't accumulating**. Otherwise the prepend fires and the decoded
text shifts.

### 3.1 The two-flag edge detector

Two state bits disambiguate the chunked-stream case from the
fresh-playback case:

* `_prevConsumerBusy` (bool) — was the consumer busy on the previous
  poll()?
* `_prevBufferNonEmpty` (bool) — was the buffer non-empty on the
  previous poll()?

The busy→idle edge fires `_prevConsumerBusy && !consumerBusy`. When
that edge fires with the buffer currently non-empty:

* If `_prevBufferNonEmpty` was true → the bytes have been
  accumulating since BEFORE the audio went busy. They are the
  accumulated next chunk, which is a continuation of the just-finished
  chunk. **`_isContinuation = true`.**
* If `_prevBufferNonEmpty` was false → the bytes arrived after the
  audio finished. They are a fresh playback. **`_isContinuation = false`.**

When the buffer is empty and the busy→idle edge fires, we set the
latch `_sawAudioEndEdge = true` and clear `_isContinuation = false`.
The NEXT poll that finds the buffer non-empty consumes the latch and
keeps `_isContinuation = false` (fresh playback arrived ≥1 poll after
audio wound down).

### 3.2 Why a third bug needed this

Before the 2026-08 fix, `_isContinuation` was reset to `false` only on
the empty-buffer / busy→idle path. Bytes that arrived one tick after
the audio wound down correctly saw `_isContinuation = false`. But bytes
that arrived on the SAME tick as the busy→idle edge were wrongly
marked as a continuation — they were treated as if they had been
accumulating during the busy phase when in fact they arrived just
after.

This is the scenario the failing test
`test_bridge_one_char_at_a_time_long_call_no_shift` reproduces: the
host feeds bytes one at a time, the bridge polls between each feed,
and each byte that happens to arrive on the same poll that sees
busy→idle was mis-classified as a continuation. The repro showed
`got = "CQC QD ED J1TFD J1TFD J1TFP SEK"` for a single chunk of
`"CQ CQ DE DJ1TF DJ1TF DJ1TF PSE K"` — every space replaced by the
adjacent mark, all chars shifted left by 1.

## 4. The prepend's append (the deepest bug)

When the bridge correctly identifies a chunk as a continuation,
`MorseGenerator::playText(text, isContinuation=true)` prepends a
2-unit CHAR_SPACE element to the encoder's output. This is a PURELY
AUDIO bridge — it preserves the spec's inter-character gap that the
encoder omits at chunk tail. It must NOT trigger a decoded-char append.

Before 2026-08-18, the prepended silence's append fired:

```cpp
// silence branch:
if (_playText[_charIdx] != '\0') {
    MorseModel::instance().appendDecodedChar(_currentChar, true);
    ...
    ++_charIdx;
    _currentChar = _playText[_charIdx];
}
```

`_currentChar` had just been reset by `playText` to `_playText[0]`
(the first char of chunk2). So the prepend's silence appended the
first char of chunk2 — a char that hadn't been keyed yet. Then the
next mark branch's skip-spaces loop advanced `_charIdx` past any
leading space in chunk2, and `_currentChar` was set to the wrong
char.

The fix: `playText` sets a one-shot `_suppressNextSilenceAppend` flag
when it prepends, and the silence branch consumes the flag (skipping
the append AND skipping the `_charIdx` advance — the next mark's
skip-spaces loop will run from `_charIdx = 0` and find the correct
char).

This is the bug that produced the user's "PSE K" → "P SEK" shift on
long calls. It only manifested when:

1. The bridge split a memory text mid-word (no trailing-space
   boundary silence on the chunk).
2. The next chunk started with a leading space (the inter-word gap
   that the encoder emits as WORD_SPACE).

Both conditions are common in real host streaming because RUMlogNG /
N1MM pace their outgoing-CW buffer based on the operator's typing,
not on the audio rate. A "long call" (32+ chars with multiple words)
is much more likely to hit both conditions than a short text.

## 5. The display's circular buffer

`MorseModel::appendDecodedChar(c, fromPlayer)` (`src/display_model.cpp`)
appends one char to a 200-char ring buffer with `_textHead` / `_textTail`
/ `_textLen` indices. Each call increments a change counter that
triggers a display render.

The buffer is large enough for ~6 callsigns at a typical contest pace
without wrap-around. Wrap-around silently drops the oldest char (the
tail advances), which is correct for a "scrolling" decoded-text
display but means the user-visible text is always the most recent
200 chars.

The web UI's `/state` endpoint reads the ring buffer from `textTail()`
for `textLen()` chars, oldest-first. The device screen renders the
same way (via `DisplayTask`'s render hook).

## 6. Bugs found and fixed (2026-08)

| Bug | Symptom | Root cause | Fix | Test |
|-----|---------|-----------|-----|------|
| 1-unit too long gap | inter-char audio gap was 4 units instead of 3 | `MorseGenerator` hardcoded CHAR_SPACE=3 and WORD_SPACE=7, ignoring encoder's units=2/6 | use `el.units` from the encoder | `test_first_play_after_wpm_change_produces_TU_not_X` |
| UR 5NN TU → "X" | chunked bridge playback dropped the inter-char gap between chunks | bridge reset `_isContinuation=false` on empty-buffer poll, so next chunk's prepend never fired | keep flag true after `cbSendText`, only reset on busy→idle edge | `test_bridge_chunks_preserve_inter_char_T_to_U` |
| Decode shift on back-to-back independent playback | web ▶ twice in a row showed "CQC QJ J1QPB/1J…" | bridge's `_isContinuation` flag stayed true across sessions because of the previous fix | add explicit reset on busy→idle edge + `_sawAudioEndEdge` latch | `test_bridge_two_independent_playbacks_do_not_shift_text` |
| "CQ " (trailing space) during playback | second C of "CQ CQ" audio played while display still showed trailing space | mark branch set `_currentChar` to a space (because WS branch left `_charIdx` pointing at one) | add skip-spaces loop in mark branch to advance past spaces | `test_word_boundary_displays_inter_word_space_at_word_boundary` |
| Long-call decode shift | `"CQ CQ DE DJ1TF DJ1TF DJ1TF PSE K"` rendered as `"...DJ1TFP SEK"` (or worse with one-char-at-a-time pacing) | bridge mis-classified one-char-at-a-time bytes as continuation; generator's prepend then advanced `_charIdx` past the first chunk's char into the next space | track `_prevBufferNonEmpty` to disambiguate chunked-stream from one-char-at-a-time; add `_suppressNextSilenceAppend` flag to skip the prepend's append | `test_bridge_one_char_at_a_time_long_call_no_shift` |

## 7. Why host tests caught what device-only testing missed

The early-session regressions were visible on hardware because the
host (USB CDC) is the source of bytes and the device's poll cadence
is the source of "drain now / wait" decisions. Reproducing them
required a unit-test harness that:

1. **Wires a real `WinkeyBridge` to a real `MorseGenerator`** — not
   just direct `playText()` calls. The bridge's chunk-boundary
   detection is what was buggy.
2. **Sets `cb.ctx = this`** so the static callbacks can find the
   test harness. (Easy to get wrong — a null `ctx` causes a
   segfault on the first `cbSendText`.)
3. **Interleaves `fillSamplesMono()` and `bridge.poll()`** rather
   than draining all audio between feeds. The busy→idle edge is only
   observable when the bridge polls while audio is busy. Without
   interleaving, the bridge never sees the busy state.
4. **Uses one-char-at-a-time pacing** for the worst-case scenario.
   This is the production cadence for slow loggers / USB-CDC
   without Nagle coalescing.

The `BridgeRunner` helper struct in
`test/test_morse_generator/test_morse_generator.cpp` provides these
wiring details once, so each regression test just composes them with
its specific feed/drain pattern.

## 8. Why an actual long-call reproduction was so hard

The user's `"CQ CQ DE DJ1TF DJ1TF DJ1TF PSE K"` (32 chars) only
triggers the bug when:

* the bridge splits the string somewhere mid-word (no trailing-space
  boundary on the chunk), AND
* the next chunk starts with a leading space (the WS in encoder
  output).

Splitting the string at every space (3 chunks) does NOT reproduce
the bug — each chunk ends at a MARK but each chunk starts with a
SPACE, so the prepend never fires (front.keyDown is false). The bug
needs both a non-space-end and a space-start at the chunk boundary.

Splitting the string at every CHAR (32 chunks) also doesn't reproduce
the bug through the original single-flag detector — but with my new
`_prevBufferNonEmpty` flag the test correctly catches the
misclassification.

The actual production trigger is somewhere in between: the host
sends bytes faster than audio can drain, the bridge accumulates a
chunk that happens to end mid-word, and the next chunk happens to
start mid-word with a leading space (because the next word in the
slot starts there). For a long call like "DJ1TF PSE K" there are
many opportunities for such a boundary; for a short text like "CQ"
the opportunities are zero.

## 9. Files involved

| File | What it does |
|------|-------------|
| `src/winkey_bridge.h` | bridge state machine declarations; `_prevBufferNonEmpty` member added |
| `src/winkey_bridge.cpp` | `poll()` lifecycle; `resetParams()` clears all three flags |
| `src/morse_generator.h` | `_suppressNextSilenceAppend` flag declaration |
| `src/morse_generator.cpp` | `playText()` sets the flag when it prepends; silence branch consumes the flag and skips append + charIdx advance |
| `src/morse_encoder.cpp` | encoder semantics (unchanged); produces correct CHAR_SPACE / WORD_SPACE units |
| `src/morse_decoder.cpp` | paddle-side decoder (unchanged); produces SPACE_CHAR for word gaps |
| `src/display_model.cpp` | `MorseModel::appendDecodedChar` — 200-char ring buffer |
| `src/winkey.cpp` | device-side glue; `cbSendText` reads `isContinuation()` and passes to `playText` |
| `test/test_morse_generator/test_morse_generator.cpp` | `BridgeRunner` helper + regression tests |

## 10. Regression tests in this change

The session added (or made robust) these tests:

* `test_bridge_chunks_preserve_inter_char_T_to_U` — bridge feeds
  `"UR 5NN "` then `"TU"` with interleaved drain+poll. Audio must
  include the CS between T and U. Regression test for the original
  "TU sounds like X" bug.
* `test_bridge_single_chunk_TU_audio_correct` — baseline single-chunk
  test for the same audio shape.
* `test_bridge_two_independent_playbacks_do_not_shift_text` — plays
  the same memory twice with 200 empty polls between, asserts decoded
  text matches source on both. Catches the
  back-to-back-independent-playback decode shift regression.
* `test_bridge_one_char_at_a_time_long_call_no_shift` — feeds the
  32-char source one char at a time with minimal drain, asserts
  decoded text matches source. Catches the long-call decode shift.
* `test_bridge_three_chunks_long_call_no_text_shift` — feeds the
  source in 3 chunks (split at spaces) with full drain between,
  asserts decoded text matches source.
* `test_bridge_chunks_ending_at_mark_no_text_shift` — feeds
  `"AB CD EF GH"` in 5 chunks each ending at a MARK, asserts no
  shift. The most adversarial split for the boundary gap.

All six pass on the current code.