# Changelog

Dates are the day the work landed. Anything marked **BREAKING** will not compile against the
previous version, which is deliberate — every one of them is a place where the old spelling
described the panel wrongly, and a silent behaviour change would have been worse.

---

## 2.0.0 — 2026-10-21

The first public release, in a new repository. It contains the 2.0 API rewrite listed below
(tagged 2.0.0 on 2026-08-09 in the old, now deleted repository, and never published) plus the
work in this section.

**BREAKING.** The one consumer (MegaOpen) ran on 1.0.0 plus two fixes that never reached
`main`; this brings them in, moves MegaOpen's transport into the library, and removes what
the library carried for a CAN stack it no longer uses. Nothing on the wire moved except the
two registration fixes below.

### A panel that never opens its `1C1` channel now registers

The Megane II panel in the car never sends `1C1 70`; the whole bus carries one id. The
unbounded wait for it held the session in `AwaitPeerChannel` for the whole power cycle.
`peerChannelReady()` still prefers the measured ordering but fails open after
`AFFA_AWAIT_PEER_MS` (400), armed once per session; a `61 11` repeated before FuncsReg draws
one more B0 burst, paced by `AFFA_HELLO_MIN_MS`. Ported from the 1.0-based branch
`carminat-peerchannel-probe` (`cad22f6`, `2e40bb9`), with two host tests that fail without it.

### `poll()` takes at most `AFFA_RX_BUDGET` frames

64 by default. A panel nobody answers retransmits at up to 10 700 frames/s; draining to empty
never returned to the sync pump, so the answer never left. The bound is in the core, not in
each link.

### `TwaiLink` replaces `CanCommonLink` — **BREAKING**

`CanCommonLink` and its `collin80/can_common` + `esp32_can` dependency are deleted: that
driver's RX queue of 6 lost every frame of a retransmit storm (MegaOpen bench: 0 frames and
43 806 bus errors in 30 s, against 115 623 frames and 0 errors on raw TWAI with a 64-deep
queue). `affa::TwaiLink` is MegaOpen's link, moved in: the IDF TWAI driver, `TwaiConfig`
(pins, bitrate, listen-only, queue depths, CRX pull-up), `Busy` for a full TX queue and
`Rejected` for a controller that cannot send, bus-off recovery, `abortTx()`, `waitRx()` for an
owner task's sleep, a raw tap, and `driver()` counters.

`affa::TxGate` (also from MegaOpen, with its 15 tests) shuts transmit when nobody ACKs and
re-probes with a backoff; `TwaiLink` feeds it from the new `ICanLink::service()`, which
`poll()` calls once per pass.

- `AFFA_ENABLE_CANCOMMON_LINK` and `AFFA_RX_RING_DEPTH` are gone. `TwaiLink` has no gate: it
  compiles wherever `<driver/twai.h>` exists.
- `ICanLink` is `trySend()` only; the legacy `bool send()` is gone.

### Long payloads are copied, never borrowed — **BREAKING**

`enqueueExternal()` and `enqueueSplit()` (and so `showNavBitmap()`) stored the caller's
pointer, which then had to outlive the ticket — across a task boundary, too. They now copy into
`AFFA_BULK_SLOTS` (2) library-owned slots of `AFFA_MAX_EXTERNAL_PAYLOAD` (now 320) bytes inside
the display object; a slot comes back when its job leaves the queue, and a render with every
slot in flight is refused `QueueFull`. No heap.

- `showMenuN()` builds in a slot and **no longer takes `scratch, cap`**.
- `busy()` now also counts renders posted from another task and not yet admitted.
- `pressKey()`, `abortPending()`, `abortAll()` and `begin()` called off the owner task ran right
  there, racing `poll()`; their dispatch ops existed but nothing posted them. They are posted now;
  the aborts return 0/false and their drops complete `Aborted` through `onComplete`.
- **`pressKey()` returns `Submitted`**, like a render: on the owner a refusal is the result and
  an accepted press completes `Ok`; posted from another task, the owner's verdict (including
  `NoSync`/`LinkDown`/`SendFailed`/`NotSupported`) arrives through `onComplete` under its ticket.
