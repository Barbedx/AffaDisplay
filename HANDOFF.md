# Handoff — 2026-08-04/05, the day both families worked

Library **0.5.0**. The previous handoff (2026-07-29) described a bench that could decode the
panel but never establish a link. That problem was solved earlier today; this one records
what happened after.

Two things landed, and the second is the bigger one:

1. **The refactor is complete** — all nine steps of
   [`docs/REFACTOR-PLAN.md`](docs/REFACTOR-PLAN.md) — and it has been on glass. The
   refactored firmware opens the session, powers the panel and renders unattended in 6.2 s
   from boot with nothing in the application asking it to.
2. **The UpdateList (AFFA2) family works on real hardware, for the first time ever.** It had
   never put a frame on a bus. Its sequencing was an *argument* — that the two families were
   nearly the same code, and that the surviving reference driver worked rather than being
   right. The bench panel turned out to be universal, and the argument held: opening to
   `SUCCESS` in 220 ms of wire time, first attempt.

Read this file first, then the refactor plan, then `docs/BENCH-VERIFIED.md` for what has
actually been seen on glass as opposed to what the code believes.

**0.5.0 is a BREAKING release.** `SyncProfile` lost six fields; a downstream profile that
set any of them will not compile, which is deliberate — every one of them encoded a fact the
captures have since settled, and silently ignoring them would be worse. See
`src/core/AffaSyncProfile.h`, which names each deleted field and where its rule now lives.

---

## Where things actually are

**It works.** `examples/09_golden` on an ESP32 DevKit V1 ran unattended for 1 h 36 m at
`0a9095c`: 24 912 fullscreen transfers, `failed 0`, and `txErr / rxErr / busErr / arbLost /
rxMissed / ringOverflow` **all zero**. The refactored build reproduces the whole opening —
announce, burst, the panel's `1C1`, registration, `03 52 09`, ISO-TP text — in 5.7 s from
boot, and soaked 31.7 minutes with zero session losses.

The library is now **`Phase`-driven**: one ordered value, one writer, nine named edges.

```
Silent -> Announced -> HelloPending -> AwaitPeerChannel -> Registering -> Settling
                                                                    -> Powering -> Ready
```

`phase` is the first line to read when the glass is dark — a phase that will not advance
names the frame that never arrived. `AwaitPeerChannel` in particular means the panel never
got our announce, which is a failure that once cost a whole session to recognise. And
**`Ready` means the glass is on**, not merely that we are registered: the library sends the
family's power-on itself and waits for the ACK, because a panel that is not on ACKs a screen
it never lights and every counter reports success.

Both families run this same machine. UpdateList's only remaining difference is that its
burst answers the panel's *first* request rather than its second.

**And UpdateList works on glass, as of 2026-08-04.** The bench panel is universal: driven by
`examples/10_updatelist` it opened, registered, powered and rendered `SUCCESS` on the first
attempt, 220 ms of wire time, every counter zero. That family had never put a frame on a bus
before — its sequencing was an argument until that run. `docs/BENCH-VERIFIED.md` records what
it proved and, more usefully, the four things it did **not**.

