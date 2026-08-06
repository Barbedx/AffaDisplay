# The OEM bus-analyser corpus — `docs/captures/some more logs from origin/`

24 SavvyCAN CSVs of a **real Carminat radio driving a real AFFA3 NAV display**. Unlike the
ESP32 `.txt` captures these are lossless (a bus analyser, not `esp32_can` with
`rx_queue_len=6`), so every ISO-TP transfer in here reassembles with zero gaps.

Decode them with:

```
py tools/decode_oem_csv.py "docs/captures/some more logs from origin" [--payload-hex]
```

Four of the 24 (`aknowledge *.csv`) are already the corpus cited throughout
`docs/WIRE-SPEC.md`. **The other 20 are new**, and they are the first captures of the OEM
radio doing *application* work rather than opening a session.

---

## 1. Who is speaking

Only eight identifiers exist in the whole corpus, in four request/reply pairs. The reply id
is `req | 0x400` (`kReplyFlag`) on the three functional channels; the sync pair is its own
thing.

| id | direction | role | reply id | filler seen |
|----|-----------|------|----------|-------------|
| `0x3AF` | radio → display | sync: `B9` alive / `BA` request / `B0` hello | — | `0x00` |
| `0x3CF` | display → radio | sync: `69` ping / `61 11 xx` auth request | — | `0xA3` |
| `0x151` | radio → display | text, screens, menus, clock, power | `0x551` | `0x00` |
| `0x1F1` | radio → display | NAV — the 48×48 bitmap | `0x5F1` | `0x00` |
| `0x1C1` | **display → radio** | panel registration + panel status | `0x5C1` | `0xA3` |

**The filler byte is the sender's fingerprint and it is 100 % consistent across all 24
files**: `0x00` on every frame the radio originates (`3AF`, `151`, `1F1`, and the `5C1`
*ack*), `0xA3` on every frame the display originates (`3CF`, `1C1`, and the `551`/`5F1`
*acks*). This is the mechanical way to attribute any frame — never guess from content.

Note what falls out of that: the radio is not only an acker. It **originates** everything on
`0x151` and `0x1F1`. The display originates on `0x1C1` and acks on `0x551`/`0x5F1`. In this
corpus the display never sends a key frame (`03 89 …`) — no joystick was touched — but it
does register `0x1C1` and push two status messages, and the radio acks those on `0x5C1`.

### Acks

* `74 <filler>×7` — `kAckDone`, message accepted.
* `30 01 00 <filler>×5` — `kAckPartial`, ISO-TP flow control, **one per consecutive frame**.
  The display runs BS=1 on every transfer: 43 flow-control frames for the 44-frame `0x1F1`
  image, 35 for the 38-frame `setTime` sequence.

---

## 2. File classification

