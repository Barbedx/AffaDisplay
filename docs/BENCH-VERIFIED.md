# What has actually been seen on a panel

Every other document here reasons from captures, from a reference implementation, or from
the code. This one records only what a human looked at on real glass, and — more usefully —
what has **not** been looked at.

Session of **2026-07-28**, ESP32-C3 bench rig at `192.168.100.85` with a universal Renault
panel. The panel is UNIVERSAL: attached to an UpdateList radio it behaves as an UpdateList
panel, so both families below were exercised on the *same* physical unit.

Legend: **✅ seen** on the glass · **⚠️ partial** — works but not as expected · **❌ tried,
did not work** · **⬜ never run**.

---

## Session of 2026-08-04, evening — AFFA2 / UpdateList, FIRST EVER CONTACT

**The UpdateList family had never put a frame on a bus.** Every byte it emits was pinned by
golden vectors extracted from a reference driver, and step 8 of `docs/REFACTOR-PLAN.md`
moved its *sequencing* onto the measured Carminat rules on the strength of an argument —
that the two originals were nearly the same code, and that the reference *worked* rather
than being *right*. This is that argument meeting glass.

`10_updatelist` on the ESP32 DevKit V1, CRX=GPIO5 / CTX=GPIO4. Full log at
`docs/captures/2026-08-04-updatelist-first-contact.log`. **It opened, registered, powered
and rendered on the first attempt, in 220 ms of wire time.**

