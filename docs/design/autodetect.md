# Panel auto-detect: what the library would need

Status: **design only, not implemented.** The owner deferred auto-detect until there is more
glass time. This file says what AffaDisplay would add so that an application (MegaOpen) can
run the probe, and what stays in the application.

## The probe the application wants

From MegaOpen's BACKLOG ("Auto-detect the panel family", researched 2026-10-21):

1. **Listen** on the panel bus for about 1 s, link in listen-only mode, transmitting nothing.
   - Another master on the bus (an OEM radio sending `0x3AF` or `0x3DF`): stay silent, report it.
   - A cluster (`0x3CF` DLC 1 `69`, never `61 11`): report it.
   - Otherwise a panel is pinging, or the bus is quiet.
2. **Try Carminat**: switch the link to normal mode and build a `CarminatDisplay`. If it does
   not register in time, destroy it and build an `UpdateListDisplay` on the same link.
3. **Remember** the winner in NVS and use it directly at the next boot.

A passive check cannot tell Carminat from UpdateList. Before registration a panel sends the same
`0x3CF` ping and `61 11 01` either way, and the bench panel accepts both openings. The family is
set by which opening we send, so step 2 is unavoidable.

## What already works without library changes

| Need | Today |
| --- | --- |
| listen-only, then normal | `TwaiLink::setListenOnly(bool)` (a driver reinstall) |
| see every frame while listening | `TwaiLink::onRaw(tap)`; no display object needed |
| a display per family at run time | both families compile into one image; build either on the heap |
| progress of an attempt | `phase()`, `syncState()`, `registered()` |

So the probe can be written entirely in the application. Three things are awkward without
library help, and each is small.

## Proposed additions

### 1. `affa::BusSniffer`: classify the bus from frames

The classification rules are protocol knowledge: which ids are a master's, what a cluster ping
looks like, the DLC 1 and DLC 2 `0x3CF` frames. Today an application would copy those constants
out of three panel folders. A pure classifier keeps them in the library and makes them
host-testable against the captures.

```cpp
// src/probe/BusSniffer.h: header-only, no I/O, builds for platform = native.
namespace affa {

enum class BusVerdict : uint8_t {
  Listening,        // less than the minimum window has passed
  Quiet,            // nothing heard in the window
  PanelPinging,     // 0x3CF pings and/or 61 11: a panel waiting for a master
  Cluster,          // 0x3CF DLC 1 0x69 and no 61 11 in the window
  ForeignCarminat,  // another node transmits 0x3AF: a Carminat-side master is present
  ForeignUpdateList // another node transmits 0x3DF
};

class BusSniffer {
public:
  explicit BusSniffer(uint32_t windowMs = 1000);
  void reset(uint32_t nowMs);
  // Feed every received frame (Direction::Rx and !f.fromSelf); ignores the rest.
  void feed(const Frame& f, Direction d, uint32_t nowMs);
  BusVerdict verdict(uint32_t nowMs) const;
  // The evidence, for a health line: counts per class of frame.
  struct Seen { uint16_t pings, syncRequests, clusterPings, master3AF, master3DF, other; };
  Seen seen() const;
};

}  // namespace affa
```

- **Precedence:** foreign master beats cluster, and cluster beats panel. A radio that also wakes
  a panel is still a radio, and transmitting there would fight it.
- **No gates:** it includes only the constant headers, so it builds whatever `AFFA_PANEL_*`
  says, including in a `<AffaTwai.h>`-only build.
- **It can keep running after the probe.** Fed from the raw tap in normal mode, a received
  `0x3AF`/`0x3DF` that is not ours means a second master appeared later (for example, the radio
  powered up after the board). The application can then go silent. Our own frames come back as
  `Direction::Tx` or `fromSelf`, so they don't count.
- **Size:** about 80 lines, no heap, no callbacks.

### 2. `openingStats()`: did this family get anywhere?

`phase()` cycles: a wrong family goes Announced → HelloPending → Registering, falls back to the
announce, and starts again (`test_unanswered_registration_falls_back_to_the_announce`). Sampling
`phase()` from another task can miss the fallback entirely. A monotonic counter cannot.