| # | file | class | what it captures |
|---|------|-------|------------------|
| 1 | `CONNECTION.csv` | **opening** | BA → `61 11 00` → 3×B0 → register → `52 09` power-on → 2× close. No payload. |
| 2 | `CONNECTION 2.csv` | **opening + 37 min soak** | same opening, then 4 416 `B9`/4 397 `69` with nothing else. The free-running-timer evidence. |
| 3 | `connect_new.csv` | opening | as #1 |
| 4 | `connect_new2.csv` | opening | as #1 |
| 5 | `aknowledge offed display.csv` | opening | as #1, display was off 85 s first |
| 6 | `aknowledge offed display 2.csv` | opening | as #1 |
| 7 | `aknowledge offed display cONNECT OT POWER.csv` | opening, unauthorized | **16× `61 11 01` at 104 ms** before the radio answers; then one `25 00 00 00` |
| 8 | `aknowledge on on display.csv` | **opening + full boot screen** | opening, then `setText` + the 302-byte `0x1F1` globe |
| 9 | `CONNECT DISPLAY TO CAN.csv` | **opening + full boot screen** | the cleanest one. 172 frames, everything |
| 10 | `CONNECT RADIO TO CAN.csv` | opening, retried | 586 `61 11 00` over 66 s, two registration attempts, then boot screen |
| 11 | `CONNECT RADIO TO CAN 2.csv` | opening + boot screen | as #9 |
| 12 | `setTime - after comfire.csv` | **application** | `56 "2020"` clock set → confirmation screen → Settings menu |
| 13 | `setTime - after comfire 1459.csv` | application | same, `56 "1559"` |
| 14 | `CONFIRM SCREEN NO.csv` | **application** | 2-button Yes/No box, "Delete destination memory?" |
| 15 | `CONFIRM SCREEN SWITCH TO YES.csv` | application | `29 05 00` — moves the confirm-box selection to button 0 |
| 16 | `AVAILABLE SPACE BIG SCREEN ONE CONFIRM.csv` | application | 1-button OK box, "Occupied: 3 / Avail.: 47" |
| 17 | `mENU NAVIGATION MAIN SCREEN AFTER BACK.csv` | **application** | a **6-item** menu in one 198-byte message |
| 18 | `MENU SAVED DEST MEMORIES 3 ITEMS SWITCH DOWN UP.csv` | application | 2-item menu + 3 `0x29` highlight moves, scroll masks `0x0B`/`0x07` |
| 19 | `MENU AUX EXPERT.csv` | application | 3 info rows (AUX/AF/SPEED) then `setText` |
| 20 | `AUX EXPERT MENU.csv` | application | same, shorter |
| 21 | `AUX EXPERT MENU TURN ON AUX.csv` | application | the same 3 rows sent 3× as AUX goes OFF → AUT → ON |
| 22 | `aux - aux switch source.csv` | application | 4 `setText` frames, source cycling to "AUX" |
| 23 | `nav still.csv` | **application, undocumented** | 20× `25 00 xx 00` alternating at 820 ms, nothing else |
| 24 | `navi start.csv` | **application, undocumented** | route-calc screen → `B3 52 …` → **two** `0x1F1` bitmaps 478 ms apart → 14× `25 00 xx 00` |

---

## 3. Message repertoire, with the library's name for it

All on `0x151` unless noted.

| wire | len | library | status |
|------|-----|---------|--------|
| `70` | 1 | `kRegisterByte` | ✅ implemented |
| `52 09 00` / `52 00 00` | 3 | `setPower(true/false)` | ✅ |
| `54 01` | 2 | `hidePopup()` | ✅ |
| `54 03` | 2 | `hidePopup()` | ✅ — `hideFullscreenText()` sent these same bytes and was removed 2026-08-06 |
| `56 "HHMM"` | 5 | `setTime()` | ✅ verbatim match |
| `77 <5 hdr> <8 cells>` | 14 | `setText()` | ✅ |
| `76 60 <row> <8 cells>` | 11 | `showInfoPopup()` | ✅ verbatim match |
| `21 01 …` windowed list | 90 / 144 / 198 | `showMenu()` | ⚠️ **2 rows only — OEM sends 2, 4 and 6** |
| `21 05 … 00 buttons` | 96–117 | `showFullscreenText()` / `showConfirmBox()` | ⚠️ **see §4** |
| `29 01 <rowtag> 80 00 00 00` | 7 | `highlightItem()` | ✅ |
| `29 05 <index>` | 3 | — | ❌ **confirm-box button select, not exposed** |
| `25 00 <00\|03> 00` | 4 | — | ❌ **undocumented** |
| `B3 52 00×16` | 18 | — | ❌ **undocumented** |
| `21 0B …` on `0x1F1` | 302 | `enqueueExternal()` in 16_navlab | ⚠️ header still partly unknown |
| `70` on `0x1C1` (panel) | 1 | auto-acked | ✅ |
| `64 0F` on `0x1C1` (panel) | 2 | — | ❌ constant `0x0F` in all 10 openings |
| `63 "NNNN"` on `0x1C1` (panel) | 5 | — | ❌ 4 ASCII digits, varies per session |

---

## 4. The `0x21` screen command, fully resolved

Offsets below are into the **reassembled payload** (the `10 LL` PCI stripped). The library's
builders count from the PCI, so library offset = these + 2.

