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
| **Fullscreen OWNS the glass** | ✅ | `setText` after it delivered `Ok` and **changed nothing** until `hideFullscreenText()`. Recorded as `[CAP]` in WIRE-SPEC §8.6 |
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