| Capability | | Evidence |
|---|---|---|
| Panel answers `0x3DF` at all | ✅ | `RX 3CF 69` → our `TX 3DF 7A 01`, then `RX 3CF 61 11 00` |
| The **single** hello frame (not Carminat's three) | ✅ | `TX 3DF 70 1A 11 00 00 00 00 01`, 1 ms after the request |
| **Registration in the OPENING**, not lazily off a render | ✅ | `121 70` then `1B1 70` with no application involvement at all — the step-8 change that had no evidence behind it |
| **The peer-channel gate** | ✅ | the panel's own `70` probe arrived and unlocked ours; ACKed `521 74` / `5B1 74` |
| **The library powering the glass itself** | ✅ | `TX 1B1 04 52 02 FF FF`, ACKed, with nothing in the application asking for it |
| **`setText("SUCCESS")` — 4-frame ISO-TP, `76` 8-cell encoding** | ✅ | user: *"SUCCESS IS HERE"*. Every CF answered `30 01 00`, terminal `521 74` |
| `replyToPing = false` on this family | ✅ | the panel pings ~500 ms and is **never** ponged; the session held for 63 s+ regardless. This was the open question the plan called "the first knob to turn" — it did not need turning |
| Steady state | ✅ | 63 s, `rx 131 tx 72`, `txErr/rxErr/busErr` **0**, no drops, 1 Hz alive free-running |

### The panel registers TWO channels, and our gate survived on luck

```
[    2910] RX 1C1  70 ..     <- CARMINAT's key id
[    2921] TX 5C1  74 ..
[    2921] RX 0A9  70 ..     <- UpdateList's key id, 11 ms later
[    2931] TX 4A9  74 ..
```

`_peerChannelSeen` was set by the **`1C1`**, because it came first. The gate is id-agnostic
— any `70` on a frame we auto-ACK — and that is the only reason this worked. **Do not
"tighten" it to the family's own key id.** A future reader will see `0A9` in
`UpdateListConstants.h` and assume that is what the gate waits for; it is not, and on this
panel that assumption would have cost the whole session.

### What this run did NOT prove, listed so it is not assumed

| | Why not |
|---|---|
| That the burst answers the **first** request (`helloRequiresAnnounce = false`) | ⬜ Our announce went out at 2796 ms, *before* the request at 2899 ms, because the panel's `69` armed it. "Answers the first request" and "answers a request that follows our announce" both fit this log. It did not matter here; it is not settled either |
| The `61 11`-while-registered teardown | ⬜ never triggered — the panel did not deauthorize us in 63 s |
| The LCD `7F` text-plus-icons encoding | ⬜ `UpdateListMenuDisplay` untried; only the `76` form was rendered |
| `setTime` | ⬜ this family has no clock command at all — see below |

### The panel shows a clock we did not set, and we have two untried ways to set it

Observed on the glass beside `SUCCESS`: **`5:51`**. Nothing in this driver writes it, so it
is the panel's own free-running clock, counting from whenever the panel last had power —
the same behaviour seen on 2026-07-28, when a Carminat `10:00` free-ran to `10:11` eleven
minutes later. The panel keeps the time; it just has no idea what the time is.

`UpdateListConstants.h` has no clock command and that is correct — the reference driver has
none. But this is a **universal panel**, and there are two candidates, neither tried:

1. **`3EF A6 <hours> <minutes>`** — three bytes, DLC 3, **no PCI and no SF_DL**, so it does
   not go through the transport at all and must be sent with a raw `ICanLink::send()`.
   Transcribed from an OEM radio↔cluster capture (`ClusterConstants.h`, `PROTOCOL-NOTES.md`
   §9) and never put on a bus by this library.
2. **Carminat's `151 05 56 "HHMM"`** — which **this exact panel has already accepted**, on
   2026-07-28, read off the glass as `10:00`. It would mean adding `0x151` to the UpdateList
   function table so the `70` probe registers it, then sending the Carminat payload. Ugly,
   cross-family, and very likely to work precisely because the panel is universal.

(2) is the better first experiment: it is the only one with evidence behind it on this
physical unit.

---

## Session of 2026-08-04 — the captured opening, on a real Carminat

The first run against the **OEM-capture-derived** handshake (`docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md`),
06_authclock 0.6.0, ESP32-C3 with CTX=GPIO4 / CRX=GPIO3.

| Capability | | Evidence |
|---|---|---|
| Cold opening (our `BA` first, then the display's `61 11`) | ✅ | `BA -> 61 11 00 -> B0/B0/B0 -> 151 70 -> 1F1 70 -> FUNCSREG` in **194 ms**. The `00` here is incidental — `01` is the same request (`CARMINAT-HANDSHAKE-GROUND-TRUTH.md` §2.0) |
| `setTime` **read off the glass** | ✅ | user: *"I CAN SEE 10 00"* — `151 05 56 31 30 30 30`, ACKed on `0x551` |
| `setPower(true)` | ✅ | `151 03 52 09 00`, ACKed |
| Panel control ACK `1C1 -> 5C1 74` | ✅ | `autoAcks-TX+ 2`, emitted between B0 fragments |
| Steady state, 8+ minutes | ✅ | `pings 991`, `openings 1`, `TX+ 973`, **`TX- 0`**, `txErr/rxErr/busErr/busOff` all **0** |
| Clock free-runs after being set | ✅ | set `10:00`, read `10:11` eleven minutes later |

And then the same sequence **through the library** (`03_hello`, `src/` rather than the
example's own FSM) — the first hardware run since the handshake work:

| Capability | | Evidence |
|---|---|---|
| Library opening + `0x70` registration | ✅ | `AFFA: sync 0x01 -> 0x00`, `[seq] panel answering` |
| `setPower(true)` via `CarminatDisplay` | ✅ | `[seq] power acknowledged` |
| **`setText("SUCCESS")` — multi-frame ISO-TP** | ✅ | user: *"i see success"*. Three frames with `30 01 00` flow control between them, panel-ACKed |
| `setTime("1000")` via `CarminatDisplay` | ✅ | `[seq] clock set to 10:00 - done` |

That `setText` is the first proof that the **segmented** transmit path works against real
glass under the captured opening — a strictly harder path than the single-frame clock.

### `09_golden` soak — the whole stack, unattended

ESP32 DevKit V1 (CRX=GPIO5, CTX=GPIO4), the **library** over `can_common` via
`CanCommonLink`. Left running untouched and filmed by the owner.

| | measured |
|---|---|
| uptime | **5744 s** (1 h 36 m), no intervention |
| fullscreen screens delivered | **24 912**, `failed 0` |
| frames on the wire | ~350 000 — each screen is 14 ISO-TP frames, every one flow-controlled |
| `txErr` / `rxErr` / `busErr` / `arbLost` | **0 / 0 / 0 / 0** |
| `rxMissed` / `ringOverflow` / queue depth | **0 / 0 / rx 0 tx 0** |
| session | `sync 0x08 REGISTERED` throughout |

| Capability | | Evidence |
|---|---|---|
| Three independent rows, 220/380/550 ms | ✅ | filmed scrolling at their own rates |
| Pause / resume from the web console | ✅ | owner: *"pause play works as supposed"* |
| Recovery without intervention | ✅ | re-announces, re-registers, re-powers the glass and resumes painting on its own |
| Whole protocol behind the library API | ✅ | the example contains no handshake, no ISO-TP, no ACK handling |

**Nothing in that run is hand-written protocol.** The application calls
`showFullscreenText()` and reads status; `src/` does the rest. Compare `07_cantime` and
`08_rows3`, which implement the same wire by hand at ~700 lines each — and in which the
`0x21`-not-`0x20` consecutive-frame bug was made and found.

**A zero here is worth more than a success elsewhere.** `rxMissed 0` and `ringOverflow 0`
over 350 000 frames say the receive path kept up; `queued tx 0` says nothing ever backed up
behind a stalled transfer. Those three are the counters that were missing when this rig spent
a week looking like a wire fault.

**The transmit-failure signature that dogged this rig for a week was `frame.ss = 1`**
(single-shot). Same wiring, same session, only that flag changed: `TX+ 43 / TX- 334` and
`busErr 63 120` became `TX+ 973 / TX- 0` and `busErr 0`. See
[[affa-single-shot-was-the-tx-failure]] — a high `busErr` here was a symptom of the flag, not
a statement about the wire.

---

## Carminat / AFFA3

| Capability | | Evidence |
|---|---|---|
| Handshake, `FUNCSREG` | ✅ | `sync.state 0x0A`, `registered:true`, ACK mean **1 287 µs** |
| `setText` | ✅ | `AFFA OK`, `RENAULT`, `NO TWINS` read off the glass |
| `setTime` | ✅ | set to `10:00`; panel read `10:17` an hour later, free-running |
| `setPower(true/false)` | ✅ | `03 52 09` / `03 52 00`, delivered `Ok` |
| Menu widget | ✅ | `Main Menu` / `Bright: 50%` / `Mode: Auto`, `sel 0x7E` |
| Menu navigation | ✅ | next/prev/select/back, edit mode, 3-field item |
| Popup show / hide | ✅ | user reported seeing `POPUP 5` |
| Counter at 2 Hz | ✅ | `0035 → 0041 → 0047` |
| Media screen, 3 independent marquees | ✅ | *"both rows and time increments as supposed"* |
| Fullscreen `0x21` mode `0x05` | ✅ | animates at ~190 ms/screen, 14 frames |
| ~~**Fullscreen OWNS the glass**~~ | ❌ | **RETRACTED — this row contradicted itself and the code.** It records `setText` over a live fullscreen changing nothing until `hideFullscreenText()`; `CarminatDisplay::showFullscreenText`'s own comment records the opposite result from the **same day** — *"a plain setText() over a live fullscreen REPLACED it, with no hideFullscreenText() sent"* — and WIRE-SPEC §8.6 has been corrected to match. A fullscreen is **not** an overlay and needs no teardown: any full-screen-class render replaces the last one, which is what lets 09_golden animate at ~8 screens/second. The true overlay is the **popup**, which survives a redraw underneath it and is cleared only by `hidePopup()`. Whichever observation was mistaken, the two cannot both stand, and the animating build is the one with evidence. |
| 8.3-hour soak | ✅ | 257 k frames, **0** ring overflows, **0** controller errors, flat heap |
| Info popup (3 rows) | ⬜ | built and tested on the host, never confirmed on glass |
| Confirm box | ⬜ | host tests only |

## UpdateList / AFFA2

| Capability | | Evidence |
|---|---|---|
| Handshake, `FUNCSREG` | ✅ | `registered:true`; LCD ACKs `0x521` and `0x5B1` |
| `setPower` `1B1 04 52 02 FF FF` | ✅ | ACKed on `0x5B1` |
| **NORMAL** mode, `loc = 0x01` | ✅ | *"show normal works perfect"* — text plus the panel's clock box |
| **MENU** mode, `loc = LOCATION(n-1,idx)` | ✅ | *"shows perfectly"* — 3 rows, scrollbar |
| Menu selected row (`\| SELECTED`) | ✅ | *"selected row works as supposed"* — inverted row moves |
| Running text (marquee) | ✅ | `RUNNING TEXT` scrolling |
| **FULLSCREEN**, `loc \| 0x02` | ⚠️ | **ONE line of ~19–20 chars, not multiple rows.** The addressed chunks CONCATENATE into one text flow and truncate: 24 chars in → 19 out; 39 chars in → still 19, with or without spaces to wrap on. mhroczny's two-line photo is therefore probably *not* this mode |
| Icon bytes (`0x55` / `0xFF`) | ⬜ | three variants sent, no change observed and none expected to be visible on this unit |
| Menu row width | ⬜ | 12 cells sent; user suspects more is possible, untested |
| Clock via `3EF A6 hh mm` | ❌ | frame accepted by the bus, **clock did not change** |

### The clock, and why it is still open

UpdateList has **no `setTime` at all** — `supports(Feature::Time)` is false. The only
candidate found is the OEM cluster's `3EF A6 <hh> <mm>` (PROTOCOL-NOTES §9.4), a raw 3-byte
frame with no PCI, and **it does not work on this panel**.

**The oracle for any future attempt:** these panels *blink* the clock while it is unset and
count up from power-on; a clock that has been set stops blinking. So nobody needs to watch
the moment a command lands — a steady clock means something worked.
`examples/15_updatelist_modes` exposes `/api/sweep?n=1..8`, which sets **hour == candidate
number** so a stopped clock names the encoding that won. Candidate 1 is the `3EF` frame and
it failed; candidates 2–8 (BCD, swapped operands, Carminat's `05 56` shape on `0x121` and
`0x1B1`, DLC 8, `0x3DF`, command `0x26`) are **untried**.

## Dashboard cluster

| | | |
|---|---|---|
| Everything | ⬜ | `src/cluster/` is transcribed from ONE capture and has **never been run**. No cluster hardware was present. It renders no text because the capture contains no text frame |

`examples/18_cluster_web` is the firmware to flash the day a cluster is on the bench: it
brings the profile up, exposes the captured clock frame (`3EF A6 hh mm` — this is the family
it was captured on, unlike the UpdateList attempt above), offers the three known text
encodings on `0x121` as labelled `[GUESS]` probes, and puts the frame ring on `/api/frames`
so the answer comes from the wire rather than from a theory. Its siblings do the same two
operations on the families that *have* been seen: `16_carminat_web`, `17_updatelist_web`.

---

## Two traps this session paid for twice

**A delivered `Ok` is not a rendered screen.** It happened three times, each with a
different cause: rendering to a powered-off panel, rendering under a fullscreen that owns
the glass, and a query parameter silently defaulting because the endpoint took `c=` and the
caller sent `cmd=`. In all three the transport succeeded, the counters looked healthy, and
the glass did not move. Never treat an acceptance verdict as evidence of anything visible.

**Read the wire before theorising.** Every real diagnosis here came from the frame ring, and
every wrong theory came from reasoning without it. The `txgate=0` test (ESP32CAN-CONTRACT
§3.1) settles "is this fault ours" in about thirty seconds and should be the first thing run,
not the last.