### Mode `0x01` — windowed list

```
[0] 21                       command
[1] 01                       mode = list
[2] row tag of the selection (0x7E top / 0x7F bottom / 0x00 / 0x01)
[3] 0x80 or 0x3D or 0x47     ?
[4] 0x00 or 0x14 or 0x01     ?
[5] 00
[6] 0x80 | itemCount         <-- CONFIRMED across three menus
[7] FF
[8] scroll mask              0x00 none / 0x07 up / 0x0B down / 0x03 BOTH
[9 .. 34]   title, 26 bytes  (== kMenuHeaderMax)
[35]        index of the first visible item
[36]        item 0 tag    0x00..0x05 list index, or 0x7E/0x7F row tag in a 2-row window
[37 .. 62]  item 0 text, 26 bytes
[63]        item 1 tag
[64 .. 89]  item 1 text
...         27 bytes per item, repeating
```

`[6] = 0x80 | itemCount` is the finding that matters, and it is three-for-three, with the
total length falling straight out of `36 + 27 × itemCount`:

| capture | items | `[6]` | total len | `36 + 27N` |
|---------|-------|-------|-----------|------------|
| `MENU SAVED DEST MEMORIES` | 2 (FFFF, GROSS-ZIMMERN) | `0x82` | 90 | 90 ✓ |
| `setTime` → Settings | 4 (Time, Voice, Measuring unit, Back) | `0x84` | 144 | 144 ✓ |
| `mENU NAVIGATION` | 6 (Destination … Back) | `0x86` | 198 | 198 ✓ |

In `mENU NAVIGATION` the six tag bytes land at 36, 63, 90, 117, 144, 171 and read
`00 01 02 03 04 05` — an unambiguous stride of 27 with a 26-byte text cell. The library
hard-codes `0x82` and a 90-byte payload, so it can only ever draw two rows.
**The panel accepts at least six.**

`[35]` is what makes the 2-row window scroll: `MENU SAVED DEST MEMORIES` sends `[35]=0x01`
with (FFFF, GROSS-ZIMMERN) and `[35]=0x00` with (GROSS-ZIMMERN, HOME). The full-list menus
send `0x01`. The library's "row-0 index / row-1 index" bytes are this field and the item-0
tag; there is no second index byte — `WIRE-SPEC`'s "offsets 4, 38 and 65" are exactly the
selection tag, the item-0 tag and the item-1 tag.

### Mode `0x05` — fullscreen text / message box

```
[0] 21
[1] 05                       mode = fullscreen
[2] default/selected button  0xFF = no buttons, else the button index
[3] 0x00 or 0x01             ?
[4] button count             0, 1 or 2
[5] 49                       constant in all four OEM captures
[6 .. 31] zero
[32 ..]   buttonCount × 6-byte labels, then the body text, 0x0D-separated
```

Four independent confirmations of `[4] = button count`:

| capture | `[2] [3] [4]` | buttons | body starts |
|---------|---------------|---------|-------------|
| "New time settings are now effective" | `FF 00 00` | none | 0x20 |
| "The route is being calculated" | `FF 01 00` | none | 0x20 |
| "Occupied: 3 / Avail.: 47" | `00 00 01` | `OK` | 0x26 |
| "Delete destination memory?" | `01 00 02` | `Yes`, `No` | 0x2C |

**This is where the library is wrong.** `showConfirmBox()` emits `21 05 00 00 01 49` —
byte `[4] = 1`, i.e. it is a **one-button OK box**, not a Yes/No confirm. The real two-button
box is `21 05 01 00 02 49` with two 6-byte labels before the body, and the selection is moved
with `29 05 <index>`, not with `29 01 <rowtag>`.

Also: `showFullscreenText()` emits `[5] = 0x40`; every OEM mode-`0x05` frame uses `0x49`.

### Mode `0x0B` — on `0x1F1`, the 48×48 bitmap

`navi start.csv` gives the **second and third** ever seen, which is the first chance to
diff the header:

