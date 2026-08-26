# Wabun code support

A1Keyer supports **Wabun code** (和文モールス符号) — Morse code for Japanese
kana — alongside the default International Morse Code. This document explains
what Wabun is, how the firmware implements it, and what to expect on the
Cardputer and on the web UI.

---

## 1. What is Wabun?

Wabun code (sometimes called **Kana code**) is a non-Latin Morse code in
which each symbol represents a Japanese *kana* rather than a Latin letter.
It is the official telegraph code defined for Japanese in the Japanese
government's radio regulations and standardised by the ITU.

Source: https://en.wikipedia.org/wiki/Wabun_code
(Donald Millikin, *The Japanese Morse Telegraph Code*, QST, September 1942;
ITU-R Recommendation M.1677-1, October 2009.)

Key properties:

- The code is **not optimised for letter frequency** like International
  Morse — it is taken from the 1854 *DÖTV* (German-Austrian Telegraph
  Union) Latin Morse alphabet with the Iroha-ordered kana slotted in. In
  Wabun, a single dit means **ヘ (he)** and a single dah means **ム (mu)**.
- The character set is the 48 *gojūon* monographs plus digraphs (拗音),
  the *n* mora, the obsolete *wi* / *we*, the dakuten and handakuten
  diacritics (used to modify the previous kana: *ga*, *pa*, …), the
  *chōonpu* long-vowel mark and the punctuation 、 。 ( ) ".
- Because it is mixed with International Morse in actual operation, the
  prosigns **DO** (–··–··) and **SN** (·–·) are sent at the start/end of a
  Wabun run to tell the receiver to switch alphabets. A1Keyer does not
  currently emit DO/SN on playback — the user is responsible for the
  switchover, mirroring what a real Wabun operator does at the start of
  a contact.

Wabun is the only Japanese Morse variant in widespread use; "American
Morse" is unrelated.

---

## 2. How A1Keyer implements it

### 2.1 Three modes

A new `MorseTableMode` selector on the device cycles through three values
(mirroring the existing WPM / freq / vol / polar / keying settings):

| Mode              | Description                                        | Label on screen |
|-------------------|----------------------------------------------------|-----------------|
| `INTERNATIONAL`   | Default A-Z, 0-9, punctuation, prosigns (default) | `Intl`          |
| `WABUN_KATAKANA`  | Wabun code; decoded/played text renders in Katakana| `Wabun ｶﾀ�ﾅ`    |
| `WABUN_HIRAGANA`  | Wabun code; decoded/played text renders in Hiragana| `Wabun ひらがな`|

Internally, *all three* modes use the same dit-dah table for the Wabun
characters — only the **glyph** shown on the display differs (Katakana
or Hiragana). The choice between Katakana and Hiragana only changes how
text is rendered; the on-the-air encoding is identical.

### 2.2 Wabun code table

The complete Wabun table is implemented in `src/MorseTable.cpp` as a
single `MorseTable` with the Iroha-ordered monographs and digraphs
defined as their respective Katakana codepoints. A parallel lookup
table maps every Katakana codepoint to its Hiragana equivalent
(`ウ` ↔ `う`, `ガ` ↔ `が`, …), so the decoder can re-render a single
buffered character in either script with no extra work.

For reference, the supported characters are:

| Group       | Example codes                                  |
|-------------|------------------------------------------------|
| Monographs  | ア `(--·-·)`  イ `(·--)`  ウ `(··-)`  …        |
| Digraphs    | キャ `(-·-··  ·-··)`  キュ `(-·-·· --·-·)` …  |
| Dakuten     | ガ `(·-· ·  ·)`  (modifier on previous kana)  |
| Handakuten  | パ `(--- ·· ·--· ·)`  (modifier on prev kana)  |
| Chōonpu     | ー `(·-·· --·-)`  (long vowel)                 |
| Punctuation | 、 `(·-·· -·-·)` 。 `(·-·· -·- ·)` ( ) "      |

