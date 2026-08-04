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

## UpdateList

The working hypothesis, from the owner: **same logic, different bytes** — with a shipped
reference implementation in `MeganeCAN/src/display/UpdateList/` that drove a real panel.

This is a hypothesis backed by an implementation, not a guess, and the profile above is
designed to express it: if UpdateList really is the same machine, it is `SyncProfile` data plus
possibly `Opening::PanelLeads`. **But the byte-level facts must be extracted from that reference
before any unification**, and where the reference and our current port disagree, that disagreement
is the finding.

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
