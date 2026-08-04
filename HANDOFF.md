# Handoff — 2026-08-04, end of the day the protocol was settled

The previous handoff (2026-07-29) described a bench that could decode the panel but never
establish a link. **That problem is solved and the whole protocol is now proven on glass.** It
is archived at `docs/HANDOFF-2026-07-29.md`; almost every open question in it is answered here.

Next session's job is a **refactor**, fully planned in
[`docs/REFACTOR-PLAN.md`](docs/REFACTOR-PLAN.md). Read that file second. Read this one first.

---

## Where things actually are

**It works.** `examples/09_golden` on an ESP32 DevKit V1 ran unattended for 1 h 36 m: 24 912
fullscreen transfers, `failed 0`, and `txErr / rxErr / busErr / arbLost / rxMissed /
ringOverflow` **all zero**. Three rows scroll at independent speeds; pause/resume, per-row text
and speed, clock entry, wire-log download, WiFi setup and OTA all work from the web console.

| | |
|---|---|
| board | ESP32 DevKit V1, **CRX → GPIO5, CTX → GPIO4**, 500 kbit/s |
| flash | `pio run -e ex09_golden -t upload --upload-port COM5` |
| console | `http://192.168.100.97/` (joins the saved network; falls back to AP `AffaGolden`/`affagolden`) |
| tests | `pio test -e native` → **255/255**. `rm -rf .pio/build/native` first if results look odd — a stale build has hidden real failures here twice |
| protocol truth | `docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md`, derived from `docs/captures/*.csv` |

The C3 SuperMini at `192.168.100.85` **cannot receive** — its CANRX reads permanently dominant.
Diagnosis is in memory and in the ground-truth doc; do not debug firmware against it.

---

## The protocol, in one screen

Direction is readable from the padding byte: **`0xA3` = the display, `0x00` = the radio (us)**.

```
silence        -> 3AF BA                       we announce. BA only, no B9. Slow.
RX 3CF 61 11 xx                                the panel asks. 00 and 01 are the SAME request.
RX 3CF 61 11 xx  -> 3AF B0 14 11 00 1F  x3     the SECOND request draws the burst, 31 ms apart
RX 1C1 70        -> 5C1 74                     MANDATORY reflex, ~0.5 ms, at any phase
                 -> 151 70, 1F1 70             ONLY after the panel's 1C1. Part of the opening.
RX 551 74, 5F1 74                              registered
+400 ms          -> 151 03 52 09 00            display ON. ALWAYS the first payload.
                 -> 151 05 56 31 30 30 30      "1000" = 10:00
registered       -> 3AF B9 every 500 ms        free-running. NOT a pong.
```

Five rules that each cost a session to learn:

1. **`61 11 00` and `61 11 01` are the same request.** One capture runs a whole session on
   sixteen `01`s and zero `00`s.
2. **Our `BA` comes first, and the panel's *next* request draws the burst.** Without it the
   panel never opens its `1C1` and nothing registers.
3. **The display registers its channel before we register ours**, and its `1C1` must be
   acknowledged on `5C1` unconditionally.
4. **`03 52 09` before anything is drawn.** A dark panel ACKs a screen it never lights — every
   counter says success and the glass stays black.
5. **Any `61 11` while registered means the panel voided us.** Stop rendering and re-open.

ISO-TP: DLC 8 always; first frame `10 <len>`; **the first consecutive frame is `0x21`, not
`0x20`**; the panel answers *every* CF with `30 01 00` (BS=1) and sends `74` only at the end.

---

## What to do next, in order

`docs/REFACTOR-PLAN.md` has the full design and rationale. The short version:

The sync FSM carries **18 profile flags and 22 state variables**, nearly all of them hedges
between competing theories of a wire that is now measured. Collapse them into one explicit
`Phase` enum, reduce `SyncProfile` to identity + timings + one real knob, split the 2102-line
`AffaDisplayBase.cpp` into four units, and bring UpdateList onto the same rules.

Nine steps, sequenced so each is provable. **Flash and soak after step 5 and step 9** — a green
suite has already let a broken handshake through twice.

### Two decisions already made, do not relitigate

* **UpdateList adopts the Carminat rules** (owner, 2026-08-04). Its bytes are confirmed correct
  against the reference implementation; its *logic* differed in five ways and all five are
  deliberately removed. Its only remaining difference is that it answers the **first** `61 11`
  with no announce precondition.
* **The periodic session drop is backlogged, not blocking.** It self-heals. Step 6 of the plan
  makes it visible instead of fixing it blind.

---

## Open, and honestly open

**Why the panel drops the session every ~7 minutes.** Fourteen times in a 96-minute soak,
invisible because recovery works. **The paint rate is NOT the cause** — tested at 16× fewer
screens, same drop rate. All driver counters stay zero across the drops, and the intervals are
15 s to 1409 s, so it is neither electrical nor a timer. Step 6 of the plan captures the wire
at the moment it happens.

**`replyToPing = false` is unverified for UpdateList.** Its reference pongs every `0x69`, and
that pong was its *only* heartbeat until March 2026. We removed it on the strength of four
**Carminat** captures. If an UpdateList panel ever stalls in the handshake, turn this knob
first.

**`0x56` (setTime) has never been seen from an OEM radio.** It is confirmed on glass — the
owner read `10:00` off the panel and it free-ran to `10:11` — but no capture witnesses the OEM
sending it.

**`02 54 01` / `02 54 03` and `1C1 02 64 0F` are undecoded.** Not needed for anything working.

---

## How this project gets things wrong

Worth reading before writing code here. **Every protocol bug this session was the same shape: a
special case standing in for a general rule.**

| what was written | what the captures actually say |
|---|---|
| `61 11 01` is discovery-only | any complete `61 11 xx` is the same request |
| tear down only on `61 11 01` | any `61 11` while registered voids the session |
| registration happens on the first render | registration is part of the opening |
| the hello answers the first request | our `BA` first; the *next* request draws it |

Four times, the same error: encoding the case in front of me instead of the law the data
states. The captures were unambiguous each time.

And the counters lie by omission. `rx 0` with zero errors fits *three* different states — a
silent bus, a bus we cannot decode, and a controller that never started. Telling them apart
needs `msgs_to_tx`; without it a dead C3 receive path looked exactly like a sleeping display
for hours, and an oscilloscope was right where the firmware was wrong. **Expose queue depths on
any diagnostic surface.**

Read `docs/BENCH-VERIFIED.md` for what has actually been seen on glass, as opposed to what the
code believes.
