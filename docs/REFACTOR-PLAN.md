# Refactor plan — one state machine, two families, no hedges

**Status: design, not yet implemented.** Written 2026-08-04, immediately after the Carminat
protocol was settled against OEM captures and proven on glass for 1 h 36 m at 24 912 screens
with every error counter at zero.

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

### The one finding to act on first

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
2. **Collapse the dead flags.** Pure deletion of settled hedges. 255 tests green.
3. **Introduce `Phase`** alongside the booleans, derived from them, and assert they agree.
4. **Invert it** — `Phase` becomes the source of truth, the booleans are deleted.
5. **Flash and soak Carminat.** Nothing may proceed on a red bench.
6. **Add the drop snapshot** — freeze the ring on leaving `Ready`, expose
   `/deregistered.txt`. Cheap once `Phase` exists, and it turns the open question above into
   a measurement instead of a mystery.
7. **Split the files.** Mechanical; no behaviour change in the same commit.
8. **Move UpdateList onto the shared rules** — registration in the opening, peer-channel gate,
   `61 11` teardown, power before render. `helloRequiresAnnounce = false` is its only
   remaining difference. `test_updatelist_wire` must stay green: the BYTES do not change, only
   the sequencing.
9. **Flash and soak again.** A green suite has already let a broken handshake through twice
   this session; only glass counts.

> Step 8 changes a family that cannot currently be tested on hardware. The mitigation is that
> every byte it emits is already pinned by golden vectors, and what changes is *when* frames go
> out, not *what* is in them. If an UpdateList panel ever reaches the bench and stalls, the
> first knob to turn is `replyToPing` (see above), and the second is reverting step 8's
> registration timing to lazy.

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
