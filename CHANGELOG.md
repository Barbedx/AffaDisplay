# Changelog

Dates are the day the work landed. Anything marked **BREAKING** will not compile against the
previous version, which is deliberate — every one of them is a place where the old spelling
described the panel wrongly, and a silent behaviour change would have been worse.

---

## 1.0.0 — 2026-08-06

**Both panel families work on real glass, the protocol questions that blocked 1.0 are
settled, and three constants that had been wrong for months are corrected against the OEM
captures.** `docs/BENCH-VERIFIED.md` remains the honest record of what has been *seen* as
opposed to what the code believes.

### BREAKING

* **`hideFullscreenText()` removed** from `IDisplay`, `AffaDisplayBase`, `CarminatDisplay`
  and `AffaTask`. It emitted `02 54 03` — byte for byte what `hidePopup()` sends — so the
  library carried two names for one command, and the name implied a teardown the bench had
  already disproved. **A fullscreen is not an overlay: the next render replaces it.** Call
  `hidePopup()` if you want the raw close command, or just draw the next screen.
* **`hideInfoPopup()` removed.** Its body was `setText("RENAULT")` and its own comment
  admitted the real close command "has never been observed" — a guess wearing a protocol
  method's name. Call `setText()` yourself and see that it is a choice, not a dismissal.
* **`kScrollBoth` changed from `0x0C` to `0x03`** (`carminat::ScrollIndicator` and
  `widget::Scroll`). `0x0C` came from the origin's hand-written constant and appears in
  **zero** captures; the OEM sends `0x03` on its 4-item and 6-item lists, three times over.
  The high bits read as *suppressors* — `0x03|0x08 = 0x0B` at the top of a list, `0x03|0x04
  = 0x07` at the bottom — which makes `0x0C` "suppress both", the opposite of its name.
* **`AFFA_MAX_PAYLOAD` raised from 113 to 119**, and the `#error` threshold with it. The old
  note called 113 "a validated wire limit"; it was neither. `8 + 15*7` is only where the
  ISO-TP sequence counter first repeats, and this library wraps that counter on every nav
  bitmap — 24 912 transfers in one soak with `failed 0`. Costs 6 bytes per queue slot.

### Added

* **`CarminatDisplay::showMessageBox(row0, row1, labels, buttonCount, selected)`** — the
  mode `0x05` message box with its button count as a real field. `showConfirmBox()` had it
  hard-coded to 1, so the OEM's zero-button screen and its two-button Yes/No box were
  unreachable however you spelled the call. Five OEM captures give the layout and both
  formulas, three-for-three across 0, 1 and 2 buttons:
  `declared = 105 + 6*buttons`, `body = 32 + 6*buttons`, labels six bytes NUL-padded at 32.
  **Confirmed on hardware 2026-08-06**: 119 bytes, 17 frames, counter wrapping `2F → 20`,
  ACKed by the panel. That screen had never put a frame on a bus before.
* **`CarminatDisplay::selectBoxButton(index)`** — `03 29 05 <n>`, a three-byte single frame.
  Not to be confused with `highlightItem()`, which is the two-row list's `29 01 <rowtag>`.
* **`setTextStyled(..., iconBank2)`** — payload byte 4, previously hard-coded and
  unreachable. Exposed so it can be swept rather than guessed at; note that the OEM sends
  `0x55` there in 13 of 13 captured frames, so it is a constant, not a second mask.
* **Named icon bits** — `kIconNoNews`, `kIconNoTraffic`, `kIconNoAfRds`, `kIconNoMode`, the
  two arrow bits, and `kIconBit7Unknown`. **The polarity is inverted: a set bit turns an
  icon OFF**, which is why "no icons" is `0x55` rather than `0x00`. Confirmed on the wire —
  bit 4 tracks the band across FM, MW, LW and AUX.
* **`showMenuN()` takes a scroll mask.** A two-row `showMenu` draws no arrows whatever this
  byte says, because a 2-item window has no overflow; the N-item list is where it is visible.

### Fixed

* **`17_mediascreen`: SET TEXT reported success and sent nothing.** The repaint gate that
  stops a scroll *tick* from painting over a screen you are reading was also swallowing the
  deliberate press, for as long as any screen was up — which, with the hold defaulting to
  never, meant for ever. An explicit SET TEXT now takes the line back immediately.
* **`17_mediascreen`: the panel-family switch never applied.** The env built Carminat only,
  so boot reset the stored choice every time and the console reported success regardless.
  Both families are compiled in now.
* **`17_mediascreen`:** image buttons put the image on the glass like the animation buttons
  beside them; scrolling an info row implies repainting it; the Wire tab filters on ids and
  byte 0 learned from the ring; the duplicate "Info popup" section is gone, because
  `showInfoPopup()` *is* `showInfoMenu()` with the OEM's default offsets.
* **`showConfirmBox()` truncates its caption at six bytes**, the size of the label field it
  lands in. A seventh character used to run into the body's first byte.

### Known and deliberately unchanged

* **`showFullscreenText()` sends byte `[5] = 0x40` where all five OEM mode-`0x05` frames
  carry `0x49`, and declares 96 bytes where the OEM declares 105.** Real, and left alone: it
  is the most exercised path in the library — 09_golden has put 24 912 of these on the glass
  — and changing it on a byte diff without watching the result is the trade this project
  keeps losing. It is a bench question, not an edit. `docs/OEM-CSV-CORPUS.md` §6.4.
* **Whether the message box's buttons are DRAWN has not been looked at.** The bytes match
  the OEM's and the panel ACKed them — and this panel ACKs everything, which is how twenty
  useless clock probes once looked like twenty discoveries.
* **The ~7-minute session drop** still has no mechanism. It self-heals and now reports
  itself with a named cause; it is backlogged, not fixed.

---

## 0.5.0 — 2026-08-04

* The nine-step refactor completed: one `Phase`-driven state machine, one writer, shared by
  both families. `Phase::Ready` means **the glass is on**, not merely that we registered —
  the library sends the family's power-on itself and waits for the ACK.
* **UpdateList (AFFA2) rendered on real hardware for the first time**, on the same universal
  bench panel as Carminat: opened to `SUCCESS` in 220 ms of wire time, first attempt.
* **BREAKING:** `SyncProfile` lost six fields. Every one encoded a fact the captures have
  since settled, and each is now a rule in `AffaDisplayBase` — see `AffaSyncProfile.h` for
  which fact went where.
* `setAutoPower()`, `lastRendered()`, `queued()`, and the `TxDisposition` three-valued
  transmit verdict.
