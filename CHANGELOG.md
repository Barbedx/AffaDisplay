# Changelog

Dates are the day the work landed. Anything marked **BREAKING** will not compile against the
previous version, which is deliberate — every one of them is a place where the old spelling
described the panel wrongly, and a silent behaviour change would have been worse.

---

## 2.0.0 — 2026-08-09

**BREAKING, and the theme is subtraction.** `src/` went from ~14 000 lines to 8 400, the
test suite from 7 976 to 1 934, the documentation from 8 176 to 2 297, and the repository
from 9.7 MB to 4.8. Nothing on the wire moved: every golden vector is byte-identical.

### The library is a transport, not a UI

`src/widget/` is gone — the menu state machine, the marquee, the three-row live screen — and
so are `MenuController`, `IPage`, `CarminatMenuRenderer` and the AMS key gesture that lived
in `UpdateListBase` and survived the first purge by not being in `widget/`.

> Which item is selected, what a long press means, how fast a title scrolls and when to
> repaint are decisions about a **product**. A CAN driver that makes them is a CAN driver you
> cannot use for a different product.

The render calls were always the panel's contract and they are unconditional now:
`showMenu`, `showMenuN`, `highlightItem`, `selectMenuItem`, `setText`, `showInfoMenu`.

**Gates deleted:** `AFFA_ENABLE_MENU`, `AFFA_ENABLE_MARQUEE`, `AFFA_ENABLE_ISOTP_RX`,
`AFFA_ENABLE_ESP32CAN_LINK`, `AFFA_PANEL_UPDATELIST_MENU`, `AFFA_MAX_SUBSCRIPTIONS`,
`AFFA_TASK_QUEUE_DEPTH`, `AFFA_MENU_MAX_*`. Each is still named in `AffaConfig.h` with what
it did and why it went.

### Every render is callable from any task

The cross-task boundary moved down into `enqueue()`, below every builder — so the guarantee
holds for renders written after it, not just the ones somebody remembered to mirror. The old
`rtos/AffaCommand.h` queue sat one layer too high and is deleted.

`AFFA_ENABLE_TASK` now defaults to **1** on ESP32. It was 0, and thirteen of nineteen
shipped examples turned it off and pumped `poll()` from `loop()` — including the one whose
HTTP handlers then raced the queue.

### One encoding for every UpdateList glass

`UpdateListMenuDisplay` is deleted. Byte `[2]` of the `0x121` text command is a command
**flavour**, not a panel selector: `0x76`, `0x7E` and `0x7F` have all been seen there, and
independent projects have driven both `0x76` and `0x7F` into the same family of display. Our
reconstruction of the `0x7F` form disagreed with the only real capture in two bytes and was
never transmitted from here. `docs/WIRE.md` §9.2 keeps the record.

`panelGeometry().mainChars` is 8 for this family — a **promise**, not a measurement: the
frame always carries 12 cells, so a wider glass shows more for free.

### Two protocol bugs, both found on the bench

**A session that died during `Registering` could never recover.** `needsHelloBeforeAuth`
tested `!atLeast(_phase, AwaitPeerChannel) || Failed`; neither is true at `Registering`, and
the void branch above needs `FuncsReg`, which a half-open session does not hold. Measured:
`RX 3CF 61 11 00` five times a second for eight minutes against `TX 151 70` every 2.5 s,
never acknowledged. Registration now gives up after one terminally failed burst and falls
back to the **announce** — pinned by
`test_unanswered_registration_falls_back_to_the_announce`, which fails without the fix.

**`-D AFFA_ENABLE_FULLSCREEN=0` did not link.** Its `#if` had grown to enclose the `BIGMENU`
and `NAV` blocks, so turning fullscreen off removed the definitions of `selectMenuItem`,
`showMenuN`, `showNavBitmap`, `showNavBitmapWithHeader` and `navTick` while the header went
on declaring them.

Also: `CarminatDisplay::supports()` read `AFFA_ENABLE_MENU != 0` — a *C++ expression*, so an
undefined macro is a hard error. The library compiled only because every env in this
repository still passed `-D AFFA_ENABLE_MENU=0`. **A consumer that did not would not build.**

### Documentation is generated or deleted

`docs/WIRE.md` is written by `tools/gen_wire_doc.js` from the golden vectors CI asserts —
103 frames, each tagged with the test that pins it, and the generator refuses to publish a
document it knows is short. Nine prose documents totalling ~8 000 lines were deleted; what
survives is `API.md` (contracts), `NOTES.md` (what we do not know, and how this project has
got things wrong eight times), `BENCH-VERIFIED.md` (what a human saw on glass) and
`ESP32CAN-CONTRACT.md`.

### What this release does NOT have

**The behavioural test suites are gone** — 165 tests across eleven files, kept only where
they asserted a golden wire vector. The §3b guarantees (one-poll key delivery, coalescing,
`abortPending`, `Priority::Urgent`) are implemented and documented but **no longer
enforced**. `docs/API.md` §3b.9 says so in those words.

Untested at all, and previously so: `ILogSink`, `shouldAutoAck()`, `PanelGeometry` for the
two working families, `stats()`, and `CanCommonLink` in its entirety — the real link. The
cluster family has never run against hardware and its opening cannot complete.

