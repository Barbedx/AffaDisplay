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

**So the two families do not collapse into one behaviour, and the three knobs in the profile
above are exactly what they are for.** `Opening`, `registerInOpening` and a peer-channel gate
carry the whole difference. Forcing UpdateList onto the Carminat rules would break a family
nobody can currently test on hardware — which is the failure this plan exists to avoid.

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

1. **Extract the UpdateList byte truth** from the reference. Report only, no code.
2. **Collapse the dead flags.** Pure deletion of settled hedges. 255 tests green.
3. **Introduce `Phase`** alongside the booleans, derived from them, and assert they agree.
4. **Invert it** — `Phase` becomes the source of truth, the booleans are deleted.
5. **Split the files.** Mechanical; no behaviour change in the same commit.
6. **Unify UpdateList** onto the profile, guided by step 1.
7. **Flash and soak** after 4 and after 6. A green suite has already let a broken handshake
   through this session; only glass counts.

## What must not be lost

The comments. This codebase's value is disproportionately in the prose that says *why* a byte
is what it is and *what was measured to find out*. A refactor that produces cleaner code and
drops the evidence would be a net loss — the flags can go, but the reasons they existed, and
what settled them, belong in the new code.