The digraphs and dakuten/handakuten variants are stored as **single
multi-element entries** (their dit-dah string contains the modifier
followed by the base kana), so a Wabun encoder call returns one
multi-element Element vector for "キャ" rather than three separate
characters. The decoder reverses this by exact-maching the assembled
dit-dah string against the table.

### 2.3 What the user sees

- **Cardputer LCD (240 × 135)** — the decoded text and memory previews
  render in a Japanese font (see § 4 below). The settings screen
  labelled `WABUN` shows three big options in the appropriate script
  (Katakana or Hiragana) with the current selection highlighted.
- **Web UI** — a "Wabun mode" dropdown is added to the Settings card
  with three options: `International`, `Wabun (Katakana)`, and
  `Wabun (Hiragana)`. The on-device mode is the source of truth; the
  dropdown reflects it on every 500 ms /state poll.

### 2.4 What the user cannot do (yet)

- **No Japanese text entry on the Cardputer keyboard.** The Cardputer
  keyboard has no IME, no kana input, and only basic Latin / symbol
  glyphs; an on-device editor for a Wabun memory slot would require a
  full Japanese IME (likely a romaji-kana converter) which is out of
  scope for this release. The web UI's memory editors *do* accept
  UTF-8 Japanese text (browsers have IMEs), so operators can
  populate CW macros in Japanese through the web interface today.
- **No DO / SN prosign emission.** When A1Keyer plays back a Wabun
  macro from NVS, it does not prefix `DO` or suffix `SN`. Add those by
  hand if you are interleaving with International Morse on the air.

---

## 3. Keyboard / interaction model

The new `J` key opens the **Wabun settings** screen. `J` and `j` both
work (matching the convention used for `W`, `F`, `V`, `M`, `K`, `D`, `C`,
`N`, `A`, `S`).

In the `WABUN_SETTINGS` screen:

| Key                | Action                                          |
|--------------------|-------------------------------------------------|
| `;`                | Cycle **forward**: Intl → Katakana → Hiragana → Intl |
| `.`                | Cycle **backward**: Intl → Hiragana → Katakana → Intl |
| **Enter**          | Save the current selection to NVS and dismiss   |
| **Esc**            | Discard any change and dismiss                  |
| **Button A**       | Same as Enter                                   |

The status line shows the active mode as a short tag so the operator can
tell at a glance whether the device is in International, Katakana, or
Hiragana mode without opening the settings screen — useful when the
device boots and the operator picks up the paddle.

Playback rules — unchanged from International mode:

- WPM and tone frequency continue to apply to Wabun characters; timing
  is identical (one dit = three units of intra-character space) because
  Wabun uses the *same dit/dah lengths* as International.
- The `M` → digit `0..9` memory picker plays the stored text via
  `Winkey::playLocalMemoryText()` regardless of mode. If the slot
  contains Japanese characters and the device is in Wabun mode, the
  text is encoded through the Wabun table; if it contains Latin
  characters, the International table is used. The two tables do not
  share symbols, so a mixed macro plays as two "words" separated by an
  inter-character gap (the encoder silently skips characters it cannot
  encode in the current table).

---

## 4. Font installation on the Cardputer

A1Keyer relies on **M5GFX**'s built-in Japanese IPA font (Gothic 12 pt)
for the Cardputer display. The font is already shipped with the
`M5GFX` library (the dependency is already declared in
`platformio.ini`), so no extra installation is required.

### 4.1 What the firmware does

When the Wabun mode is active, the main decoded-text renderer
(`CardputerDisplay::renderScrollingText`) calls
`M5.Display.setFont(&fonts::lgfxJapanGothic_12)` instead of the
default `&fonts::FreeMono24pt7b`. Because `setFont()` selects a
single font table at a time, and the build is linked with
`-ffunction-sections -fdata-sections` (see `platformio.ini`), the
linker only pulls the `lgfxJapanGothic_12` glyph table — the
other 35 sizes in the IPA family stay out of the firmware binary.

