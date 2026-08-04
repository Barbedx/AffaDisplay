# Refactor plan — one state machine, two families, no hedges

**Status: EXECUTED, 2026-08-04.** All nine steps are done and the result has been on glass.
Written the same day, immediately after the Carminat protocol was settled against OEM
captures and proven for 1 h 36 m at 24 912 screens with every error counter at zero.

What is left is not implementation: a **long soak** of the final build (step 9's second half),
and the two open questions at the end of this file — the half-open-session hole, and why the
panel drops the session, which a 31.7-minute run of the intermediate build did not reproduce.

The rest of this document is kept in the present tense on purpose. It is the reasoning, and
the reasoning is the part worth re-reading when something on this bus surprises you.

## Why now, and why not before

The sync FSM currently carries **18 profile flags** and **22 state variables**. That is not
because the protocol is complicated — the whole handshake fits on one page — but because it
was **unknown**. Almost every flag is a hedge between two competing theories of the wire:

| flag | the question it was hedging |
|---|---|
| `requireAuthRequest`, `authRequestByte2` | does only `61 11 00` authorize? |
| `helloOnNonAuthRequest` | should `61 11 01` get a hello? |
| `helloAfterBootstrapRequest` | …or does it authorize after our BA? |
| `helloRequiresAnnounce` | must our BA precede the burst? |
| `bootstrapAliveFrame` | does the announce lead with B9? |
| `oneShotResyncOnStart`, `oneShotResyncOnPeerAlive` | which panel frame arms discovery? |
| `registerAfterHello` | is registration lazy or part of the opening? |
| `replyToPing` | is B9 a pong? |
| `waitForPanel`, `sendSyncRequest` | who opens the conversation? |

**Every one of those questions now has a measured answer.** A flag that encodes a settled fact
is not configuration — it is a place for the two halves of the codebase to disagree, and four
separate bugs this session lived in exactly those seams. The flags should collapse into the
answer.

## The target

### One explicit phase, replacing 22 booleans

```cpp
enum class Phase : uint8_t {
  Silent,            // nothing heard ever; announcing on a slow timer
  Announced,         // our request is on the wire; awaiting the panel's next
  HelloPending,      // the announce burst is mid-flight
  AwaitPeerChannel,  // burst done; waiting for the panel to open ITS channel
  Registering,       // our function probes are out, awaiting their ACKs
  Settling,          // the measured quiet interval before any payload
  Ready              // power confirmed; application rendering permitted
};
```

Every transition becomes one named edge with one trigger. The questions that cost this session
a week — *"can we register yet?"*, *"why is nothing rendering?"* — become one field to print.
`Phase` is also the natural thing to expose on a console, which is how the last four bugs were
actually found.

### A profile that is identity and timing, not policy

```cpp
struct SyncProfile {
  // Identity — the bytes and ids that differ between families
  uint16_t syncTxId, syncRxId, replyFlag;
  uint8_t  aliveByte, requestByte, requestArg, filler;
  const uint8_t (*hello)[8];
  uint8_t  helloCount;
  uint16_t peerChannelId;          // the panel's own channel; 0 = family has none
  const uint16_t* funcIds;
  uint8_t  funcCount;

  // Timing — measured, per family
  uint32_t helloFirstDelayMs, helloFrameGapMs, helloMinMs;
  uint32_t payloadAfterRegistrationMs, heartbeatMs, announceWhenSilentMs;

  // Behaviour — only where the families GENUINELY differ
  Opening  opening;                // RadioAnnounces | PanelLeads
  bool     registerInOpening;      // vs. lazily, on the first render
  bool     heartbeatAfterRegistration;
};
```

Three behaviour knobs instead of ten. Anything that is now a known fact — the peer-channel ACK
being unconditional, `61 11 xx` being one request regardless of byte 2, the burst answering the
*second* request — becomes **code**, not configuration.

### Files split by responsibility

`AffaDisplayBase.cpp` is 2102 lines spanning four unrelated jobs:

| new unit | ~lines | owns |
|---|---|---|
| `AffaDisplayBase.cpp` | 600 | lifecycle, `poll()` orchestration, public surface |
| `AffaSync.cpp` | 500 | the opening FSM — the `Phase` table and nothing else |
| `AffaTx.cpp` | 700 | queue, ISO-TP segmentation, flow control, retries |
| `AffaObserve.cpp` | 300 | frame tap, subscriptions, events |

### `CanCommonLink` becomes the default ESP32 link

It is the stack proven end to end this session, and the one most existing Renault/ESP32 code
already uses. `Esp32CanLink` stays for raw-TWAI builds.

## UpdateList — the hypothesis was half right, and the other half is load-bearing

The working hypothesis was **same logic, different bytes**. The reference implementation
(`MeganeCAN/src/display/UpdateList/`, which drove a real panel) was read against all ten
questions the Carminat model answers. Result:

**The bytes are right — completely.** Every literal in `src/updatelist/` matches the reference:
ids (`3DF`/`3CF`, funcs `121`/`1B1`, panel channel `0A9` → `4A9`), filler `0x81`, hello
`70 1A 11 00 00 00 00 01`, alive `79 00`, request `7A 01` (a real argument, not filler),
registration order, ACK `0x74` and flow control `30 01 00`, power `04 52 02 FF FF`, and both
setText encodings including the 29-byte segment payload. **No byte-level disagreement was found
anywhere.** The golden vectors in WIRE-SPEC are correct.

**The logic is NOT the same machine.** Five structural differences, each read from the shipped
driver rather than inferred:

| Carminat, proven on glass 2026-08-04 | UpdateList reference |
|---|---|
| our `BA` precedes the burst; the panel's **second** request draws it | answers the **first** `61 11` immediately, from inside `recv()`; no announce precondition |
| registration is part of the opening, pipelined | **lazy** — triggered by the first render — and **serial**, each probe waiting for its own ACK |
| the panel's `1C1 70` must arrive and be acknowledged before we register | `0A9 70` is acknowledged, but **nothing gates on it** |
| any `61 11 xx` while registered voids the session | **no `61 11` teardown at all**; `FUNCSREG` survives. Only the peer watchdog drops it |
| `03 52 09` must precede any render or the glass stays dark | **no such rule**; builds routinely render having never sent `0x1B1` |

### DECISION 2026-08-04: UpdateList adopts the Carminat rules

The owner's call, and the reasoning is sound: **the two originals were nearly the same code**
(`archive_mhroczny/affa3.c` is the ancestor of *both* — see the naming note below), the
Carminat side has been reworked against OEM captures and proven on glass for 96 minutes, and
the UpdateList reference *worked* rather than being *right*. A driver whose only heartbeat was
a pong for most of its life, that registers lazily and serially, and that cannot notice a panel
deauthorizing it, is coincidence-shaped. We take the measured machine over the surviving one.

So all five differences are **deliberately removed**, one exception aside:

| # | UpdateList reference did | we will do | note |
|---|---|---|---|
| 1 | answers the first `61 11` immediately | **same — answer the first request** | THE ONE EXCEPTION. `helloRequiresAnnounce = false`. Our `BA` should provoke a `61 11`, but the panel also volunteers them, so the announce is not a precondition here |
| 2 | lazy, serial registration on first render | **registration in the opening**, pipelined | `registerInOpening = true` |
| 3 | ACKs `0A9 70`, gates nothing | **wait for the peer channel** before registering ours | same gate as Carminat |
| 4 | no `61 11` teardown; `FUNCSREG` survives | **any `61 11` while registered voids the session** | same rule, byte 2 irrelevant |
| 5 | no power-before-render rule | **`1B1 04 52 02 FF FF` is mandatory before any text** | a panel that is not on ACKs a render it never lights |

**Consequence for the design: the knobs shrink further.** `registerInOpening`,
`waitForPeerChannel`, the teardown rule and power-before-render all become **universal code**,
not configuration. Only `helloRequiresAnnounce` remains a genuine per-family difference — plus
the identity bytes and timings, which is what a profile should have been all along.

**And the library should own power-before-render.** Both families now require it, and it is
exactly the kind of rule an application forgets — this one did, and the panel ACKed a screen it
never lit. `Phase::Ready` should mean *"registered AND the glass is on"*, with the library
emitting the family's power frame itself on the way there. That is what "the library handles
it and exposes an API" has to mean.

### Naming trap, recorded because it wastes an hour every time

`notes/archive_mhroczny/affa3.c` is the **UpdateList** ancestor, not Carminat — it defines
`0x3DF` / `0x121` / `0x1B1`, which are the UpdateList ids. Meanwhile this repository calls the
*Carminat* family "AFFA3 NAV". So the file named `affa3` implements what we call AFFA2. Do not
assume the numbering agrees with ours.

### The one finding to act on first — SETTLED 2026-08-04, and it was not wrong

**`replyToPing = false` is correct for UpdateList.** Measured on glass: the panel pings
every ~500 ms, is never ponged, and the session held throughout with every counter at zero.
The reasoning below stands as the reason it was *doubted*, which is worth keeping — the
removal really was justified on Carminat evidence alone, and it really could have been
wrong. It simply was not.

**`replyToPing = false` is unverified for UpdateList and may be wrong.** The reference calls
`tick()` from its `0x69` handler, so every panel ping provokes an immediate `3DF 79 00 …` —
and until March 2026 that pong was the driver's **only** heartbeat; the free-running 1 Hz timer
was added later and the pong left in. Our port removed it. The removal is justified by four
**Carminat** captures (B9 free-running at σ 0.33 against a 69 at σ 4.60, phase wrapping past
zero) and there is **no UpdateList evidence either way**. If an UpdateList panel ever stalls in
the handshake, this is the first knob to turn.

### Also unverified, and not to be assumed

* Lazy registration — the reference is lazy, but the one real OEM bus using these function ids
  registers 400 ms *ahead* of content, i.e. as part of the opening.
* `04 52 <state> FF FF` versus an OEM head unit's observed `03 52 <state> 00`.
* The panel's own filler byte — genuinely unknown. `0x84` and `0xA2` are the only peer fillers
  seen anywhere near this family; do **not** port Carminat's `0xA3` as an expectation.

`test_updatelist_wire` must stay green throughout. It is the only thing standing between a
refactor and silently changing a family nobody can currently test on hardware.

## Sequencing, and how each step is proved

The library works. Each step must keep it working, and "it compiles" is not evidence.

1. ~~**Extract the UpdateList byte truth**~~ — **DONE**, see the section above. The bytes are
   right; the logic differs; the owner has ruled that it adopts the Carminat rules.
2. ~~**Collapse the dead flags.**~~ — **DONE** (`2f561c0`). Six went:
   `authRequestByte2`, `helloAfterBootstrapRequest`, `helloOnNonAuthRequest`,
   `oneShotResyncOnStart`, `oneShotResyncOnPeerAlive`, `bootstrapAliveFrame`, and with them
   the whole second copy of the request branch, `BootstrapStage`, and `_unauthControlStage`.
   Not a pure deletion in the end, and the commit says so: making `61 11 xx` one request
   changes what an unmeasured byte 2 does, and closing the second branch closed the only
   inbound teardown of a pre-`FUNCSREG` session (see the new open item below).
3. ~~**Introduce `Phase`**~~ — **DONE** (`9ac2a11`), derived rather than stored, because a
   tenth thing that can disagree with the other nine is the disease. What makes step 4
   checkable is `test_phase_walks_the_measured_opening_in_order`, which drives the opening
   one frame at a time and pins which frame moves the phase on.
4. ~~**Invert it**~~ — **DONE** (`67975d8`). `_authRequestObserved`, `_authHelloPending` and
   `_unauthControlIssued` deleted; `_phase` has one writer, `enterPhase()`, and nine named
   edges. What survives is documented as *not* a hedge: `_helloPending` is a transmit
   cursor, `_unauthControlSpent` is a budget ("spent but never sent" is a real state),
   `_peerChannelSeen` has to be a latch because the panel's `1C1` arrives *during* the
   burst, and `_panelObserved` / `_syncRequestObserved` are facts about the peer.
5. ~~**Flash and soak Carminat.**~~ — **DONE 2026-08-04.** The step-2/3/6 build opened the
   session unattended in 5.6 s and ran **31.7 minutes, 246 screens, zero drops, every
   counter zero**. See "the drop did not happen" below.
6. ~~**Add the drop snapshot**~~ — **DONE EARLY** (`cc9c9db`), deliberately out of order: it
   had to exist before the step 5 soak or that soak collects the same evidence the last one
   did, which is none. `LossReason` in the library, `/deregistered.txt` in `09_golden`,
   `sessionsLost()` on the status page. Still outstanding: the ring snapshot belongs in the
   library once step 7 gives `AffaObserve` the tap; today it lives in the example.
7. ~~**Split the files.**~~ — **DONE** (`94e932e`). 605 / 710 / 728 / 184 lines. No behaviour
   change, and it is *measured*: ex09_golden built either side of the split came out at
   **identical RAM (60580) and Flash (862377) byte counts with 7018 defined symbols on both
   sides, none added or removed.** 18 small leaf functions changed size by a few bytes —
   cross-TU inlining, which is exactly and only what splitting a translation unit causes.
8. ~~**Move UpdateList onto the shared rules**~~ — **DONE** (`fc33740`, `e5020d9`).
   Registration in the opening, peer-channel gate, `61 11` teardown, heartbeat after
   registration, power before render, `helloRequiresAnnounce = false` as the sole
   difference. Not one byte moved and `test_updatelist_wire` is green.
9. ~~**Flash and soak again.**~~ — **FLASHED 2026-08-04** and verified on glass: the opening
   completes in 6.2 s with the library lighting the panel itself, exactly one `03 52 09` on
   the wire, phase `Ready`, every counter zero. **The long soak is the one thing still
   outstanding.**

### Step 8's fifth item, as decided

*"`Phase::Ready` should mean registered AND the glass is on."* Owner's call, 2026-08-04:
**universal, with an opt-out.** `Phase::Powering` sits between `Settling` and `Ready`, and
`AffaDisplayBase::setAutoPower(false)` turns it off for a build that must decide for itself.

Two details worth keeping, because both were got wrong once on the way in:

* **The command is queued on the FUNCSREG rising edge, not when the payload gate opens.**
  `03 52 09` must precede any render, so it has to be *in the queue* before the
  application's held work becomes eligible. Issued at the gate it merely races that work,
  and the first version did exactly that — the frame arrived a poll late and landed in the
  middle of other people's sequences.
* **It stands down whenever somebody else has an opinion** — a queued or cached power state
  (including a deliberate `setPower(false)`), a family with no power command, or the
  opt-out. That is also why a *recovered* session does not re-issue it: the durable-control
  cache already replays power after every re-registration, so the desired state exists.

`09_golden` stopped sending power itself and now only waits for `Ready`. It kept its 750 ms
warm-up, which is a property of the **glass**, not the protocol: the panel ACKs the command
before the display is legible, and a screen drawn into that window is lost with every
counter reporting success.

### Why step 4 was not done with 2 and 3, and the case for soaking first

Steps 2, 3 and 6 landed together on 2026-08-04. Step 4 deliberately did not, and the plan's
own sequencing is the argument against that decision, so here is the argument for it.

Right now the tree is a *good* thing to put on a bench: the behavioural delta since the
proven build is one branch (`61 11 xx` for xx outside {00, 01}) plus a hold-window re-arm,
and everything else added is observation — `Phase`, `sessionsLost()`, `LossReason`,
`/deregistered.txt`. If it soaks clean, that is a real result, and if it does not, the
suspect list is two items long.

Step 4 rewrites `handleSyncFrame`, `pumpSync`, `pumpHello`, `pumpUnauthControl`,
`linkReady`, `pumpTx`'s gates and `begin()` — the whole opening — against a host suite
only. Stacking it on top of 2 and 3 before any bench check means that if the next soak is
red, three unvalidated changes are in the frame at once. **A green suite has already let a
broken handshake through twice this session**; that sentence is in this plan and in
HANDOFF.md, and it is the reason to spend one bench cycle here.

Owner's call. If the answer is "just do 4 and soak once", nothing in step 4 depends on the
soak having happened — the phase tests are the safety net either way.

> **RESOLVED 2026-08-04, on glass.** The caveat below is kept because it was the honest
> position at the time, but it no longer applies: the bench panel is universal and answered
> as an UpdateList panel on the first attempt. Registration in the opening, the peer-channel
> gate, the single hello and library-owned power all worked, `setText("SUCCESS")` rendered,
> and `replyToPing = false` — "the first knob to turn" — did not need turning. See
> `docs/BENCH-VERIFIED.md` and `docs/captures/2026-08-04-updatelist-first-contact.log`.
>
> Step 8 changes a family that cannot currently be tested on hardware. The mitigation is that
> every byte it emits is already pinned by golden vectors, and what changes is *when* frames go
> out, not *what* is in them. If an UpdateList panel ever reaches the bench and stalls, the
> first knob to turn is `replyToPing` (see above), and the second is reverting step 8's
> registration timing to lazy.

## Open, new on 2026-08-04: a half-open session has no teardown

Deleting the second request branch (step 2) closed the only path by which an inbound frame
could void a session that had **drawn its burst but not yet latched `FUNCSREG`**. The
teardown is now gated on `FUNCSREG`, which is the measured rule — *"any `61 11` while
registered means the panel voided us"* — and the peer watchdog does not run before `FUNCSREG`
either, because `pumpSync()` returns early on `registerAfterHello && !FuncsReg`.

**It is not a regression.** A real panel sends `00` or `01`, and both were already absorbed
in that state; only an unmeasured byte 2 reached the branch that tore down. But it is a hole:
if our `151` probe is never acknowledged, the library re-queues registrations for ever and
nothing — no frame, no timer — says the opening failed.

Closing it means deciding what a `61 11` arriving *after* the burst and *before* registration
means, and **no capture answers that**: in all four, the panel stops asking once it has the
burst. The two candidate rules are "re-burst, the opening failed" and "absorb, it is a
duplicate", and picking wrong in the first direction re-opens the session on every panel that
merely repeats itself. Do not guess it; measure it, or bound it with a timer that is honestly
labelled as ours rather than the protocol's.

## The drop did not happen — 2026-08-04, and this is NOT a fix claim

The step-2/3/6 build ran **31.7 minutes with zero session losses**, 246 fullscreen transfers,
every driver counter at zero. The build it replaced lost fourteen sessions in 96 minutes,
i.e. one per 411 s on average. Over 1901 s a Poisson process at that rate misses entirely
about **1 % of the time**.

That is suggestive and it is not proof, and the difference matters:

* The observed intervals ran from **15 s to 1409 s**, so the distribution is nothing like
  Poisson — it is irregular in a way nobody has explained. A single 1901 s gap is *inside*
  the observed spread, barely.
* The paint rate was changed mid-run (2 s to 12 s per marquee step) to widen the snapshot
  window. Rate was already shown not to drive the drop — 16× fewer screens gave the same
  rate — but it is a variable that moved.
* Nothing in steps 2, 3 or 6 was aimed at this. The only behavioural changes are `61 11 xx`
  for an unmeasured byte 2, and a hold-window re-arm on the FUNCSREG teardown. Neither has a
  mechanism that would stop a panel deauthorizing us.

**So: do not write this down as fixed.** What it does mean is that the next soak is worth
running long, and that if a drop does come, `/deregistered.txt` and the `LossReason` line now
answer *which kind* it was on the first occurrence rather than the fifteenth.

## Open: why the panel drops the session every ~7 minutes

During a 1 h 36 m soak the session was lost and re-opened **fourteen times**. It is invisible
to a user because the FSM self-heals — re-announce, re-register, re-power the glass, resume —
which is worth having, but the cause is unexplained.

**The paint rate is NOT the cause.** Tested directly by dropping the row periods from
220/380/550 ms to 2000 ms, i.e. ~8 fullscreen transfers per second down to ~0.5:

| | drops | over | rate |
|---|---|---|---|
| ~8 screens/s | 11 | 4900 s | 1 per 445 s |
| ~0.5 screens/s | 3 | 1099 s | 1 per 366 s |

Sixteen times fewer screens, and if anything a slightly *higher* drop rate. The theory that we
were overrunning the panel is dead.

What is known: every driver counter stays at zero across the drops — `txErr`, `rxErr`,
`busErr`, `arbLost`, `rxMissed`, `ringOverflow` — so it is not electrical and nothing is being
lost in our receive path. The intervals are wildly irregular (15 s to 1409 s), so it is not a
timer on either side. Remaining candidates, untested: the panel re-registering as normal
behaviour for this unit; CPU contention between WiFi and the CAN poll task; or something in our
heartbeat timing that the OEM radio does differently.

**Accepted for now** (owner, 2026-08-04): the recovery is good enough to ship on, and this goes
to the backlog rather than blocking the refactor. But it needs to stop being invisible.

### Build the evidence capture into the refactor — `Phase` makes it nearly free

The problem is that the drop scrolls away: by the time anyone looks, the ring holds the frames
of the *recovery*, not of the cause. Downloading `/wire.txt` promptly is a race nobody wins at
2 a.m.

**Snapshot the ring at the transition.** When `Phase` leaves `Ready` for any reason, copy the
wire ring into a second, non-circular buffer and stop writing it. That freeze is exactly the
mechanism `04_rows` already had for its DEAF watchdog — which is why the retirement of that
freeze should be a *move*, not a deletion. Expose it as:

* `/deregistered.txt` — the frames immediately **before** the drop, the phase it fell from,
  the reason the FSM recorded, and the driver counters at that instant
* a count and a timestamp of the last N transitions on the status page, so a soak reports
  "14 drops" instead of the owner discovering it by reading a log

`Phase` gives the exact hook: one place where `Ready` is left, one place to snapshot. Without
it that hook is spread across the twenty-two booleans, which is a large part of why nobody
noticed fourteen re-openings during a soak that looked perfect.

## What must not be lost

The comments. This codebase's value is disproportionately in the prose that says *why* a byte
is what it is and *what was measured to find out*. A refactor that produces cleaner code and
drops the evidence would be a net loss — the flags can go, but the reasons they existed, and
what settled them, belong in the new code.