- The cross-task path has tests at last: `test_dispatch`, including three producer threads
  against an owner thread with every ticket getting exactly one verdict.

### Smaller, all BREAKING for anyone who used them

- `Result::Aborted` is 11. Log `resultName()`, not the number.
- `NavCommand` is gone (its `nav()` went in 2.0); so is `AFFA_TASK_ARG_MAX`.
- `AffaConfig.h` lists only gates that exist; the removed ones are here, not there.

### The link without a panel, and an owner that wakes on a frame

- The "no panel selected" `#error` moved from `AffaConfig.h` to `AffaDisplay.h`. A build that
  only wants the CAN link includes the new `<AffaTwai.h>` and names no panel — a listen-only
  sniffer, `examples/04_sniffer`, whose env carries no panel flag at all.
- `affa::setLogSink(ILogSink*)` (util/AffaLog.h, exported by `<AffaTwai.h>`): the log sink is the
  library's, so a link-only build sets it without a display.
- `ICanLink::waitRx(ms)` → `RxWait{Unsupported, Ready, Timeout}`, default Unsupported;
  `TwaiLink::waitRx` blocks in `twai_receive` and now returns it; `AffaDisplayBase::waitRx`
  forwards. `AffaTask` sleeps there instead of a fixed 2 ms, so a key frame wakes the owner
  at once; `WakePacer` (host-tested) yields a tick on every second back-to-back early wake so a
  storm cannot hold the task awake.

### Repository

The bench history moved out of the library into the maintainer's bench archive, which is not
published here: `HANDOFF.md`, `Hardware.md`,
`docs/captures/`, `docs/BENCH-VERIFIED.md`, `docs/ESP32CAN-CONTRACT.md`, `examples/19_cantest`,
the footprint harness and the capture tools. Code that cites a capture says
`archive/bench-notes:docs/captures/…`, a path in that archive.

GitHub Actions: `ci.yml` runs the host tests, builds every example and fails on a warning
from `src/` or `examples/`; it also checks that `docs/WIRE.md` is current, that the example web
pages parse, and that no log argument has a side effect. `release.yml` runs on a `vX.Y.Z` tag
that matches `library.json` and `library.properties`, and publishes this section as the release
notes. README and `docs/API.md` were cut to the facts.

### Known gaps

- UpdateList still uses its older startup contract (lazy registration, no auth gate); moving it
  onto the measured opening Carminat uses is planned, not done.
- This tree has not driven a panel yet: `TwaiLink`, the pool and the bounded `1C1` wait on this
  base are host-tested and compiled, not seen on glass.
- `AffaTask` has no after-poll hook, so an owner that does its own work after every `poll()`
  (MegaOpen) keeps its own loop, sleeping the same way: `waitRx()` + `WakePacer`.

### Migrating MegaOpen

| Was | Now |
| --- | --- |
| render returns `Result`, ticket from `lastEnqueued()` | `Submitted{ticket, result}` from the call |
| `UpdateListMenuDisplay` | `UpdateListDisplay` |
| own `TwaiAffaLink` + RX budget + `TxGate` | `affa::TwaiLink` (or keep your own `ICanLink`, now `trySend()` only) |
| `PaneBuffers` lend/release around `showNavBitmap` | nothing: the image is copied |
| `-D AFFA_ENABLE_CANCOMMON_LINK`, `AFFA_PANEL_UPDATELIST_MENU`, `AFFA_ENABLE_MENU` | delete |

---

## 2.0 API rewrite — 2026-08-09 (part of 2.0.0)

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
they asserted a golden wire vector. The ordering guarantees (one-poll key delivery, coalescing,
`abortPending`, `Priority::Urgent`) are implemented and documented but **no longer
enforced**. `docs/API.md` §3.4 lists what is tested.

Untested at all, and previously so: `ILogSink`, `shouldAutoAck()`, `PanelGeometry` for the
two working families, `stats()`, and `CanCommonLink` in its entirety — the real link. The
cluster family has never run against hardware and its opening cannot complete.

---

## 1.0.x — the work that became 2.0.0

Everything below landed before the 2.0 rewrite and is included in 2.0.0.

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