The Gothic 12 pt table is ~109 KB of PROGMEM. A1Keyer's typical
firmware size budget is ~1.5–2 MB; adding 109 KB is acceptable on
the Cardputer ADV's 8 MB flash.

### 4.2 Font license

The IPA Gothic font in M5GFX is licensed under the
**IPA Font License Agreement v1.0** (https://moji.or.jp/ipafont/).
The licence permits free use, modification, and redistribution
bundled with the firmware, as long as the licence text is shipped
along with the font. M5GFX already ships the licence file in
`src/lgfx/Fonts/IPA/IPA_Font_License_Agreement_v1.0.txt`; that file
is part of the build artifact via the library's distribution, so no
additional licence file needs to be added to A1Keyer.

### 4.3 Building it yourself

Nothing extra to do — the dependency on `M5GFX` already declares the
IPA font directory, so a normal `pio run -e esp32s3_cardputer`
compiles the font into the firmware whenever the renderer references
it.

### 4.4 Why only one font size?

The IPA Gothic 12 pt at native size fits ~20 characters across a
240 px row, which keeps at least one full Japanese sentence visible
on the decoder screen. Larger sizes (16, 20, 24 …) would shrink the
visible window to single characters, which is too narrow to scan
during a QSO. The smaller 8 pt is offered in M5GFX but is
visually uncomfortable on a 240×135 LCD at arm's length.

If a future build wants the larger font, add a second `setFont()`
call in `renderScrollingText()` and reference the appropriate
`lgfxJapanGothic_NN` symbol — the linker will pick up the new size
and drop any other.

---

## 5. State, web UI, and persistence

The active mode lives in `MorseModel::morseTableMode()` and is
persisted to NVS under the `morse` namespace, key `wabun`:

- `0` → International (default)
- `1` → Wabun, Katakana
- `2` → Wabun, Hiragana

`/api/settings` accepts a new `"wabunMode"` integer field, validated
to the [0..2] range, and writes both the model and the NVS key. The
`/state` snapshot adds a `"wabunMode"` integer so the web UI can
mirror the device mode without an extra round-trip.

The web UI's `Settings` card adds a `<select id="wabunMode">` with
three options:

```
International
Wabun (Katakana)  — 和文モールス (ｶﾀｶﾅ)
Wabun (Hiragana)  — 和文モールス (ひらがな)
```

On the Cardputer the same mode is shown in the status line as
`Intl` / `和文ｶ` / `和文ひ` (3-char tags so they fit beside the
existing `Paddle / Straight / WK / DBG` tags).

---

## 6. Tests

Two new unit tests live alongside the existing suites:

- `test/test_morse_encoder/test_wabun_encoder.cpp` — round-trip encode
  for representative Katakana characters (`ア`, `イ`, `ウ`, `キ`, `ク`,
  `キャ`, `ガ`, `パ`, `、`, `。`), checking that the encoder emits the
  expected dit-dah sequence.
- `test/test_morse_decoder/test_wabun_decoder.cpp` — round-trip decode
  for the same set of characters.

Both tests reuse the existing `MorseEncoder::setTable()` / `currentTable()`
API; they don't touch the keyboard or display layers, so they run on
the host under `run_tests.sh` without any Cardputer-specific setup.

---

## 7. References

- https://en.wikipedia.org/wiki/Wabun_code — overview, history, full
  code chart in Iroha order.
- ITU-R Recommendation M.1677-1 (October 2009) — *International Morse
  Code*. (Wabun is referenced in § 3 of the recommendation as a
  non-Latin extension used in Japanese.)
- Japanese Government Radio Station Operation Regulations, Article 12,
  Attached Table No. 1 — the binding domestic table for Japanese
  operators; cited from the Wikipedia article.
- IPA Font License Agreement v1.0 — the licence that ships the
  Japanese font inside M5GFX.
