# Handoff — 2026-08-04, end of the day the protocol was settled

The previous handoff (2026-07-29) described a bench that could decode the panel but never
establish a link. **That problem is solved and the whole protocol is now proven on glass.** It
is archived at `docs/HANDOFF-2026-07-29.md`; almost every open question in it is answered here.

The refactor planned in [`docs/REFACTOR-PLAN.md`](docs/REFACTOR-PLAN.md) **landed in full on
2026-08-04**, all nine steps, and **it has been on glass**: the refactored firmware opens the
session, powers the panel and renders unattended, 6.2 s from boot, with nothing in the
application asking it to. Read that file second. Read this one first.

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

Three rows scroll at independent speeds; pause/resume, per-row text and speed, clock entry,
wire-log download, WiFi setup and OTA all work from the web console.

| | |
|---|---|
| board | ESP32 DevKit V1, **CRX → GPIO5, CTX → GPIO4**, 500 kbit/s |
| flash | `pio run -e ex09_golden -t upload --upload-port COM5` |
| console | `http://192.168.100.97/` (joins the saved network; falls back to AP `AffaGolden`/`affagolden`) |
| tests | `pio test -e native` → **257/257**. `rm -rf .pio/build/native` first if results look odd — a stale build has hidden real failures here twice |
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

### 1. Let it soak, and read the drop line

```
http://192.168.100.97/          ->   phase / drops / reason on the third line
http://192.168.100.97/deregistered.txt        the frames from BEFORE the first drop
```

The step-2/3/6 build ran **31.7 min, 246 screens, zero drops, every counter zero**, where the
proven build lost fourteen sessions in 96 min. **That is not a fix claim** — the reasoning,
including why it is only ~1 % surprising, is in `docs/REFACTOR-PLAN.md` under "The drop did
not happen". Run the next one long. `/deregistered.txt` lives in RAM: **download it before
re-flashing.**

### 2. Know that `Ready` now means the glass is on

The library sends the family's power-on itself at the end of the opening and does not report
`Phase::Ready` until the panel has acknowledged it. An application's whole share of *"`03 52
09` before anything is drawn"* is now **do not draw before `Ready`**.

It stands down whenever a desired power state already exists — queued, cached, or a
deliberate `setPower(false)` — and `setAutoPower(false)` turns it off entirely, at which
point `Ready` means what it used to mean. `09_golden` is the worked example: it stopped
sending power and kept only its 750 ms warm-up, which is a property of the glass rather than
of the protocol.

### 3. Then the two open items below

Neither blocks anything. The half-open-session hole needs a rule no capture provides; the
~7-minute drop now reports itself.

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