**An unrelated project corroborates the whole opening, frame for frame.** A three-part
[Hackaday series on this exact display](https://hackaday.io/project/27439-smart-car-radio/log/67942-reverse-engineering-the-renault-update-list-display-part-3),
by a different author from a different car, documents `3DF 7A 01` → `3DF 79 00` →
`3DF 70 1A 11 00 00 00 00 01` → `121 70` → `1B1 70` → `1B1 04 52 02 FF FF`. That is
byte-identical to what this library emits, **in that order**. Those bytes reached us through
`notes/archive_mhroczny/affa3.c` on the argument that the reference driver was trustworthy;
an outsider arriving at the same six frames independently turns that argument into evidence
— including the ORDER, which is the part step 8 changed on reasoning alone.

Three rows scroll at independent speeds; pause/resume, per-row text and speed, clock entry,
wire-log download, WiFi setup and OTA all work from the web console.

| | |
|---|---|
| board | ESP32 DevKit V1, **CRX → GPIO5, CTX → GPIO4**, 500 kbit/s |
| panel | **UNIVERSAL** — the same physical unit answers as Carminat *and* as UpdateList. Both were driven on it today |
| flash Carminat | `pio run -e ex09_golden -t upload --upload-port COM5` |
| flash UpdateList | `pio run -e ex10_updatelist -t upload --upload-port COM5`, then `pio device monitor -e ex10_updatelist` |
| console | `http://192.168.100.97/` (joins the saved network; falls back to AP `AffaGolden`/`affagolden`) |
| tests | `pio test -e native` → **259/259**. `rm -rf .pio/build/native` first if results look odd — a stale build has hidden real failures here twice |
| protocol truth | `docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md`, derived from `docs/captures/*.csv` |
| what is proven | `docs/BENCH-VERIFIED.md` — and it is careful about what is *not* |

**Whatever is on the board right now is probably not `09_golden`.** The day ended with
`12_ulclock` flashed — the clock probe, which cycles 23 candidate frames and will look like
nonsense on the glass until you know what it is. Check before assuming, and reflash before
starting a soak.

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

1. **`61 11 00` and `61 11 01` are the same request** — and so is every other byte 2. The
   library stopped reading it on 2026-08-04; one capture runs a whole session on sixteen
   `01`s and zero `00`s, and nothing distinguishes an unmeasured value from either.
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

**THE REFACTOR IS DONE — all nine steps of `docs/REFACTOR-PLAN.md`.** The final build is
flashed and running, and it opens the session, lights the glass and renders unattended. What
is left is a long soak.

### 1. THE LONG SOAK. It is the one thing owed, and it has been owed all day

```
pio run -e ex09_golden -t upload --upload-port COM5     # then leave it ALONE
http://192.168.100.97/          ->   phase / drops / reason on the third line
http://192.168.100.97/deregistered.txt        the frames from BEFORE the first drop
```

Three soaks were attempted and **all three were cut short by reflashing the board underneath
them** — 31.7, 20.8 and 15.5 minutes, zero drops in every one. Against a build that lost
fourteen sessions in 96 minutes that is encouraging and *nothing more*: half an hour clean is
only about 1 % surprising if the rate were unchanged, the observed intervals ran from 15 s to
1409 s so even that number is generous, and **nothing in the refactor has a mechanism that
would stop a panel deauthorizing us.** The reasoning is in `docs/REFACTOR-PLAN.md` under "The
drop did not happen"; it is deliberately not written up as fixed.

What is different now is that a drop can no longer hide. The status page counts them and
names the cause, and the first one freezes the wire ring. **`/deregistered.txt` lives in RAM
— download it before re-flashing.**

So: flash `09_golden`, walk away for a few hours, and read the third line when you come back.

### 2. Know that `Ready` now means the glass is on

The library sends the family's power-on itself at the end of the opening and does not report
`Phase::Ready` until the panel has acknowledged it. An application's whole share of *"`03 52
09` before anything is drawn"* is now **do not draw before `Ready`**.

It stands down whenever a desired power state already exists — queued, cached, or a
deliberate `setPower(false)` — and `setAutoPower(false)` turns it off entirely, at which
point `Ready` means what it used to mean. `09_golden` is the worked example: it stopped
sending power and kept only its 750 ms warm-up, which is a property of the glass rather than
of the protocol.

### 3. The clock — ASKED AND ANSWERED, negatively. Do not re-run it blind

`examples/12_ulclock` fired **23 candidate frames over 7 passes — 162 probes** — at a
registered, lit, rendering UpdateList session. **Nothing moved the clock.** The full table is
in `docs/BENCH-VERIFIED.md`; between it, three independent reverse-engineering projects that
document no clock frame either, and a Hackaday series describing the panel as having an
*integrated* clock, the working conclusion is that **on this family the radio does not set
the clock and the panel does.** If you need it set, drive the panel as Carminat —
`151 05 56 "HHMM"` is proven on this exact unit.

**The reusable lesson is not the failure, it is the instrument.** THE PANEL ACKS EVERYTHING:
it answered a terminal `74` to command bytes it has certainly never seen, on both registered
functions. Anyone probing this family by watching for ACKs will believe they have found
something twenty times over. It is the same trap as *"a dark panel ACKs a screen it never
lights"*, one layer down, and it will bite the next protocol question too.

If you do want to push further, the cheap next step is a **`70` probe sweep over candidate
function ids** — the panel answers `<id>|0x400 74` for each function it accepts, so it will
*tell you* its table instead of being guessed at.

### 4. Then the two open items below

Neither blocks anything. The half-open-session hole needs a rule no capture provides; the
~7-minute drop now reports itself.

### Decisions already made, do not relitigate

* **UpdateList adopts the Carminat rules** (owner, 2026-08-04) — and this is no longer a
  decision, it is a measurement. Four of the five differences it removed are now confirmed on
  glass. Its only remaining difference is that it answers the **first** `61 11` with no
  announce precondition, and even that is *unproven* rather than wrong: in the one hardware
  run, our announce went out before the request anyway.
* **The library owns power-before-render** (owner, 2026-08-04): universal, with
  `setAutoPower(false)` as the opt-out. `Phase::Ready` means the glass is on.
* **The periodic session drop is backlogged, not blocking.** It self-heals, and it now
  reports itself instead of being invisible.

---

## Open, and honestly open

**Why the panel drops the session every ~7 minutes.** Fourteen times in a 96-minute soak,
invisible because recovery works. **The paint rate is NOT the cause** — tested at 16× fewer
screens, same drop rate. All driver counters stay zero across the drops, and the intervals are
15 s to 1409 s, so it is neither electrical nor a timer. **It is no longer invisible:** the
next soak counts the drops, names the cause (`PanelVoided` / `PeerTimeout` / `LinkRestarted`)
and freezes the wire ring at the first one. See "What to do next".

**A half-open session has no teardown, as of 2026-08-04.** A session that drew its burst but
never latched `FUNCSREG` cannot be voided by any inbound frame, and the peer watchdog does not
run before `FUNCSREG` either — so if our `151` probe is never acknowledged, registration
re-queues for ever and nothing says the opening failed. Not a regression (a real panel sends
`00` or `01`, and both were already absorbed there), but a hole. Closing it needs a rule for a
`61 11` arriving after the burst and before registration, and **no capture answers that** —
in all four the panel stops asking once it has the burst. Do not guess it.

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

**A fifth was found in a TEST, on 2026-08-04, which is worse.**
`test_carminat_ignores_unknown_full_auth_until_00` asserted that `61 11 5A` produced nothing
at all — no announce, no burst, no session — until a `61 11 00` arrived. No capture contains
`5A`, or says byte 2 is read at all. The special case had been promoted from code into a
regression test, where it looked like a measurement and would have outlived the code that
made it true. It is now `test_any_complete_61_11_xx_is_the_same_request`.

The lesson generalises past this bug: **a test that pins a flag's VALUE is weaker than one
that pins the wire.** Four assertions of the form `TEST_ASSERT_FALSE(kSync.someFlag)` went
with the flags they named, and every one of them would have gone on passing while the FSM
did something else entirely.

And the counters lie by omission. `rx 0` with zero errors fits *three* different states — a
silent bus, a bus we cannot decode, and a controller that never started. Telling them apart
needs `msgs_to_tx`; without it a dead C3 receive path looked exactly like a sleeping display
for hours, and an oscilloscope was right where the firmware was wrong. **Expose queue depths on
any diagnostic surface.**

Read `docs/BENCH-VERIFIED.md` for what has actually been seen on glass, as opposed to what the
code believes.