```cpp
// AffaDisplayBase, plain reads (atomics), safe from any task.
struct OpeningStats {
  uint16_t hellos;                  // hello bursts sent (the panel asked: 61 11 seen)
  uint16_t registrationsUnanswered; // registration rounds that fell back to the announce
  uint32_t firstHelloMs;            // IClock time of the first hello; 0 = none yet
  uint32_t registeredMs;            // first FuncsReg; 0 = never
};
OpeningStats openingStats() const;
```

The application's rule for an attempt, with every timing value its own:
- `registeredMs != 0`: this family works.
- `hellos == 0` after the window: the panel never asked. Nothing to decide yet; keep waiting or
  report "no panel".
- `registrationsUnanswered >= 1`, or no FuncsReg within N ms of `firstHelloMs`: wrong family, so
  try the other one.

This is a counter in `AffaSync.cpp` at two places that already exist, plus a host test that
scripts an UpdateList-only panel (one that ignores `0x3AF`) on `LoopbackLink`.

### 3. `shutdown()`: destroy a display cleanly

Rebuilding means deleting a live display. Today `~AffaDisplayBase()` is the default destructor,
so:
- tickets still queued or posted never complete;
- an application waiting on one waits forever;
- a frame half-sent stays half-sent.

```cpp
// Owner task only, with no AffaTask running (stop() it first) or from the owner's own loop.
// Stops every transfer at a frame boundary, completes every outstanding ticket with
// Result::Cancelled, releases the long-payload pool and clears the poll owner. Afterwards
// the object may be deleted, and nothing it sent is left mid-transfer.
void shutdown();
```

The pool and the dispatch ring are members of the display, so deleting it frees them. The link
and the log sink are not owned by the display and stay as they are.

## What stays in the application

- The sequence, its timings (listen window, attempt timeout) and the order Carminat →
  UpdateList.
- Remembering the winner (NVS, like MegaOpen's `can2Detected`) and the `displayType=auto`
  setting.
- What to tell the user: health codes, the "a radio is on this bus" message.
- Re-probing: when, and whether a stored winner is trusted after a session loss.

## Sequence (MegaOpen, `displayType=auto`, no stored winner)

```
link.begin({listenOnly = true}); link.onRaw(sniffer.feed)
  ... 1 s ...
switch (sniffer.verdict()):
  ForeignCarminat / ForeignUpdateList -> stay listen-only, health "another master", done
  Cluster                             -> health "cluster", done (no driver for it yet)
  PanelPinging / Quiet                -> continue
link.setListenOnly(false)
d = new CarminatDisplay(link, clock); d->begin(); own loop polls d
  ... until registeredMs, or unanswered >= 1, or timeout ...
registered   -> store "carminat", done
wrong family -> d->shutdown(); delete d
                (quiet gap: see open questions)
                d = new UpdateListDisplay(link, clock); d->begin()
                registered -> store "updatelist"; else health "panel did not register"
```

## Open questions (glass tests, before any of this is enabled)

1. **Switching families in one power cycle.** After a Carminat opening that did not complete, does
   a panel accept the UpdateList one at once, or only after its own session times out? The bench
   panel is universal and always ends at Carminat, so the fallback path has never run on
   hardware. Until it has, the application should leave a quiet gap of at least
   `AFFA_PEER_TIMEOUT_MS` before the second attempt.
2. **The attempt timeout.** It has to cover the bounded `1C1` wait (`AFFA_AWAIT_PEER_MS`, 400 ms)
   plus registration. The BACKLOG's 250 ms is shorter than the wait alone. Measure it on glass.
3. **A radio that is silent at boot.** The 1 s listen misses a radio that powers up later. Item 1
   allows continuous sniffing; the policy for that case is the application's.
4. **Cost of the listen window.** The first draw comes about 1 s later. Once a winner is stored,
   later boots skip the probe.

## Tests the library would add

- `test_sniffer`: verdicts from vectors. A panel's ping and `61 11`, the cluster capture's DLC 1
  `69`, an OEM radio's `0x3AF` traffic, DLC 1/2 `0x3CF`, and our own echoed frames ignored.
- `test_opening_stats`: an UpdateList-only scripted panel against `CarminatDisplay` counts
  unanswered registrations. A Carminat panel gives `registeredMs`.
- `test_shutdown`: queued, posted and in-flight tickets all complete `Cancelled`, and the pool is
  empty afterwards.

Estimated size: about 200 lines of library code and 3 host suites, with no change to anything an
existing application calls.