```
boot globe   21 0B 00 25 41 42 43 44 45 46 00 01 30 30   ("%ABCDEF")
route calc   21 0B 00 00 00 00 00 00 00 00 00 12 30 30   (blank)
```

Bytes `[3..9]` are a 7-byte ASCII field, blank in the nav frames. `[11]` moves `0x01` → `0x12`.
`[12..13] = 30 30` is constant, consistent with the "48,48" reading in
`docs/PROTOCOL-NOTES.md` §18. The two `navi start` images are 478 ms apart with different
pixel data — **the OEM animates this channel**, which is new.

---

## 5. Genuinely undocumented traffic

### `25 00 <00|03> 00` — the nav tick

Four bytes, on `0x151`, acked with a bare `74` (no flow control — it is a single frame).
`nav still.csv` sends 20 of them at **820 ms**, strictly alternating `00` / `03` in `[2]`,
with no other traffic on the bus. `navi start.csv` sends 14 more, starting the instant the
route-calculation screen goes up. One also appears in `aknowledge offed display cONNECT OT
POWER.csv` 7 s after power-on.

An alternating two-state value at a fixed sub-second cadence, on the nav screen, is a
**blink/animation tick** — almost certainly the flashing element of the nav display. It is
cheap and safe to try: 4 bytes, single frame, already acked by the panel.

### `B3 52 00 00 …` — 18 bytes, once

Sent 416 ms after the "route is being calculated" screen and 100 ms before the first
`0x1F1` bitmap. `0xB3` then `0x52` then sixteen zeros. One sample only; the zeros suggest a
route/heading block that had no data yet.

### `0x1C1` panel status — `64 0F` and `63 "NNNN"`

The panel pushes both during every opening, right after the radio's `52 09` power-on:

* `64 0F` — constant `0x0F` in all ten openings.
* `63` + four ASCII digits — varies per session: `0024`, `0031`, `0038`, `0040`, `0043`,
  `0255`, `0257`, `2111`, `2115`, `2117`. Monotonic within a capture session, which reads
  like a counter (power cycles or elapsed units), not a serial number.

`docs/WIRE-SPEC.md` already names these as the reason the `03 89` key guard is load-bearing.
They are not noise — they are the panel's status report, and nothing decodes them.

### `3CF 69 01`

`CONNECT DISPLAY TO CAN.csv` at 32.040 s: the display answers `BA` with `69 **01**` — a ping
with a non-zero `data[1]` — and only *then* sends `61 11 00`. Every other `69` in the corpus
is `69 00`. `AffaConstants.h` says "`data[0]` is the ENTIRE test", which stays correct as a
matching rule, but the byte is not always zero and it appears exactly at the BA boundary.

---

## 6. What the library could expose next

Ranked by evidence strength and cost:

1. ~~**`showMenu` with N rows.**~~ **DONE** — `showMenuN()`.
2. ~~**A real two-button confirm box**~~ **DONE, 2026-08-06** — `showMessageBox(row0, row1,
   labels, buttonCount, selected)` and `selectBoxButton(i)` sending `03 29 05 i`. Both
   length formulas below are pinned by `test_messageBox_lengths_follow_the_button_count`,
   and the two-button form's counter wrap by
   `test_messageBox_two_buttons_wraps_the_sequence_counter`. `AFFA_MAX_PAYLOAD` went 113 →
   119 to hold it; the old "113 is a validated wire limit" note was wrong on both counts.
3. ~~**`navTick(bool)`**~~ **DONE**.
4. **Fix `showFullscreenText` byte `[5]` to `0x49`** and let the payload length vary; the
   OEM uses 105 and 117 where the library is fixed at 96.
5. **Decode inbound `0x1C1` `63`/`64`** into an observer callback. Receive-side only, zero
   wire risk.
6. **`0x1F1` header `[3..9]` as a text field** in 16_navlab, and a two-frame animation to
   replay what `navi start.csv` does.

Everything above is byte-for-byte from a working OEM pair. The standing caution from
`12_ulclock` applies to all of it: **this panel ACKs everything**, so an ack is not proof
that anything reached the glass.
