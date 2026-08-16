# Memory Keyer

A1Keyer's **memory keyer** lets the operator store up to ten canned CW
exchanges — contest reports, station IDs, a quick `CQ CQ DE W1AW K` —
and play any one of them with a single keystroke. The same playback
drives the **sidetone**, the **WinKeyer host protocol** (so a connected
logger sees the bytes), and the **radio keying output** (gated by the
operator's `KEYING` setting). Editing uses the same on-device keyboard
flow as the Wi-Fi passphrase entry, with the slot's persisted text
pre-filled so the operator can edit in place rather than retype.

This document is both the **development plan** (architecture, files,
verification) and the **operator reference** (keystrokes, behaviour).
Sections 1–4 are operator-facing; sections 5–10 are developer-facing.

---

## Contents

1. [Overview](#1-overview)
2. [Operator reference](#2-operator-reference)
3. [Storage](#3-storage)
4. [Playback paths](#4-playback-paths)
5. [Editing flow](#5-editing-flow)
6. [Concurrency and cross-core notes](#6-concurrency-and-cross-core-notes)
7. [Files and module map](#7-files-and-module-map)
8. [Risks and trade-offs](#8-risks-and-trade-offs)
9. [Verification](#9-verification)
10. [Future work](#10-future-work)

---

## 1. Overview

- **Function:** ten named CW text slots, addressable by the digits
  `0`..`9`. Pressing a digit from the **DECODER** screen plays the slot
  through sidetone, the WinKeyer bridge, and the radio keying output.
  Editing uses a three-gesture flow: `M` opens a single-slot picker,
  `0`..`9` switches which slot is shown, then `Enter` opens the editor
  pre-filled with the currently shown slot. `X` / `x` clears the shown
  slot from the picker.
- **Persistence:** NVS namespace `"memory"`, loaded once at boot and
  written back on every commit (Enter from the editor).
- **Default:** **all slots empty**. Pressing a digit for an unset slot
  is a silent no-op — no click, no error beep.
- **Target:** M5Stack Cardputer ADV only (keyboard-driven UI; Tab5
  doesn't compile the overlay).

---

## 2. Operator reference

### 2.1 Keystroke table

| Keystroke | From | Action |
|---|---|---|
| `0`..`9` | DECODER | Play slot N. Empty slots are no-ops. |
| `M` | DECODER | Open the memory-keyer picker (`MEMORY_PICK`). Cursor lands on slot 0. |
| `0`..`9` | MEMORY_PICK | Switch the shown slot to N. Stays on `MEMORY_PICK`. |
| `X` / `x` | MEMORY_PICK | Clear the currently shown slot and persist. Stays on `MEMORY_PICK`. |
| `Enter` | MEMORY_PICK | Open the editor (`MEMORY_EDIT`) pre-filled with the currently shown slot. |
| `Esc` | MEMORY_PICK | Cancel, return to DECODER. |
| `0`..`9` | MEMORY_EDIT | Type the digit into the buffer (digits are valid CW text). |
| `,` | MEMORY_EDIT | Type the comma character. |
| `/` | MEMORY_EDIT | Type the slash character. |
| `Fn` + `,` | MEMORY_EDIT | Move cursor left. |
| `Fn` + `/` | MEMORY_EDIT | Move cursor right. |
| `OPT` | MEMORY_EDIT | Toggle caps lock (same as the Wi-Fi password screen). |
| `Enter` | MEMORY_EDIT | Commit, save to NVS, return to DECODER. |
| `Esc` | MEMORY_EDIT | Discard edits, return to DECODER. |
| `M` | MEMORY_PICK / MEMORY_EDIT | Ignored (no nesting). |
| `P` | MEMORY_PICK / MEMORY_EDIT | Ignored — typing P inside a memory must not fire the "Hello Morse!" demo. The P-key still works from DECODER. |

### 2.2 Switching slots and committing

Switching slots is a picker-only action: press `0`..`9` while on
`MEMORY_PICK` to bring a different slot to the front. The screen
re-renders with the new slot's text. Digits do **not** commit you into
the editor — you stay on `MEMORY_PICK` until you press `Enter`.

The full "edit a slot" flow is therefore:

1. From `DECODER`, press `M`.
2. Press `0`..`9` to position the cursor on the slot to edit (or
   accept the default slot 0).
3. Press `Enter` to open `MEMORY_EDIT` for that slot.
4. Edit the buffer; press `Enter` to commit, or `Esc` to discard.

Pressing a digit inside `MEMORY_EDIT` types the digit into the buffer
— it does not switch slots. Digits are common in CW text (`5NN`, `599`,
contest exchanges) and an accidental slot switch would overwrite
in-progress edits. To edit a different slot, `Esc` (or commit) back to
`DECODER` and re-enter the picker.

### 2.3 Playback-when-busy

Pressing a digit while a previous slot is still playing is a no-op.
The new request is dropped, not queued. The current playback continues
uninterrupted. This matches the `P` "Hello Morse!" behaviour at
`main.cpp` and prevents audible clicks from a mid-element restart.

### 2.4 Empty slots

Pressing a digit for an unset slot is silent. The screen does not flash
an error — `playLocalMemoryText` returns when `text[0] == '\0'`.

### 2.5 Persistence

The bank is loaded into the model once at boot (`setup()` in
`main.cpp`). The bank is written back on every commit (Enter from the
editor) and on every clear (`X` / `x` from the picker) via
`memoryBankSave()`. Power-cycling preserves the operator's stored text.
Clearing the device settings (the `"morse"` namespace) does **not**
affect the memory bank — they live in separate NVS namespaces.

### 2.6 Cancel-on-any-keypress

While a memory slot or the `P`-key "Hello Morse!" is playing, **any
key press stops playback immediately**. This is true for:

- **Cardputer keyboard** — any key (W, F, V, M, K, D, C, N, digits,
  `P`, Enter, Esc, …).
- **Morse paddle** — pressing either the DIT or DAH paddle.

The key press is **consumed**: it stops playback but does **not**
generate morse code, key the radio, open an overlay, or perform any
other action. While the key remains held, it is ignored — the next
"real action" requires the operator to release and re-press. This
matches the intuition that any keydown during playback is a deliberate
abort, not a multi-tap.

Concrete examples:

- Press `P` from `DECODER` → "Hello Morse!" plays. Hold `P` through
  playback → playback stops on the first held tick, no restart on
  subsequent held ticks. Release and re-press `P` → "Hello Morse!"
  plays once.
- Press digit `3` from `DECODER` → memory 3 plays. Press `W` mid-macro
  → playback stops; `WPM_SETTINGS` does **not** open. Release `W`,
  press `W` again → overlay opens.
- Press digit `3` from `DECODER` → memory 3 plays. Tap the DIT paddle
  mid-element → playback stops; GPIO4 stays LOW for the entire hold.
  Release the paddle, tap it again → a real dit (sidetone blip, GPIO4
  HIGH for one dit-length).
- Squeeze both DIT and DAH during playback → playback stops; both
  paddles are ignored until **both** are released. Then the next
  paddle keydown is a real action.

See § 4.4 and § 6.3 for the implementation rationale.

---

## 3. Storage

### 3.1 NVS namespace and keys

- **Namespace:** `"memory"` (separate from `"morse"` so the device
  settings and the memory bank don't collide).
- **Keys:** `"m0"`, `"m1"`, ..., `"m9"`.
- **Encoding:** `Preferences::putString` / `getString` — UTF-8
  internally; CW text is ASCII so this is safe.
- **Capacity per slot:** 81 bytes including the terminator, so **80
  characters** of usable CW text. Long enough for contest exchanges
  like `"CQ TEST DE W1AW K 5NN 001 BK"` plus headroom; the editor
  scrolls horizontally when the cursor runs past the visible window
  at the size-3 font, so longer macros stay readable.

### 3.2 Schema source of truth

The constants live in `src/memory_store.h`:

```cpp
inline constexpr const char* kMemoryNamespace = "memory";
inline constexpr uint8_t kMemSlots = 10;
inline constexpr size_t  kMemLen   = 81;
```

`memoryBankLoad(MemoryBank& out)` reads each slot key into the
corresponding `MemoryBank::slot[i]`, NUL-terminating at `kMemLen - 1`.
`memoryBankSave(const MemoryBank& bank)` writes every slot;
`Preferences::putString` is a no-op when the value is unchanged, so
untouched rows cost nothing. `memoryBankClear()` erases the `"memory"`
namespace only — leaving the `"morse"` namespace alone.

### 3.3 Empty-slot semantics

NVS stores an empty-string entry when a slot has been saved with
`text == ""`. `memoryBankLoad` distinguishes "missing" (key not present)
from "present but empty" — both result in `slot[i][0] == '\0'`, which
is the only state the operator observes.

---

## 4. Playback paths

The playback pipeline is intentionally short so each layer can be
tested in isolation:

```
digit → MorseModel::getMemory(slot)         (read the row)
       → Winkey::playLocalMemoryText(text)  (entry point)
            ├─ if WinKeyer bridge is open:
            │    bridge.feed(byte) × len    (per-byte K1EL echo)
            │    bridge.poll()              (drain → cbSendText → MorseGenerator::playText)
            └─ else:
                MorseGenerator::playText(text) (sidetone + radio keying via KeyEventBus)
```

### 4.1 Open bridge path (host attached)

When a host logger is connected in WinKey mode (the default at boot),
the bytes are routed through `WinkeyBridge::feed()` so the host sees
the same per-byte K1EL echo it would see for any text it sent itself.
The bridge's `poll()` drains the accumulated FIFO through
`cbSendText` → `MorseGenerator::playText()` as a single chunk, so the
memory playback is **indistinguishable from host-driven playback**
on the wire and on the air.

### 4.2 No-host path

When the WinKeyer bridge is not open (Console / debug mode), the text
goes straight to `MorseGenerator::playText()`. Sidetone plays
through the speaker; the radio keying output follows the elements via
the `MorseGenerator → KeyEventBus → RadioKeyer` wiring added in this
change. See § 6.

### 4.3 Busy gate

If `MorseGenerator::isPlaying()` is true when `playLocalMemoryText`
runs, the call returns immediately. This prevents a mid-element restart
which would produce an audible click and a confusing on-air glitch.
The operator can hit the digit again after the current playback
completes (or after explicitly cancelling it — see § 4.4).

### 4.4 Cancel-on-any-keypress gates

Two consumer-side gates enforce the operator-facing behaviour in § 2.6.
Both run *before* any per-key handler / keyer branch, so the consumed
press never reaches the rest of the system.

**Keyboard side** — `src/main.cpp::handleKeyboard()`:

- After all key state is sampled but before the W/F/V/M/D/C/N/K/P
  handlers run, if `MorseGenerator::isPlaying()` is true, the gate
  calls `gen->stop()`, sets `MorseModel::setMode(KeyerMode::KEYER)`,
  copies every `wasX = xKey` (so the next tick sees no stale edge
  for any tracked key), and returns.
- The `wasX = xKey` update is what implements "ignore the key until
  release": holding `W` through the stop does **not** fire the `W`
  handler on subsequent ticks. The operator must release and re-press
  for the action to take effect.
- This gate runs on the main loop (Core 0 in current builds).

**Paddle side** — `src/audio_engine.cpp::fillBuffer()`:

- At the top of `fillBuffer()` (the audio task on Core 1), if the
  generator is playing and a paddle is held, the gate calls
  `gen->stop()`, sets mode back to `KEYER`, and sets the static flag
  `AudioEngine::s_paddleSuppressed = true`.
- While `s_paddleSuppressed` is true, `fillBuffer()` skips every
  keyer branch (straight / iambic / morseGen) and emits silence.
  This keeps the radio quiet (no `KeyEventBus::keyDown` from the
  iambic keyer), the decoder ring buffer empty, and the sidetone
  silent for the entire duration of the held press.
- When the paddle is released, `fillBuffer()` clears the flag and
  calls `MorseKey::clearMemory()` to drop any pending memory flags.
  The release tick itself is also silenced so the falling edge is
  not observed by the iambic keyer.

**Why two gates?** The two press sources run on different cores and
go through different consumers: the keyboard handler runs on the main
loop and dispatches to per-key handlers; paddle presses are GPIO
interrupts read by the iambic/straight keyer in the audio task.
Adding a single shared gate would either be racy or require a
cross-core flag for every keyer state. Two narrow gates keep each
core's logic self-contained. The two gates are independent — both
may fire on the same tick (e.g. paddle press + keyboard key), and
both `gen->stop()` calls are idempotent.

See § 6.3 for the cross-core-safety argument and § 8 for risks.

---

## 5. Editing flow

### 5.1 Modal screens

Two new `DisplayScreen` values drive the editor:

- `MEMORY_PICK` — opens after `M` from DECODER. Single-slot view: a
  large-font rendering of one slot's text (or `(empty)` in red), with
  a `"Memory N"` size-2 title in accent (the slot number is part of
  the title — there is no separate right-aligned badge), and a two-line
  hint row at the bottom listing `0-9: switch  ENTER: edit` and
  `X: clear  ESC: back`. The cursor slot is held in
  `MorseModel::_memoryPickSlot`; pressing `0`..`9` swaps to a
  different slot without leaving the screen, `X`/`x` clears the
  current slot and persists it, and `Enter` advances to `MEMORY_EDIT`
  for the currently shown slot.
- `MEMORY_EDIT` — opens after `Enter` is pressed in `MEMORY_PICK`
  (cursor-target determines which slot). Renders the `TextInput`
  control (the same widget used by the Wi-Fi passphrase screen), but
  **never masked** — memory text is not a secret like a passphrase, the
  operator must read what they typed. Pre-filled with the slot's
  persisted text, cursor at end.

### 5.2 Editor reuse

The editor reuses `TextInput` from `src/text_input.h` — no new widget
code. The renderer (`CardputerDisplay::showMemoryEdit`, modelled on
`showWifiPasswordInput` at `cardputer_display.cpp:517`) draws:

- Title `"Mem N"` (size 2) in accent colour, with a `"<len>/<cap>"`
  size-1 suffix that turns warning-red when within three characters
  of the cap.
- `CAPS` indicator (size 1) when caps lock is engaged.
- A field box (32 px tall to fit a size-3 glyph, full width minus
  margins) showing **the actual CW text — never masked**. Memory text
  is not a secret like a Wi-Fi passphrase; the operator needs to read
  what they typed. The size-3 font (≈18 px per glyph) gives 13 visible
  characters in the box; when the cursor runs past the right edge the
  visible window slides leftward so newly-typed text is always in
  view. Walking back left follows the window until it hits the left
  edge, then stays put.
- A static caret at the cursor column within the visible window.
- Hint rows: `"ENTER ok   FN , left  FN / right"` and
  `"OPT caps             ESC bk"`. No reveal/mask gesture — there is
  nothing to mask. Bare `,` and `/` are typed as ordinary text
  (CW macros routinely contain `/`); cursor navigation requires `Fn`
  to be held.

### 5.3 Commit vs. cancel

The `TextInput` is bound to a separate buffer (`_memoryEditorBuf` in
`MorseModel`), not to `_memory[slot]`. This is the same pattern the
Wi-Fi password flow uses — edits live in their own storage and are
written to the model on commit. The behaviour:

| Gesture | Effect on `_memory[slot]` | Effect on NVS |
|---|---|---|
| Enter (in editor) | Buffer copied to slot | Bank saved |
| Esc (in editor) | Unchanged | Unchanged |
| Digit (in picker) | Unchanged | Unchanged — digit only moves the cursor, it does not commit |
| X / x (in picker) | Slot set to `""` | Bank saved |
| Enter (in picker) | Unchanged — picks the cursor into the editor | Unchanged until the operator commits from the editor |

The picker keeps its own cursor: `_memoryPickSlot` is reset to `-1`
when committing from or discarding the editor, and reset to `0` when
the operator re-opens the picker from DECODER. `X`/`x` clearing does
**not** reset the cursor — the operator stays on the same slot and
sees the red `(empty)` placeholder, ready to press `Enter` and type
fresh content.

#### 5.3.1 Held-Enter priming

When transitioning from `MEMORY_PICK` to `MEMORY_EDIT`, both
`TextInput::primePrintableHeld()` and `TextInput::primeEnterHeld()`
must be called after `setValue()`. Without the Enter prime, the still-
held Enter key would auto-commit on the editor's first `feed()` tick —
the operator would briefly transition into `MEMORY_EDIT` and then
immediately bounce back to `DECODER` without ever seeing the editor.
This mirrors the same priming pattern in the Wi-Fi password flow.

### 5.5 Caps lock

`OPT` toggles caps lock (mirrors `WIFI_PASSWORD_INPUT`). The reveal /
mask gesture from the password screen is intentionally absent — memory
text is never masked, so the `FN show` hint would be misleading.

### 5.6 Digits inside the editor

Digits `0`..`9` are typed as ordinary text inside `MEMORY_EDIT`. CW
macros routinely contain numbers (`5NN`, `599`, contest exchanges), so
the editor's `TextInput` accepts them straight from `feed()`. There
is no slot-pivot shortcut — switching slots requires `Esc` back to
`MEMORY_PICK` and a fresh digit press, the same back-and-re-pick flow
the Wi-Fi passphrase screen uses to abandon an entry.

---

## 6. Concurrency and cross-core notes

### 6.1 The seam being closed

`MorseGenerator` was the last producer of CW elements that did **not**
participate in the `KeyEventBus` contract. Paddle keying (IambicKeyer
and StraightKeyer) and the keyboard `K` hold-to-key all publish their
edges to the bus; `RadioKeyer` subscribes and gates on its own
`_enabled` flag. `MorseGenerator` previously drove the audio task
without touching the bus — so stored-text playback was the only
**CW-source** that could not key the radio.

Closing this seam was a deliberate scope-creep decision: pressing `P`
("Hello Morse!") now also keys the radio whenever `KEYING` is on. The
trade-off is documented in § 8.

### 6.2 Edge detection inside `MorseGenerator`

`MorseGenerator::advanceToNextElement()` runs on whichever core woke
first (the audio task on Core 0, the loop on Core 1 when an element
boundary is crossed). The new wiring adds:

```cpp
if      (_elKeyDown && !_wasElKeyDown) KeyEventBus::keyDown();
else if (!_elKeyDown &&  _wasElKeyDown) KeyEventBus::keyUp();
_wasElKeyDown = _elKeyDown;
```

`stop()` and the natural-playback-exhausted branch also emit a
matching `keyUp()` when the line was held, so the radio unkeys at
every termination path.

### 6.3 Cross-core invariant

The `_wasElKeyDown` plain `bool` is safe because the existing ordering
invariant — `_mode == PLAYING` is published only **after**
`advance()` completes — guarantees only one core advances the
generator at a time. The bus calls themselves are atomic via
`std::atomic` refcount dispatch. See the comment block at
`morse_generator.cpp` for the full invariant.

### 6.4 Cancel-gate cross-core safety

The two cancel gates in § 4.4 are independent and run on different
cores:

- **Keyboard gate** runs in `handleKeyboard()` on the main loop. It
  reads `gen->isPlaying()`, calls `gen->stop()`, writes `wasX = xKey`
  for every tracked key, and returns. `gen->stop()` writes to
  `MorseGenerator` state that the audio task also reads, so the
  ordering invariant from § 6.3 applies: a single atomic write that
  the audio task observes on its next `fillBuffer()` call. `MorseGenerator::stop()`
  itself is idempotent and is safe to call from any context — the
  survey confirmed no caller holds a lock that would deadlock.

- **Paddle gate** runs in `fillBuffer()` on the audio task (Core 1).
  It reads `MorseKey::isDitPressed()` / `isDahPressed()`, which read
  the same atomic `s_keyState.state[]` flags the paddle ISRs write —
  standard acquire/release semantics, no new atomicity concerns. The
  `s_paddleSuppressed` flag is a plain `bool` because it is read and
  written only on the audio task — no cross-core visibility needed.

If both gates fire on the same tick from different cores, both
`gen->stop()` calls happen. The first transitions the gen to `IDLE`
and issues `KeyEventBus::keyUp()` if mid-mark; the second sees the
gen already `IDLE` and returns as a no-op. The bus refcount
saturates-at-0 so an extra `keyUp` is harmless.

The `s_paddleSuppressed` flag is **not** visible to the keyboard
gate (it lives on the audio task and the keyboard handler is on the
main loop). This is intentional — the keyboard and paddle presses
are tracked independently so the operator can press a keyboard key
*and* a paddle at the same time, and both presses are consumed
without either one re-firing.

### 6.5 Test coverage

`test/test_morse_generator_bus` drives a known element sequence by
calling `advanceToNextElement()` directly (mirroring the
`test_iambic_keyer` style) and asserts the bus rises and falls cleanly
with no double-down across adjacent marks, exactly one `keyUp` on
natural playback completion, and exactly one `keyUp` on
`stop()`-mid-playback.

The cancel-gate logic in § 4.4 lives in `audio_engine.cpp` and
`main.cpp`, neither of which is part of the `UNIT_TEST` host build
(`audio_engine.cpp` depends on ESP-IDF I2S; `main.cpp` depends on
`M5Cardputer`). The behaviour is therefore covered by the on-device
smoke test in § 9.3, not by a host suite. The contract being relied
upon — that `MorseGenerator::stop()` is idempotent and unkeys the bus
correctly — is covered by `test_morse_generator` and
`test_morse_generator_bus`.

---

## 7. Files and module map

### 7.1 New files

| Path | Purpose |
| --- | --- |
| `src/memory_store.h` | NVS-backed persistent memory bank. Namespace `"memory"`, keys `"m0"`..`"m9"`. |
| `src/memory_store.cpp` | `memoryBankLoad`, `memoryBankSave`, `memoryBankClear`. |
| `test/test_memory_store/test_memory_store.cpp` | Round-trip, empty-NVS, truncation, special-character coverage, `memoryBankClear` namespace isolation. |
| `test/test_morse_generator_bus/test_morse_generator_bus.cpp` | Edge-detected bus wiring assertions. |
| `test/test_winkey_playback/test_winkey_playback.cpp` | `Winkey::playLocalMemoryText` stub + bridge multi-byte send test. |
| `docs/memory.md` | This document. |

### 7.2 Modified files

- `src/display_model.h` / `.cpp` — added `MEMORY_PICK` and `MEMORY_EDIT`
  to `DisplayScreen`; added `_memory[kMemSlots][kMemLen]`,
  `_memoryEditorBuf[kMemLen]`, `_memoryInput`, `_memoryEditingSlot`,
  `_memoryPickSlot`; accessors `memoryInput()`,
  `memoryClearEditor()`, `memoryEditingSlot()`,
  `setMemoryEditingSlot()`, `memoryPickSlot()`,
  `setMemoryPickSlot()`, `getMemory()`, `setMemory()`,
  `copyMemoryBank()`.
- `src/display_interface.h` — added `showMemoryPick` and
  `showMemoryEdit` pure virtuals.
- `src/cardputer_display.h` / `.cpp` — added the two new renderers and
  two new cases in `render()`.
- `src/main.cpp` — added `handleMemoryScreen()`, the M → `MEMORY_PICK`
  binding, the plain-digit playback branch, the memory-screen
  exemption on the universal Enter block, and the bank-load in
  `setup()`.
- `src/morse_generator.h` / `.cpp` — added `_wasElKeyDown`, the
  edge-detect block in `advanceToNextElement()`, and cleanup in
  `stop()` and the exhausted branch.
- `src/winkey.h` / `.cpp` — added `Winkey::playLocalMemoryText(const
  char*)` and its `UNIT_TEST` no-op stub.
- `CMakeLists.txt` — added `src/memory_store.cpp` to `LIB_SOURCES` and
  the three new suites to the `foreach(SUITE IN ITEMS …)` block.

### 7.3 Source mirrors

- `src/net_config.{h,cpp}` is the template for
  `src/memory_store.{h,cpp}` — same conventions for namespace keys,
  same NVS access pattern.
- `cardputer_display.cpp:517` (`showWifiPasswordInput`) is the template
  for `CardputerDisplay::showMemoryEdit` — same field box, same caret
  math, same hint row layout.
- `main.cpp:95` (`handleWifiScreen`) is the template for
  `main.cpp` `handleMemoryScreen` — same screen-local edge detection,
  same OPT caps-lock wiring, same Fn-gated `,` / `/` cursor nav.

---

## 8. Risks and trade-offs

1. **`MorseGenerator → KeyEventBus` scope creep.** Pressing `P`
   ("Hello Morse!") now also keys the radio whenever `KEYING` is on.
   The user explicitly opted in to global wiring. `RadioKeyer::_enabled`
   keeps the operator's toggle authoritative — `setOutputEnable` from
   the host still cannot re-enable RF the operator turned off.

2. **NVS storage wear.** `memoryBankSave` rewrites every slot every
   commit. Preferences is a flash-backed key-value store on ESP32;
   `putString` on an unchanged value is internally a no-op, so
   untouched rows cost nothing. Per-slot save could be added later
   if wear becomes a concern (a contest logger running ~100 saves per
   hour would still take years to hit ESP32 NVS endurance limits).

3. **Cross-core safety on the bus.** `advanceToNextElement` runs on
   either core; the bus calls are atomic; the `_wasElKeyDown` plain
   bool is safe because only one core advances the generator at a time
   (the `_mode == PLAYING` publish-after-advance invariant).
   `test_morse_generator_bus` exercises rapid fills to assert no drift.

4. **`M` key conflict.** `MODE_SETTINGS` previously used `M`. The new
   binding from DECODER opens `MEMORY_PICK` instead. `MODE_SETTINGS`
   becomes unreachable from the keyboard but stays in the enum and the
   renderer so the state machine remains self-consistent.

5. **Universal Enter block.** Added `MEMORY_PICK` and `MEMORY_EDIT` to
   the exemption list at `main.cpp` so Enter committed inside the
   editor reaches `handleMemoryScreen` instead of the universal
   dismissal path.

6. **Empty slot.** Pressing a digit for an empty slot is a no-op —
   `playLocalMemoryText` returns when `text[0] == '\0'`. No error
   beep, no click.

7. **Bridge state.** Whether the bridge is open or closed affects only
   whether the host sees per-byte echoes — local sidetone and radio
   keying work in either case.

8. **TextInput buffer ownership.** The editor is bound to
   `_memoryEditorBuf`, not to `_memory[slot]`. Edits only persist on
   Enter. ESC discards. This matches the Wi-Fi password pattern.

9. **Cancel-on-any-keypress "consume" semantics** (§ 4.4). The
   keyboard gate updates `wasX = xKey` for **every** tracked key on
   the consumed tick — a held key does not fire its handler on
   subsequent ticks. If a future change adds a new tracked key
   without including it in that update list, the new key would
   fire on the next tick (because `wasX` would be stale). The list
   at `main.cpp::handleKeyboard` must stay in sync with the static
   `wasX` declarations above it. The paddle gate's `s_paddleSuppressed`
   flag is a plain `bool` (not `std::atomic`) — only safe because the
   audio task is the sole reader and writer.

10. **Paddle suppression drops one element of "sensitivity".** While
    a paddle is suppressed, the operator's intentional squeeze on
    DIT+DAH is invisible to the iambic keyer. After release + a fresh
    press, the keyer works normally. This is the intended trade-off —
    the cancel gesture is a deliberate abort, not a CW character.

---

## 9. Verification

### 9.1 Host tests

```
cmake -B build
cmake --build build
ctest --test-dir build -V
```

Targeted reruns while iterating:

```
ctest --test-dir build -R test_memory_store       -V
ctest --test-dir build -R test_morse_generator    -V   # regression
ctest --test-dir build -R test_morse_generator_bus -V
ctest --test-dir build -R test_winkey_bridge      -V   # regression
ctest --test-dir build -R test_winkey_playback    -V
```

What to look for: every suite reports `0` failures.
`test_morse_generator_bus` must confirm no double-down across adjacent
marks, exactly one `keyUp` on playback completion, and exactly one
`keyUp` on `stop()`-mid-playback.

### 9.2 Device build

```
pio run -e esp32s3_cardputer
```

Clean compile expected. Cardputer is the only target with a keyboard;
Tab5 doesn't apply.

### 9.3 On-device smoke test

1. **Cold-boot with empty NVS:** powers up into DECODER with no error.
   Pressing each digit is silent.
2. **Edit a memory (default slot):** press `M` → picker opens with title
   `Memory 0` in accent, `(empty)` in red at size 3. Press `Enter`
   (no digit first). **Editor opens and stays open** — the held Enter
   must not auto-commit on entry (regression for the primeEnterHeld
   bug — see § 5.3.1). Type `CQ CQ DE W1AW K`. Press Enter. Returns to
   DECODER. Serial log shows `[MEM] saved m0="CQ CQ DE W1AW K"`.
3. **Re-edit a slot (pre-fill):** press `M` → picker opens with title
   `Memory 0`. Press `3` to switch the picker to slot 3 (no transition
   to the editor, title updates to `Memory 3`). Press `Enter`. Editor
   opens for slot 3, pre-fills with `CQ CQ DE W1AW K`, cursor at end.
   Press Enter without changes — same value resaved.
4. **Digit in picker only switches slot:** from `MEMORY_PICK` of slot 3,
   press `5`. Title updates to `Memory 5`; picker re-renders showing
   slot 5's contents (or `(empty)`). Stay on `MEMORY_PICK`. Press
   Enter — editor opens for slot 5, not slot 3.
5. **Clear a slot with X:** from `MEMORY_PICK` of a slot that has
   content (e.g. slot 3 with `CQ CQ DE W1AW K`), press `x`. Slot
   content is wiped, picker re-renders with `(empty)` in red, title
   stays `Memory 3`. Serial log shows `[MEM] cleared m3`. Press
   `Enter` — editor opens empty, ready for fresh content. Verify
   persistence with power-cycle.
6. **ESC aborts without saving:** press `M` → `3`, type something,
   ESC. Returns to DECODER. Slot 3 unchanged.
7. **Playback through sidetone + radio:** enable `KEYING` (`K` then
   `;` for On). Press digit `3` from DECODER. Hear the stored CQ
   through the speaker **and** see GPIO4 (EXT header G4) HIGH during
   each dit/dah on a scope/LED probe.
8. **Playback with host echo (no radio):** turn `KEYING` off, connect a
   host (RUMlogNG / N1MM / `wk2ping`), open the WinKey session. Press
   digit `3`. The host's outgoing-CW field populates byte-by-byte.
9. **Playback when busy:** press `3`, then immediately press `4` while
   memory 3 is still playing. The second press is ignored; memory 3
   continues uninterrupted.
10. **Empty slot is a no-op:** press a digit whose slot is empty.
    Nothing plays, no error.
11. **Arm timeout / ESC:** press `M`, then ESC. Returns to DECODER
    without entering the editor.
12. **P-key still works:** press `P` from DECODER. Hears "Hello
    Morse!" through the speaker. With `KEYING` enabled, GPIO4 also
    keys during each dit/dah (new behaviour, expected per § 8.1).
13. **Persistence:** power-cycle. Press `3` — stored CQ plays again.

14. **Cancel-on-any-keypress — paddle held suppresses keying:** press
    `3` to start memory 3 (GPIO4 toggling). Press the DIT paddle
    mid-element → playback stops immediately, sidetone cuts off, GPIO4
    returns LOW. **GPIO4 must stay LOW for the entire duration the
    paddle remains held.** No element completes after the cancel.
15. **Cancel-on-any-keypress — fresh press after release is real:**
    continue from #14 with paddle still held (GPIO4 LOW). Release the
    paddle → GPIO4 stays LOW (no spurious re-key on release). Press
    the DIT paddle again → a real dit plays (sidetone blip, GPIO4
    HIGH for one dit-length).
16. **Cancel-on-any-keypress — held P cancels itself:** press `P` to
    start "Hello Morse!". Hold `P` through playback → playback stops
    on the first held tick. Subsequent held ticks produce no
    restart. Release `P`, press `P` again → "Hello Morse!" plays once.
17. **Cancel-on-any-keypress — held W opens no overlay:** press `P` to
    play "Hello Morse!". Press `W` mid-playback → playback stops;
    `WPM_SETTINGS` does **not** appear. Release `W`, press `W` again →
    `WPM_SETTINGS` opens.
18. **Cancel-on-any-keypress — held digit does not start a memory:**
    press `P` to play "Hello Morse!". Press `2` mid-playback →
    playback stops; slot 2 does not start playing (the busy gate
    prevented the start; the cancel gate consumed the press).
    Release `2`, press `2` again → slot 2 plays.
19. **Cancel-on-any-keypress — squeeze suppressed until both released:**
    press `P` to play "Hello Morse!". Squeeze both DIT and DAH
    paddles → playback stops; GPIO4 LOW. Release DIT only, keep DAH
    held → GPIO4 still LOW. Release DAH → GPIO4 still LOW. Press DIT
    again → a real dit plays.

### 9.4 Regression checks

- WPM / FREQ / VOLUME / MODE / KEYING settings screens still toggle
  correctly. (Note: `MODE_SETTINGS` is no longer reachable from the
  keyboard — see § 8.4.)
- Wi-Fi screens (`C`, `N`) work; `W`, `F`, `V`, `M`, `K` no longer
  trigger on the password input screen.
- Existing `test_morse_generator` suite passes (the new wiring must
  not break any timing-sensitive behaviour).

---

## 10. Future work

- **Multi-tap macros.** `{NAME}` substitution from a separate `name`
  table; `%` for sequential contest exchanges.
- **Paddle recording.** Capture operator keying into a slot for replay
  — useful for ad-hoc CQ styles that don't fit the canned set.
- **Per-slot save.** Skip `Preferences::putString` for unchanged rows
  if NVS wear becomes a concern (see § 8.2).
- **MODE_SETTINGS recovery.** A new key (e.g. `Shift+M` or
  `Fn+PaddleType`) can restore the paddle / straight toggle if the
  operator needs it back.