---

## 1.0.x — the work that became 2.0.0

Everything below landed before the version bump and is included in 2.0.0.

### The list screen has a pictogram and a scrollbar, and the library was hiding both

**Swept on the bench panel 2026-08-07.** Payload `[3]` and `[4]` of the `21 01` list screen
were hard-coded to `0x80 0x00` and unreachable from any caller, so neither capability
existed as far as this library was concerned.

**`[3]` is a glyph index** into a table the panel owns:

| value | on the glass |
|-------|--------------|
| `0x00`–`0x10` | blank |
| `0x11` | book with a magnifier |
| `0x19` | open book |
| `0x26` | bluetooth |
| `0x30` | GPS |
| `0x34` | aircraft |
| `0x3D` / `0x47` | the OEM Navigation and Settings glyphs |

with populated **uncatalogued runs between**. Bit 7 set draws nothing and the index survives
underneath, so `0x80` was never a "none" value — it is glyph `0`, blank twice over.

**`[4]` is the scrollbar thumb position** — `0x00` none, `0x10` top, `0x58` bottom. A
position, not a proportion, and independent of `[8]` (arrow mask) and `[35]` (first visible
item): three separate scrolling controls, none derived from the others.

**They were first read as one field.** That came from three OEM captures in which both bytes
co-varied; walking them independently took it apart in one bench session. Worth keeping as a
lesson — *three co-varying samples are not a field.*

### Added

* **`CarminatDisplay::showMenuIcon(header, row0, row1, scroll, icon, thumb)`** and
  **`showMenuN(..., icon, thumb)`**. Defaults are the bytes the builders always sent, so **no
  existing call changes a byte** — every capture-verbatim vector in `test_carminat_wire`
  passes unmodified. Five new tests, including one per field asserting it moves *its own byte
  and nothing else*, which is the check that would have caught the one-field misreading.
* **`carminat::kMenuIcon{None,First,BookGlass,BookOpen,Bluetooth,Gps,Plane,OemNav,OemSet}`**,
  **`kMenuIconSuppress`**, **`kMenuThumb{None,Min,Max}`**, and **`kMenuIconOemBlank`** — the
  builders' default, `0x80`, kept over the tidier-looking `0x00` because it is the captured
  byte and both are blank.
* **17_mediascreen: separate *Gutter glyph* and *Scrollbar thumb* cards** on the Menus tab —
  a dropdown of the named glyphs, ±1 stepping for the uncatalogued runs, a hide checkbox for
  bit 7, a slider with off/top/bottom shortcuts for the thumb, a live wire preview, and three
  explicit send buttons. **Nothing sends by itself:** an earlier version swept on
  `setInterval`, so the glass changed while you were still reading it.
* **17_mediascreen: `op=oem`**, the OEM Navigation menu replayed verbatim — 200 wire bytes
  from `mENU NAVIGATION MAIN SCREEN AFTER BACK.csv`. It was the control that separated "our
  builder is wrong" from "the panel needs more than one message", and it stays as the control
  for the next such question.
* **17_mediascreen: `op=wifi`** — writes the STA credentials to NVS `megaopen`/`ssid`/`pass`,
  the keys `startWifi()` already read. Moving the board between networks used to mean a
  reflash. A wrong password is not a brick: the join is given 15 s and then the SoftAP comes
  up with OTA on it. Reports the SSID only, never the stored password.
* **17_mediascreen: a `list + pane` button** — turns the `0x1F1` layer on, then draws the
  list on top of it.

### Fixed

* **17_mediascreen: the info rows repainted over every menu, and the gate was inverted.**
  The row tick was `rowsShouldTick() && !mainLineIsOurs()` — the rows woke up precisely when
  something *else* was on the glass. Draw a menu, and 700 ms later three `76 60 …` messages
  painted over it; the slot then read `InfoPopup`, still "not the main line", so it never
  stopped. The same sign error meant a row with scroll enabled never moved while the main
  line was up. Now gated on `rowsAreOurs()` — `None`/`Text`/`Clock`/`InfoPopup`, so a screen
  somebody opened is never repainted over. Measured: a two-row menu now sits for 8 s with
  **zero** further traffic on `0x151`.
* **17_mediascreen: `panic` left the rows repainting.** It cleared `rowslive` but not the
  per-row scroll flags, and *any* of those drives the tick. It clears them too now.
* **17_mediascreen: `/api/state` could not answer "is anything repainting right now".**
  `rowslive` read `false` through the entire session in which the rows were overwriting
  every menu, because a per-row flag drove the tick. New `rowstick` reports the tick itself,
  the console shows it, and a **stop repainting** button clears all four switches at once.
* **17_mediascreen: which commands reboot is a flag now, not a list of op names.** The list
  had already been forgotten once, and `op=wifi` can be asked *not* to reboot.

### Still open

The uncatalogued glyph runs between the named entries. `docs/captures/some more logs from origin/` §4 and
`docs/BENCH-VERIFIED.md`.

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
  keeps losing. It is a bench question, not an edit. `docs/captures/some more logs from origin/` §6.4.
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
