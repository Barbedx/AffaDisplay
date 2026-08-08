# AffaDisplay — public API and configuration surface

Namespace `affa`. **C++17**, `-fno-exceptions`, `-fno-rtti`. Public signatures use
`const char*`; Arduino `String` never appears in one, and never appears in `core/` or
`util/` at all.

> **THIS DOCUMENT DOES NOT COPY DECLARATIONS, and that is the most important thing about
> it.** It used to. §2 was 1691 lines of pasted headers and §5 was `AffaConfig.h`
> reproduced in full — and by the time 2.0 landed, both described a library that no longer
> existed: `proto/`, `widget/`, `Esp32CanLink`, `MenuModel`, `UpdateListMenuDisplay`,
> `subscribe()`, `EventKind` and four build gates were all still documented here and all
> gone from `src/`. A copy of the source is a second source that nothing checks.
>
> So: **the headers are the declarations.** They are commented at the density this project
> uses everywhere else, and `src/` is 8.5k lines — smaller than this file used to be. What
> lives here is only what a header cannot say: the guarantees that span several files, the
> ones a test enforces, and the reasons a shape is odd.

Where this document and a comment in the source disagree about a **contract**, this
document wins until it is amended. Where they disagree about a **declaration**, the source
wins, always — see the box above.

Companion documents:

* `docs/WIRE.md` — byte-level frame layouts and where each byte was observed.
* `docs/NOTES.md` — what is known, how it is known, and what is still a guess.
* `docs/ESP32CAN-CONTRACT.md` — driver ownership, RX/TX and recovery.
* `docs/API.md §7` — why the surface has the shape it now has.

---

## 0. The four defects this API exists to make impossible

Everything unusual in the shapes below traces back to one of these. They are stated
first so that a reviewer can check the design against its purpose.

1. **The sync watchdog counted `tick()` calls, not milliseconds.** `static int8_t
   timeout = SYNC_TIMEOUT` decremented once per call meant "five seconds" only if the
   caller happened to tick at exactly 1 Hz. From a free-running loop it expired in
   milliseconds, tore down `FUNCSREG`, and restarted the handshake forever.
   *Fix in the API:* `IClock` exposes `millis()` and nothing else, all periodic
   behaviour is a wall-clock deadline inside `poll()`, and `poll()` is specified as
   frequency-independent (§4.4). A host test enforces it.

2. **A 2000 ms blocking ACK wait sat inside the only path that could deliver the ACK.**
   `affa3_do_send` spun on `delayMs(1)` waiting for a frame that only arrived if
   something drained RX — and the drain was downstream of the spin.
   *Fix in the API:* `ICanLink` is a **pull** port (`recv(Frame&)`), the transmit path
   is a state machine advanced by `poll()`, and there is **no `delayMs()` anywhere in
   the library**. A send cannot wait, so a send cannot deadlock.

3. **`delay(100)` in the sync-request branch**, and a duplicated copy of the whole sync
   FSM in `CarminatDisplay::tick()` and `UpdateListBase::tick()` — so both copies
   carried both defects. *Fix in the API:* one FSM in `AffaDisplayBase`, parameterised
   by `SyncProfile`; the `delay(100)` is deleted, not replaced.

4. **A queued render could not be superseded, and a key could not overtake one.** This
   defect has not bitten yet only because the extracted code had no queue at all — it
   blocked instead. Turning the blocking send into a queue *introduces* the defect
   unless it is designed out on day one: an application rendering a counter at 10 Hz
   leaves a backlog of stale values, and the panel visibly keeps counting for a second
   after the user pressed Pause and the library correctly received the key. It reads
   like a key-handling latency bug and is really a queueing bug.
   *Fix in the API:* `poll()` drains RX and delivers keys **strictly before** it pumps
   the transmit FSM (§3b.3); a render supersedes a queued-but-not-yet-started render of
   the same `RenderSlot` instead of stacking behind it (§3b.4); and preemption is
   explicit through `abortPending()` and `Priority::Urgent` (§3b.5). All three are
   specified as testable guarantees, not as intentions.

---


---

## 1. What is in `src/`

Thirty-six files, 8.5k lines. **`Host` means it compiles for `platform = native` against
nothing but the C++17 standard library** — no `<Arduino.h>`, no `<driver/twai.h>`, no
FreeRTOS, no `String`, no `std::vector`, no `std::function`, and no heap after `begin()`.
That property is why the host suite can test the interesting half of a CAN driver on a
laptop, and it is worth more than any single feature in here.

### The umbrella and the gates

| File | What it is |
| --- | --- |
| `AffaDisplay.h` | The only header a consumer includes. Pulls `AffaConfig.h`, `core/`, `util/`, and each selected panel behind its own gate. Declares nothing of its own. |
| `AffaConfig.h` | Every `#define` gate and sizing knob, plus a written record of the gates that were **deleted** and why. Includes nothing; included first by everything. |

### `core/` — the part that knows nothing about a panel

| File | What it is |
| --- | --- |
| `AffaTypes.h` | `Frame`, `Key`, `KeyEdge`, `Result`, `Submitted`, `SyncState`, `Feature`, `NavCommand`, `TxTicket`, `RenderSlot`, `Priority`, `TxOptions`, `Stats`, `CbKind`. |
| `ICanLink.h`, `IClock.h` | The two ports. `recv(Frame&)` is a **pull**; `millis()` is the only thing a clock does. |
| `IDisplay.h`, `IPanel.h` | The panel-agnostic surface an application holds, and the four-primitive rendering port. |
| `PanelGeometry.h` | What a given glass can actually show, queried rather than assumed. §6. |
| `AffaConstants.h` | Constants shared by every panel: ISO-TP opcodes, `kReplyFlag`, ACK bytes. No panel IDs. |
| `AffaSyncProfile.h` | `SyncProfile` — the opening expressed as **data**. The per-family instances live in the panel folders. |
| `AffaRing.h` | `AffaRing<T,N>`, lock-free SPSC. |
| `AffaDispatch.h` | `AffaMpsc<T,N>` — bounded multi-producer queue, CAS claim plus a publish-last release store. This is what makes a render from any task safe. |
| `AffaDisplayBase.h` | The whole class in one header; four `.cpp` files because it spans four unrelated jobs. |
| `AffaDisplayBase.cpp` | Lifecycle, `poll()` orchestration, the RX drain, the key decoder, the capability defaults. |
| `AffaSync.cpp` | The opening: the `Phase` table, the announce, the hello burst, the peer-channel gate, the heartbeat, the watchdog. |
| `AffaTx.cpp` | The transmit queue, ISO-TP segmentation, flow control, retries, registration probes — and **`enqueue()`, the one place a cross-task render becomes safe.** §4. |
| `AffaObserve.cpp` | `txFrame()`, the choke point every frame passes through in both directions, and the frame tap. |
| `AffaBaseInternal.h` | Shared detail between those four `.cpp` files. Not public. |

### Panels — each gated, each compiling to an empty object file when unselected

| Folder | What it is |
| --- | --- |
| `carminat/` | `0x3AF` sync, `0x151`/`0x1F1` data, `0x1C1` keys. The largest family: text, time, menus, popups, info screens, lists, the 48×48 nav bitmap. Gated on `AFFA_PANEL_CARMINAT`. |
| `updatelist/` | AFFA2: `0x3DF` sync, `0x121`/`0x1B1` data, `0x0A9` keys. **One `setText` encoding for every glass in the family** — `UpdateListDisplay.h` records why the second one was a misreading of a command flavour. Gated on `AFFA_PANEL_UPDATELIST`. |
| `cluster/` | The instrument cluster: `0x3AF` again, but `59`/`5A` where Carminat has `61`/`62`. **Not verified on hardware, and its opening cannot complete** — `docs/NOTES.md` §9.2a. Gated on `AFFA_PANEL_CLUSTER`. |

### The rest

| File | What it is |
| --- | --- |
| `link/CanCommonLink.h` | The `ICanLink` over `can_common` / `esp32_can`. Header-only, gated. |
| `link/LoopbackLink.h` | The test double: records TX, injects RX, optional synthetic ACK. Host. |
| `util/AffaLog.{h,cpp}` | `ILogSink` and the `AFFA_LOG*` macros. `.cpp` body entirely gated. |
| `util/AffaText.{h,cpp}` | `toAscii`, `normalizeTitle`. Pure C API, no allocation. |
| `rtos/AffaTask.{h,cpp}` | `TaskOptions`, `Status`, `AffaTask`. **The only file in the library that includes FreeRTOS**, and the one directory a non-FreeRTOS port omits. §4b. |

**The fences, and the build enforces them rather than good intentions.**

* `<driver/twai.h>` appears nowhere. The driver is reached through `can_common`.
* `<Arduino.h>` is permitted in panel `.cpp` files only, and only if something genuinely
  needs it. Prefer `<cstring>` / `<cstdio>`.
* `src/rtos/` is the single exception to Host, and it is fenced: FreeRTOS headers appear
  in `rtos/` and nowhere else. The poll-owner guard `core/` needs for the owned-task mode
  is a **function pointer** (`AffaDisplayBase::TaskIdFn`) precisely so that `core/` never
  learns what a task is — and it is host-tested through that seam (`test_owned_task`).
* `core/` and `util/` are compiled by `test/` for `platform = native`. If a change breaks
  that build, the change is wrong, not the test.

---

## 2. The surface you call

**The declarations are in the headers.** What follows is the shape, so you know which
header to open.

```cpp
#include <AffaDisplay.h>

affa::CanCommonLink   link;
ArduinoClock          clk;                   // your IClock: one millis()
affa::CarminatDisplay panel(link, clk);
affa::rtos::AffaTask  task;

panel.begin();
task.start(panel);                           // AFFA_ENABLE_TASK=1; otherwise call poll()

panel.setText("HELLO");                      // from ANY task — see §4
```

**One handle.** An application holds an `IDisplay&`. Everything it can ask of a panel is
on that interface or on the concrete panel class; there is no second object to keep in
sync, no controller, no widget and no model. A library that implements a transport does
not own your UI, which is why 2.0 deleted the ones that had crept in (§7).

**Every render returns `Submitted`.** It carries a `TxTicket` and a `Result`, converts to
`bool`, and is `[[nodiscard]]` — so "was it accepted" and "which transfer was it" have one
answer, and that answer cannot be dropped silently.

```cpp
if (const affa::Submitted s = panel.setText("HELLO")) { /* s.ticket identifies it */ }
else                                                  { /* s.result says why not   */ }
```

**Renders are enqueued and never block.** There is no `delay()` anywhere in the library
and no send waits for an ACK. §3 gives the `Result` vocabulary; §3b gives the ordering and
preemption guarantees; §4 gives the one rule that makes a render safe from another task.

**Ask the panel what it can show** — `supports(Feature)` for capabilities and
`panelGeometry()` for dimensions (§6). Code that assumes 26-character rows produces
nothing an 8-cell segment display can render.
## 3. `Result` semantics

Two disjoint populations. The set a call can return **at enqueue** is not the set that
can arrive **through `onComplete`**, and confusing them is how "it returned Ok so it
displayed" bugs get written.

| Value | Returned by an enqueue call | Delivered via `onComplete` | Meaning |
| --- | :---: | :---: | --- |
| `Ok` | yes | yes | *Enqueue:* accepted into the queue, nothing has been transmitted yet. *Complete:* the panel acknowledged the last frame with `0x74`. |
| `NoSync` | yes | no | Not passive, and `SyncState::Failed` is set. Nothing was queued. |
| `UnknownFunc` | yes | no | `funcId` is not in this panel's function table. Nothing was queued. |
| `SendFailed` | no | yes | `ICanLink::send()` refused a frame, or the panel answered with something that was neither `0x74` nor `30 01 00`, or it answered `30 01 00` when no bytes remained. |
| `Timeout` | no | yes | No ACK within `AFFA_ACK_TIMEOUT_MS` for the frame in flight. |
| `TooLong` | yes | no | `len > AFFA_MAX_PAYLOAD`, or a text argument exceeds the panel's field. Nothing was queued. |
| `QueueFull` | yes | no | `AFFA_TX_QUEUE_DEPTH` reached, counting the registration jobs the call would have to push ahead of itself. Nothing was queued. |
| `NotSupported` | yes | no | `supports(Feature)` is false for this call on this panel. Nothing was queued. |
| `BadArgument` | yes | no | Null pointer, `len == 0`, row/index out of range. Nothing was queued. |
| `LinkDown` | yes | yes | *Enqueue:* `ICanLink::isLive()` was already false. *Complete:* it went false while the job was in flight. |
| `Cancelled` | no | yes | The job was discarded: sync was lost, `begin()` was re-run, or a registration job ahead of it failed and propagated. |
| `Aborted` | **no** | **yes** | The application discarded the message before any byte of it reached the wire, through `abortPending()`, through `abortAll()`, or by enqueuing a newer message for the same `RenderSlot` (§3b.4). `Aborted` is an `onComplete`-only value: no enqueue call ever returns it, because a call cannot abort itself. It is the caller's own decision reported back, which is why it is distinct from `Cancelled` (the library discarded the job because the link or the sync went away). |

**Read the two columns as two different questions.** "Was it accepted?" is answered
synchronously and can only be `Ok`, `NoSync`, `UnknownFunc`, `TooLong`, `QueueFull`,
`NotSupported`, `BadArgument` or `LinkDown`. "Did the panel display it?" is
answered later, through `onComplete`, and can only be `Ok`, `SendFailed`, `Timeout`,
`LinkDown`, `Cancelled` or `Aborted`. `Ok` and `LinkDown` are the only two values that
appear in both columns, and they mean different things in each.

Every ticket returned non-zero by an enqueue call completes exactly once, with exactly
one of the six completion values. There is no path on which a ticket is issued and
never completed — including `abortAll()` on a job in flight, which completes it at the
frame boundary.

`lastResult()` returns the `Result` of the most recently **completed** ticket, except
immediately after a rejected enqueue, where it holds the rejection reason and
`lastTicket()` is unchanged. If you need to distinguish, compare `lastTicket()`.

A render call returns only the acceptance verdict, not the ticket. To follow one to
completion, read `lastEnqueued()` immediately after the call:

```cpp
if (display.setText("HELLO") == affa::Result::Ok) {
  const affa::TxTicket t = display.lastEnqueued();   // match this in onComplete
}
```

**`pressKey()` and `nav()` are not enqueue calls** and their `Result` is a third thing
again: it reports whether the *intent was delivered*, not whether anything was queued and
not whether anything changed on screen. They return `Ok`, `NotSupported` (no menu, no key
transmit id, or a hold edge on a wheel code with a source that includes `Wire` — see the key decoder in `AffaDisplayBase.cpp`),
`LinkDown` or `SendFailed` (the `Wire` half only). Any rendering they cause is enqueued by
the menu on their behalf and reports through `onComplete` under its own ticket, which
`lastEnqueued()` will hold immediately after the call.

Tickets are issued strictly increasing, but they **do not necessarily complete in issue
order**, and code must not assume they do. `Priority::Urgent` overtakes queued `Normal`
work, and a superseded render completes `Aborted` ahead of the message that replaced
it. Anything an application builds on completion order must therefore use `onComplete`
and match the **exact ticket** it was given, never compare ticket numbers. (`Result::Busy`
used to appear in the table above, and a one-slot ticket watcher used to exist inside the
base class; both were there only for `sendBlocking()`, and all three are gone.)

### 3.1 Delivery: the library is the recovery layer, not your application

**Since 0.3.0 an application does not retry, does not hold a value while the panel is away,
and does not re-register after a resync.** All three used to be the consumer's job, every
consumer wrote them, and the failures were expensive enough to be worth listing:

| What the app used to own | What the library does now |
| --- | --- |
| retry a `Timeout` | retried up to `AFFA_TX_MAX_RETRIES` with a doubling backoff from `AFFA_TX_RETRY_MS`, capped at `AFFA_TX_RETRY_MAX_MS`. The application is told **once**, at the end, if all attempts fail. |
| leave the panel alone after a torn transfer | a job whose bytes had already started going out waits an extra `AFFA_TX_DIRTY_QUIET_MS` before the next attempt, so the panel's reassembler hears silence rather than a fresh first frame landing inside the old message |
| keep the value and re-issue it when sync returns | a render made while the link is down is **accepted and held** for up to `AFFA_TX_HOLD_MS`, then started when the link is usable |
| notice `PeerLost` and re-render everything | the queue **survives** the peer. Only the registration is invalidated; the renders are held |
| re-register after a resync | `pumpTx()` splices the `0x70` burst in front of a held payload when `FUNCSREG` is missing |

**What is still refused at the call site**, because waiting could never help: `BadArgument`,
`TooLong`, `UnknownFunc` (your mistake, fix the call) and `QueueFull` (backpressure, and the
one thing the library cannot absorb without blocking you).

**What is never retried**, and the distinction matters:

* `SendFailed` — the panel **answered**, and the answer was neither DONE nor PARTIAL. That
  is a disagreement about *content*; re-sending identical bytes gets the same answer three
  more times and buries the one diagnostic that says your builder is wrong. Silence is
  transient, rejection is not.
* `Aborted` / `Cancelled` — deliberate. The application asked, and retrying would be the
  library overruling the caller.

Set `AFFA_TX_MAX_RETRIES = 0` and `AFFA_TX_HOLD_MS = 0` to get the pre-0.3.0 contract back
exactly: one attempt, rejections at the call site, and the recovery is yours again.

#### 3.1.1 What this costs you

**One ticket now covers several seconds and several attempts.** `onComplete` fires once per
ticket, as it always has, but the gap between enqueue and verdict can now be
`AFFA_TX_MAX_RETRIES × (AFFA_ACK_TIMEOUT_MS + backoff)` — about eleven seconds at the
defaults. If you were using completion latency as a health signal, use
`Stats`/`AffaTask::Status` instead.

**Head-of-line, deliberately.** A job waiting out a backoff stalls the queue behind it
rather than being skipped, because skipping would reorder renders the application issued in
sequence. `Priority::Urgent` still overtakes — a retrying job has not started, so an urgent
one is spliced in front of it.

**Coalescing still wins.** A newer render of the same `RenderSlot` replaces a waiting one
and gets a fresh retry budget, but **not** a fresh backoff: the backoff protects the panel,
and the panel does not care that you changed your mind.

### 3.1.2 A rejection is INSTANT, so a retry needs a deadline

`NoSync`, `LinkDown`, `UnknownFunc`, `BadArgument`, `TooLong` and `QueueFull` are all
decided inside `enqueue()`, before anything touches the wire. The call is a few dozen
instructions and it returns *now*.

That is the right behaviour — a render call must never block — and it is a trap for the
caller, because the obvious reaction to a failed render is to try again:

```cpp
// WRONG. Spins at loop rate the moment the panel is unplugged.
if (g_clockPending && display.setTime("1000") == affa::Result::Ok) g_clockPending = false;
```

With the panel disconnected, that costs **one failed render per loop iteration**: measured
in the 17_mediascreen soak, 4 800 failures and 170 log lines per second in 90
seconds, which then pushed the one line that explained it — `peer lost` — out of the log
ring. It is failure mode #2 from `docs/NOTES.md §3` §2, written a second time by
someone who had just read it.

**§3.1 is the reason that example no longer needs the loop at all** — the library holds and
retries. This section survives because the *shape* is still a trap for anything the library
cannot absorb for you: a `QueueFull` from `AffaTask`, or your own producer reacting to a
verdict. If you write a retry, give it a deadline.

**Every retry site needs a deadline against `IClock::millis()`, exactly like everything
else in this library:**

```cpp
if (g_clockPending && affa::expired(now, g_retryAt)) {
  if (display.setTime("1000") == affa::Result::Ok) g_clockPending = false;
  else                                             g_retryAt = now + 1000;
}
```

And log the *transition*, not the attempt: one line when it starts failing, one when it
recovers. The same applies to the completion side — a render that is accepted and then
completes `Timeout` must re-arm the same deadline, not retry immediately.

---

## 3b. Latency and preemption

A key press must land **now**, not after the queue drains. This is a headline guarantee
of the library, specified here in full and pinned by tests, because getting it wrong is
invisible in code review and obvious on the bench.

### 3b.1 Two latencies, two different questions

They get conflated, and then the wrong one gets optimised. Name them separately and
measure them separately.

| | Definition | Who owns it |
| --- | --- | --- |
| **L1 — key delivery** | From the key frame entering the RX ring to the application's `KeyCb` returning from its first statement. | **The library, entirely.** |
| **L2 — reaction on the wire** | From the same instant to the first byte of the application's reaction leaving `ICanLink::send()`. | Shared: the library owns the queueing, the panel owns the ACK turnaround, the application owns what it does in the callback. |

```
key frame in ring ──L1──► KeyCb fires ──► app enqueues ──L2-L1──► first byte on the wire
                    │                                        │
        bounded by the poll period                bounded by the frame in flight,
        and by NOTHING ELSE                       not by the queue behind it
```

**L1 is bounded by the poll period and nothing else.** Not by the transmit queue depth,
not by the length of a message in flight, not by whether the TX FSM is waiting on an
ACK with a 2000 ms deadline. That is the guarantee in §3b.3.

**L2 cannot be bounded by the library alone**, and any document that claims otherwise
is lying: the message on the wire is abandoned only at a frame boundary, so the
reaction waits for at most one ACK round-trip (or one `AFFA_ACK_TIMEOUT_MS`, if the
panel has gone away). What the library *does* guarantee about L2 is that **nothing
queued behind the in-flight message contributes to it**, provided the application uses
either coalescing (automatic, §3b.4) or `Priority::Urgent` / `abortPending()`
(explicit, §3b.5).

### 3b.2 Three mechanisms, all of them the library's job

1. **Ordering inside `poll()`** — RX drain and key delivery strictly before the
   transmit pump. Fixes L1.
2. **Latest-value-wins coalescing** — a render supersedes a queued, not-yet-started
   render of the same slot instead of stacking behind it. Fixes the *stale backlog*,
   which is the part that actually looks like a latency bug.
3. **Explicit preemption** — `abortPending()`, `abortAll()`, `Priority::Urgent`. Fixes
   the residual L2 for applications that need it.

Mechanism 2 is the one that is easy to leave out and expensive to leave out. Without
it, the key arrives on time, the application reacts on time, and the panel *still*
keeps counting for a second — because a dozen stale counter values are queued in front
of the reaction. It reads as a key-handling defect and it is not one.

### 3b.3 The ordering guarantee

`poll()` performs, in this order, on every single call, with no exceptions and no
early-out that can skip a step:

```
poll():
  1. pumpRx()    while (_link.recv(f)) { sync? ack? key? -> KeyCb / CompleteCb }
  2. pumpSync()  heartbeat, sync request, peer watchdog
  3. pumpTx()    build and send at most one frame; check the ACK deadline
  4. onPoll()    panel hook
```

There is no configuration, no priority, no queue state and no error path that reorders
these. `pumpTx()` is never entered before `pumpRx()` has returned. Stated as the
sentence a test asserts:

> **The number of `poll()` calls between a key frame entering the RX ring and the key
> callback firing is exactly one, regardless of transmit queue depth or of any message
> in flight.**

`test/test_latency` enforces it as a **poll count**, at both ends of the range: with an
empty queue, and with a 96-byte `showMenu` in `WaitAck` and every one of the
`AFFA_TX_QUEUE_DEPTH` slots occupied (the next `enqueue` returning `QueueFull` is asserted
too, so the queue really is full). In both cases a `0x1C1` key frame injected into
`LoopbackLink` reaches `KeyCb` after **exactly one** `poll()`, and the in-flight job is
untouched. A five-key burst behind an ACK is delivered whole in one poll. A regression that
moves `pumpTx()` above `pumpRx()` fails every one of them.

Two consequences worth stating outright, because they are the reason the ordering is
specified rather than assumed:

* A `WaitAck` with 1900 ms left on its deadline delays **nothing** on the receive side.
  The TX FSM never waits; it checks a deadline and returns.
* `pumpRx()` drains the ring to empty, not one frame per call. A burst that arrived
  between two polls is delivered in full on the next one, in arrival order, so a key
  that arrived behind an ACK is still delivered in that same poll.

### 3b.4 Coalescing: latest value wins, per slot

**The rule.** At `enqueue()`, if `opt.coalesce` is true and `opt.slot != RenderSlot::None`,
scan the queue for a job that satisfies **all** of:

* `started == false` — not one byte of it has gone to `ICanLink::send()`;
* `slot == opt.slot`;
* `funcId == funcId`;
* `coalesce == true`;
* `kind == Payload` — Registration jobs are never coalesced (see `admit()` in `AffaTx.cpp`).

If one is found, **replace its payload in place**: copy the new bytes over it, give it
the new ticket, and complete the **old** ticket immediately with `Result::Aborted`.
Otherwise append normally. At most one job is ever replaced — the queue can never hold
two coalescable jobs for the same slot, by induction from this rule.

**Queue position is inherited, not reset.** The replacement keeps the superseded job's
position, so ordering relative to *other* slots is exactly what the application asked
for. The one exception: if the new job is `Urgent` and the superseded one was `Normal`,
the entry is also moved to the urgent insertion point (§3b.5) — a promotion cannot be
silently ignored.

**"Not yet started", defined once and precisely.** A job is *started* from the moment
`pumpTx()` has passed its first frame to `ICanLink::send()` and returned true, and it
remains started until `finishJob()` pops it. `TxJob::started` is the single authority;
no other condition (being at the head of the queue, the FSM being non-`Idle`, having a
non-zero `frameIndex`) is used anywhere to decide preemptability. A message that is
started is **never** touched by coalescing, and never has its bytes altered mid-
sequence. This is not a performance choice: rewriting the payload of a transfer whose
first frames are already at the panel would produce a screen assembled from two
different messages, and the panel has no way to detect it.

**Slot assignment.** Every render call passes a slot; an application calling `enqueue()`
directly chooses its own.

| Call | Slot | Note |
| --- | --- | --- |
| `setText` | `Text` | the counter case |
| `setTime` | `Clock` | |
| `showMenu` | `Menu` | the 96-byte screen |
| `highlightItem` | `Highlight` | deliberately **not** `Menu`: a highlight must not replace a pending full redraw, and vice versa |
| `showPopupText`, `hidePopup` | `Popup` | |
| `showFullscreenText` | `Fullscreen` | |
| `showConfirmBox` | `ConfirmBox` | |
| `showInfoPopup` | `InfoPopup` | |
| `setPower` | `Control` | |
| `enqueue(...)` | `None` by default | raw protocol send: never coalesced |

Note that a `hide` shares its slot with the matching `show`. That is intended: if
`showPopupText` is still queued when `hidePopup` is called, the net effect the
application asked for is "no popup", and the panel should never see the popup at all.
Latest instruction wins, per slot, is the whole semantic.

**Why the slot and not the funcId alone.** On Carminat, `showMenu`, `setText`,
`highlightItem` and `showPopupText` all transmit on `0x151`. Coalescing on the function
id would let a highlight eat a menu redraw. Coalescing on the slot *and* the function
id is the narrowest key that is still correct.

**Opting out.** `TxOptions::coalesce = false` on a specific message makes it neither a
replacer nor a replaceable. Use it when consecutive messages of the same slot are a
*sequence* rather than a *value* — an animation, or a deliberate flash where every
intermediate state must be seen. `AFFA_TX_COALESCE = 0` turns the mechanism off
library-wide; §3b.7 is what you get.

### 3b.5 Explicit preemption

```cpp
uint8_t abortPending();          // drop everything queued and not started
bool    abortAll();              // + abandon the in-flight job at a frame boundary
TxTicket enqueue(uint16_t funcId, const uint8_t* data, uint8_t len, TxOptions opt);
                                 // opt.priority = Priority::Normal | Priority::Urgent
```

**`abortPending()`** drops every `started == false` `Payload` job, reports
`Result::Aborted` through `onComplete` for each dropped ticket, and returns the count.
It does not touch the job on the wire, does not touch Registration jobs, and does not
transmit anything. It is the correct thing to call from a key callback when the key
invalidates whatever the application had queued — which is most keys.

Ordering inside `abortPending()` matters and is specified: **the queue is mutated
first, the callbacks fire second.** Consequently a nested `abortPending()` from inside
one of those `CompleteCb` invocations finds nothing to drop and returns 0, and the
recursion depth is bounded by `AFFA_TX_QUEUE_DEPTH` even if a callback enqueues and
aborts in a loop. This is the general rule for the whole library: *state first,
callbacks second* (§4.3).

**`abortAll()`** additionally sets `abandon` on the in-flight job. The FSM honours it in
`WaitAck` only, so the frame already handed to the link is transmitted whole and the
next frame of that job is never built; the job completes `Aborted` and the continuation
counter resets (see `admit()` in `AffaTx.cpp`). The panel is then holding a partial transfer. **Whether it
recovers cleanly on the next frame 0 has not been verified on hardware and must not be
assumed** — verify with `examples/17_mediascreen` before using `abortAll()` in an
application. Routine preemption does not need it.

**`Priority::Urgent`** inserts the new job after the last started job and after any
queued Registration job, and before the first queued `Normal` job. It therefore:

* never splits a message that is already on the wire;
* never overtakes function registration (the panel would reject the payload);
* overtakes any number of queued `Normal` renders;
* is FIFO among other `Urgent` jobs.

`insertIndexFor(Priority)` is the single function that computes this, and inserting in
the middle of the queue moves at most `AFFA_TX_QUEUE_DEPTH - 1` slots — three
`memmove`s of a `TxJob` at the default depth, inside `poll()`, on a queue that is
statically sized. That is the whole cost.

**Which to use.** Coalescing handles the repeated-render case with no application code
at all, and should handle it. Reach for `abortPending()` when the queued work is of a
*different* slot than the reaction (a queued clock update in front of a "PAUSED" text).
Reach for `Priority::Urgent` when the reaction must go first but the queued work is
still wanted afterwards. `abortAll()` is a bench and shutdown tool.

### 3b.6 The scenario, step by step

The user's scenario, exactly: an application renders a counter at 10 Hz; a 14-frame
menu render is on the wire; **Pause arrives while frame 3 of 14 is in flight.** The
counter must stop the moment the key is pressed.

Setup: `poll()` every 5 ms from the application task. Counter = `setText` of a 4-digit
value, a 22-byte payload → 3 ISO-TP frames, slot `Text`, `0x151`. Menu = `showMenu`, a
96-byte payload → **14 frames** (frame 0 carries 8 payload bytes, each continuation
carries 7: 8 + 13×7 = 99 ≥ 96), slot `Menu`, also `0x151`. The panel is assumed to ACK
within one poll period. `t = 0` at an arbitrary poll; the key frame lands in the RX
ring at `t = 6.2 ms`.

```mermaid
sequenceDiagram
    autonumber
    participant P as Panel
    participant L as RX ring
    participant D as AffaDisplayBase::poll()
    participant A as Application
    Note over D: P0 t=0 ms — menu frame 3/14 on the wire, "0342" queued
    P->>L: ACK 30 01 00 (frame 4)
    P->>L: 1C1 key frame — Pause (t=6.2 ms)
    Note over D: P2 t=10 ms — poll() begins
    D->>D: 1. pumpRx: ACK -> frameIndex=5
    D->>A: 1. pumpRx: KeyCb(Pause, Click)   [L1 = 3.8 ms]
    A->>D: counter stopped; abortPending()
    D->>A: CompleteCb("0342", Aborted)      [nested, no frame was sent]
    A->>D: setText("PAUSE", Urgent)
    D->>P: 3. pumpTx: menu frame 5/14 — the transfer is NOT split
    Note over D,P: P3..P11 — menu frames 6..14
    D->>P: P12 t=60 ms: "PAUSE" frame 1/3   [L2 = 53.8 ms]
```

| poll | t (ms) | 1. `pumpRx()` | 3. `pumpTx()` | queue afterwards |
| --- | --- | --- | --- | --- |
| P0 | 0 | ring empty | frame 3/14 → link; `WaitAck` | `[menu*(3), text "0342"]` |
| P1 | 5 | ACK `30 01 00` → `frameIndex = 4` | frame 4/14 → link | `[menu*(4), text "0342"]` |
| | 6.2 | — | — | **Pause key frame `0x1C1` enters the RX ring** |
| P2 | 10 | pops the ACK for frame 4, then pops the key frame → `decodeKey` → `routeKey` → the menu does not consume `Pause` → **`KeyCb` fires, `t ≈ 10.0 ms`, L1 = 3.8 ms.** The application stops incrementing and calls `abortPending()`: the queued `text "0342"` is dropped and its ticket completes `Aborted` in a nested `CompleteCb`. The application enqueues `setText("PAUSE")` with `Priority::Urgent`. | frame 5/14 → link — **the menu is not split** | `[menu*(5), text "PAUSE"(urgent)]` |
| P3…P10 | 15…50 | one ACK each | frames 6…13 | unchanged |
| P11 | 55 | ACK for frame 13 | frame 14/14 → link | unchanged |
| P12 | 60 | ACK `0x74` → menu ticket completes `Ok` | `"PAUSE"` frame 1/3 → link. **L2 = 53.8 ms**, all of it the menu finishing. | `[text "PAUSE"*(1)]` |
| P13, P14 | 65, 70 | ACK each | frames 2/3, 3/3 | `[]` → `Idle` |

**Where the counter actually stops: at P2, `t = 10 ms`.** Not because a frame stopped
going out — frames 5…14 of the menu keep going out, and they must — but because the
last counter value the panel will ever be told about is the one that completed *before*
the menu started. `"0342"` was queued and not started, so it is dropped and never
reaches the wire. Zero stale counter values are displayed after the key. Had the
application not called `abortPending()`, coalescing alone would still have capped the
damage at exactly one stale value; `abortPending()` takes it to zero.

L1 = 3.8 ms is the poll period, and would be the same if the queue had been full and a
120-byte message had been in flight. L2 = 53.8 ms is 3.8 ms of L1 plus 50 ms of "the
menu had ten frames left", and no part of it is queueing behind the reaction.

To cut L2 further there are exactly two levers, both application decisions: poll more
often (linear in L1, and it also shortens each frame's turnaround since a frame only
leaves on a poll), or call `abortAll()` and accept a partial transfer at the panel.

### 3b.7 What `AFFA_TX_COALESCE = 0` costs, arithmetically

A render loop at `f` Hz in front of a transfer that takes `T` seconds queues
`min(⌈f·T⌉, AFFA_TX_QUEUE_DEPTH − 1)` stale messages, and every render beyond that
returns `Result::QueueFull`. Both halves are wrong in a different way:

* the panel keeps displaying superseded values for `⌈f·T⌉ / f` seconds after the key —
  the exact symptom this section exists to prevent;
* renders are rejected with `QueueFull`, so the application starts dropping updates
  arbitrarily rather than dropping the *oldest* ones, and the value that finally sticks
  is not necessarily the newest.

With coalescing on, a repeated render of one slot occupies **exactly one queue slot
regardless of render rate**, and it always holds the newest value. That is also why
`AFFA_TX_QUEUE_DEPTH = 4` is enough: the depth is a function of how many distinct slots
an application drives concurrently, not of how fast it drives them.

### 3b.8 Numbers

The library's own terms are exact and computable:

| Term | Value | Where it comes from |
| --- | --- | --- |
| One 8-byte standard CAN frame at 500 kbit/s | 222 µs unstuffed, ≤ ~260 µs worst-case stuffing | 111 bit times including IFS |
| L1 worst case | one poll period + the driver's `task_CAN` delivery jitter | §3b.3 |
| L1 as a poll count | **exactly 1**, always | §3b.3 |
| Queue contribution to L2 | **0** with coalescing or `Urgent` or `abortPending()` | §3b.4, §3b.5 |
| In-flight contribution to L2 | one ACK round-trip, or `AFFA_ACK_TIMEOUT_MS` if the panel is gone | `AffaTx.cpp` |

The one term the library cannot compute is the panel's ACK turnaround, and it dominates
L2. It is measured, not estimated: `examples/17_mediascreen` timestamps each transmitted
frame and its ACK against `IClock`, and prints the min/mean/max over a 14-frame
`showMenu`. **The measured figure for the Carminat panel on the 2-node 500 kbit/s bus
must be recorded here and in the README before v0.1.0 is tagged — owner: the core
implementer, as part of the on-vehicle acceptance run.** Until that number exists,
quote L2 as "L1 + the remaining frames of the transfer in flight" and nothing more
precise; a guessed millisecond figure in a datasheet-shaped table is worse than no
figure at all.

### 3b.9 The tests that pin this section

None of the above is a claim until one of these fails when it is broken. All run on the
host against `LoopbackLink` + `FakeClock`; none needs hardware.

| Test | Asserts |
| --- | --- |
| `test_isotp/key_latency_matrix` | The §3b.3 sentence: for queue occupancy 0…`AFFA_TX_QUEUE_DEPTH` × in-flight frame index 0…13, one injected key frame plus exactly one `poll()` fires `KeyCb`. |
| `test_isotp/coalesce_latest_wins` | With `FuncsReg` already latched, 100 `setText` calls and no `poll()` between them leave exactly one job in the queue, carrying the 100th payload, with 99 tickets completed `Aborted` and zero frames transmitted. |
| `test_isotp/coalesce_never_touches_started` | A `setText` enqueued while frame 2 of a 3-frame `setText` is in flight does **not** modify the in-flight job; the wire shows both messages complete and in order. |
| `test_isotp/abort_pending_reports_aborted` | Every dropped ticket produces exactly one `onComplete(_, Aborted)`; the in-flight job is untouched; a nested `abortPending()` returns 0. |
| `test_isotp/abort_all_frame_boundary` | After `abortAll()` mid-transfer, no further continuation frame of that job appears on the wire, the job completes `Aborted`, and the next message's first transmitted frame is its own frame 0. |
| `test_isotp/urgent_never_splits` | An `Urgent` enqueue during a 14-frame transfer appears on the wire strictly after the 14th frame and strictly before every queued `Normal` job. |
| `test_isotp/urgent_never_overtakes_registration` | With `FuncsReg` unlatched, an `Urgent` payload is still transmitted after both `0x70` registration probes. |
| `test_sync/poll_frequency_independence` | §4.4: one million `poll()` calls across one simulated second emit exactly one `0xB9` and never set `Failed`. |

## 4. Threading contract

**Since 0.3.0 there are two modes, and you pick one at compile time.**

| | `AFFA_ENABLE_TASK = 0` (default) | `AFFA_ENABLE_TASK = 1` |
| --- | --- | --- |
| Who calls `poll()` | **you**, from exactly one task, for ever | the library, on a task it owns |
| Who may render | that same task only | **any task, calling the panel directly** — §4.7 |
| What breaks it | anything blocking that task — see §4b.1 | a callback that blocks, and nothing else |
| Where the contract lives | §4.1 – §4.6, below | §4b |

§4.1 – §4.6 describe the caller-owned mode and remain exactly true of it. Everything in
them about *which context a callback fires in* is also true of the owned task — the only
thing that changes is which task that is. §4b describes the owned-task mode and the four
things it adds.

### 4.1 Where frames come from

`collin80/esp32_can` delivers frames from its own FreeRTOS tasks:

```
task_LowLevelRX (prio 19)  ->  callbackQueue (depth 16)  ->  task_CAN (prio 15)
                                                                  |
                                                          general callback
                                                                  |
                                              CanCommonLink::ingest -> AffaRing (SPSC)
                                                                  |
                                                     ... your task calls poll() ...
                                                                  |
                                                   AffaDisplayBase::poll()
```

The general callback therefore runs **in `task_CAN`**, not in an ISR — but it is on
the critical path of every frame on the bus, and `callbackQueue` is only 16 deep. It
does one thing: copy into the ring. It must never log, never allocate, never block,
never call user code. Everything else happens in the caller's task, inside `poll()`.

### 4.2 Which context each callback fires in

| Callback | Fires from | May it call back into the library? |
| --- | --- | --- |
| `KeyCb` (`onKey`) | the task that called `poll()`, or the task that called `pressKey`/`nav` | Yes, except `poll()`. Render calls are fine — they only enqueue. `abortPending()` from here is the intended reaction to a key that invalidates queued work (§3b.5). |
| `CompleteCb` (`onComplete`) | the task that called `poll()`, or from inside `KeyCb` when a key handler aborted queued work | Same. Enqueuing the next message from here is the intended pattern. Note it may be delivering `Result::Aborted` for a message *you* just discarded — check the ticket rather than assuming it was the panel. |
| `SyncCb` (`onSync`) | the task that called `poll()` | Same. |
| `FrameTap` (`onFrame`) | the task that called `poll()` (RX) or whichever task caused a transmission (TX) — including `pressKey`, a render call is queued so its frames always leave from `poll()` | Yes, but it is on the path of **every** frame on the bus. Keep it to a ring push. Do not render from it. |
| `ILogSink::write` | any of the above, plus `CanCommonLink::begin()` | **No.** Treat it as a leaf. It may be called with the library's internal state mid-transition. |

A callback must not block. It is running inside `poll()`; anything it waits for that
needs `poll()` to happen will not happen.

### 4.3 Re-entrancy

`poll()` sets `_inPoll` for its duration. A nested `poll()` returns immediately having
done nothing. This is a guard, not a feature — do not build on it. (It also refused the
now-deleted `sendBlocking()`, which is what `Result::Busy` was for.)

**State first, callbacks second.** Every callback in the library is fired *after* the
transition that caused it is complete: `finishJob()` pops the job before it invokes
`CompleteCb`; `abortPending()` empties the pending set before it reports any
`Aborted`; `setSync()` stores the new state before it invokes `SyncCb`. A callback
therefore always observes a consistent library, and a re-entrant call from inside one
sees the world as it is, not as it was.

Callbacks consequently nest, legitimately. The intended and tested shape is: `poll()` →
`pumpRx()` → `KeyCb` → the application calls `abortPending()` → `CompleteCb(ticket,
Aborted)` for each dropped message. `CompleteCb` running inside `KeyCb` is normal. What
is not allowed from any callback is `poll()`; everything else,
including `enqueue`, every render call, `abortPending()`, `abortAll()`, `pressKey()`
and `nav()`, is permitted. Nesting depth is bounded by `AFFA_TX_QUEUE_DEPTH`.

One re-entrancy rule specific to the observation seam: **`pressKey(..., KeySource::Wire)`
from inside a `FrameTap` is legal** — the frame it transmits is observed after the current
dispatch finishes, so the tap sees frames in wire order rather than nested.

### 4.4 `poll()` is frequency-independent

> Every periodic behaviour in the library is a comparison against `IClock::millis()`.
> Nothing counts calls. Calling `poll()` once per second and calling it a million
> times per second produce **the same frames, in the same order, with the same
> timing**; they differ only in how soon a received frame is noticed and in how much
> CPU is burned.

**That is a statement about the frames we EMIT. It is not a statement about whether a
transfer COMPLETES, and the difference is expensive.** An earlier edition of this section
ended with "there is no minimum call rate for correctness — only for latency and for
keeping `Stats::ringOverflow` at zero", and a consumer read it, correctly, as licence to
share the poll task with a BLE stack and a web server. It cost them 401 timed-out renders,
a latched `BUS_OFF` and two lost registrations in one day.

Split it in two, because the two halves are governed by different things:

* **The transmitted frame sequence is frequency-independent.** Proven, by the two tests
  below. Nothing in the library counts calls.
* **Delivery is not.** `AFFA_ACK_TIMEOUT_MS` and `AFFA_PEER_TIMEOUT_MS` are wall-clock
  deadlines evaluated *inside* `poll()`. A `poll()` that arrives late does not merely
  delay a result, it **changes** it: an ACK that arrived on time and sat in the ring for
  two seconds is a `Result::Timeout`, and an expired peer deadline tears down `FUNCSREG`
  and cancels the queue. There is a minimum call rate for *delivery*, and it is set by
  the tightest deadline you care about.

So: the frames are frequency-independent; the outcomes are not. If your task can be
blocked for longer than `AFFA_ACK_TIMEOUT_MS` by anything at all, you want §4b.

Sizing follows from `ringOverflow`, not from the protocol: at 500 kbit/s a full
8-byte frame takes ~228 µs, so `AFFA_RX_RING_DEPTH = 32` tolerates a ~7 ms gap between
`poll()` calls on a fully saturated bus, and far longer on the 2-node bus this library
actually runs on.

Two tests enforce this, one per profile. `test_core` calls `poll()` a million times across
one simulated second on the Carminat profile; `test_sync_profiles` does the same 20 000
times on the UpdateList profile, because a call-counting implementation would emit a storm
on **both** and the FSM is shared. Each asserts that

* exactly **one** heartbeat was transmitted (`0xB9` / `0x79`), and no sync request, and
* `SyncState::Failed` was never set — the link did not break.

Run the same test with `poll()` called twice per simulated second and the transmitted
frame sequence must be identical.

### 4.5 What an application task may call

Safe from any task, at any time, as long as **only one task drives the library**:
`poll()`, `enqueue()`, every render call, `abortPending()`, `abortAll()`,
`pressKey()`, `nav()`, `onFrame()`, `onKey()`, `onText()`, `onRadioText()`,
`onComplete()`, and all the observers (`syncState`, `busy`, `pending`, `stats`,
`panelGeometry`, `supports`, …).

None of these is safe from an **ISR**, including `pressKey()` — it can route into the
menu, which renders, which enqueues. An ISR-sourced key goes into a FreeRTOS queue and
is replayed through `pressKey()` from the task that owns `poll()`.

The library is **not internally locked**. If two tasks must reach it, either put a
mutex around every entry point in your adapter, or — better — give the second task a
queue and drain it from the task that owns `poll()`. **Better still, since 0.3.0: set
`AFFA_ENABLE_TASK=1` and let the library own both the task and the queue (§4b).** That
queue is proven code, it is the shape this section has recommended since 0.1.0, and
having every consumer write it again is how it gets written wrong.

The one thing that is genuinely concurrent, `CanCommonLink::ingest` writing into
`AffaRing` from `task_CAN` while `poll()` reads, is handled by the ring's SPSC
discipline and needs no lock.

### 4.6 The recommended shape for key handling

Split the reaction in two. The part that decides *what must stop* is cheap, cannot
block, and belongs in the callback — deferring it is what puts stale frames on the
panel. The part that talks to WiFi, NVS or a media player is expensive and belongs in
the application task.

```cpp
// Fires inside poll(), before the transmit pump. Cheap half only.
static void onKey(affa::Key k, affa::KeyEdge e, void* ctx) {
  auto* app = static_cast<App*>(ctx);
  if (k == affa::Key::Pause) {
    app->counterRunning = false;      // stop producing renders
    app->display->abortPending();     // discard the ones already queued
    app->display->setText("PAUSE");   // reaction, ahead of nothing
  }
  app->queueForLater(k, e);           // expensive half
}
```

Then the expensive half, off the callback:

```cpp
struct KeyEvent { affa::Key k; affa::KeyEdge e; };
static QueueHandle_t g_keys;   // xQueueCreate(8, sizeof(KeyEvent))

// App::queueForLater — still inside poll(). Never blocks, never allocates, never logs.
void App::queueForLater(affa::Key k, affa::KeyEdge e) {
  const KeyEvent ev{k, e};
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(g_keys, &ev, &woken);   // FromISR variant: never blocks, ever
}

void appTask(void*) {
  for (;;) {
    display.poll();                          // drains RX, may fire onKey
    KeyEvent ev;
    while (xQueueReceive(g_keys, &ev, 0) == pdTRUE)
      handleKeyProperly(ev.k, ev.e);         // WiFi, NVS, whatever you like
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
```

Note what is **not** deferred: `abortPending()` and the reaction render stay in the
callback. Deferring them by one task wakeup would put the reaction behind whatever the
counter enqueued in the meantime and reintroduce exactly the backlog §3b exists to
prevent. Defer the slow work, never the preemption.

`examples/17_mediascreen` ships this shape. In owned-task mode the split still applies
and for the same reason — the callback runs on the library's task now, so the expensive
half belongs off it more than ever — but the queue in the second half is yours to keep or
delete: the *preemption* half is what must stay in the callback.

---

### 4.7 Why a render is safe from any task — the boundary is `enqueue()`

**This is the change 2.0 exists for**, and it is one `if`:

```cpp
// AffaTx.cpp
if (!onPollOwner()) return post(t, funcId, data, len, opt, nullptr, len);
return admit(t, funcId, data, len, opt, nullptr, len);
```

Every render in the library — all 22 of them, across three families — funnels through
`enqueue()` after it has finished building its bytes. So the cross-task boundary sits
**below** every builder rather than above them, and the queue carries *finished bytes*
instead of an *intention*.

That is what makes "callable from any task" true for renders written after this sentence.
The previous design put the queue one layer up: a render had to be re-transcribed by hand
into an `Op` enum before it could cross, so the guarantee held for the calls somebody
remembered to mirror and quietly failed for everything else (§7.5).

What made it possible was an audit result rather than a clever mechanism: **no Carminat
render touches a member.** The builders are pure functions of their arguments, so their
bytes are complete at the moment `enqueue()` sees them and there is nothing left to
synchronise.

`onPollOwner()` answers true when no owner is registered, so a build with
`AFFA_ENABLE_TASK = 0` takes the direct path and pays nothing — the ring is not merely
unused, it is untouched. The one-task rule in §4.5 is exactly the rule for that build.

A refusal from `post()` is `QueueFull`: the ring was full. It is the same answer the
transmit queue gives for the same condition, because from the caller's side they are the
same event — there was no room for this render.


---

## 4b. The owned-task mode — `AFFA_ENABLE_TASK = 1`

```cpp
affa::CarminatDisplay display(link, clock);
affa::rtos::AffaTask  task;

void setup() {
  display.setLogSink(&sink);
  display.onKey(&onKey, nullptr);      // 1. callbacks
  display.onSync(&onSync, nullptr);
  task.onComplete(&onDone, nullptr);
  display.begin();                     // 2. begin()
  task.start(display);                 // 3. start() — and never call poll() again
}

void loop() {
  display.setText("HELLO");            // from here, or an HTTP handler, or a BLE task
  delay(10);
}
```

`examples/17_mediascreen` is this, complete, with WiFi, OTA and forty concurrent HTTP
renders against a real panel.

### 4b.1 What it is for

The caller-owned contract is one sentence and it is violated **by addition**: by the next
feature somebody puts in `loop()`, not by the code that was reviewed. One competent
consumer broke it three times in a single day, each time somewhere else, each time with
the same symptom on the glass:

| What went on the poll task | How it presented |
| --- | --- |
| a BLE service call, allowed to block on GATT | sync stuck at `0x08`, **401 of 644 renders `Timeout`**, every error counter zero |
| a retry site that advanced its backoff only on success | **21 261 frames transmitted against 9 625 received**, `BUS_OFF` latched, registration lost |
| a blocking WebSocket write | *"it froze again — because I opened the web interface?"* Two browser tabs, 115 failed renders |

In all three the frames arrived correctly and on time; the consumer was not awake to
consume them. The owned task removes the whole class, because the task that polls is no
longer a task anyone else can put anything on.

### 4b.2 Key latency does not regress, and that is the acceptance criterion

`KeyCb` still fires **synchronously inside `poll()`**, before any TX pumping, exactly as
it always has. Keys are **never** routed back to an application task through a queue —
that would add a task hop and make key latency depend on the application's scheduling,
which is the precise property this mode exists to remove.

What follows from that:

* **Key latency ≈ `AFFA_TASK_PERIOD_MS`, and nothing else.** It defaults to **2 ms**, not
  10 or 20. Measured on the bench rig at that period: worst observed iteration 472 µs.
* **The owned task outranks the application.** `AFFA_TASK_PRIO` defaults to **2**, above
  the Arduino loop task's 1. A key must not wait behind an application that is busy.
* **Your `KeyCb` now runs on the library's task, so it must not block.** This has the same
  force as "callbacks must not call `poll()`". It cannot be prevented, so it is made
  visible: `Status::pollLateMaxUs`.
* **Commands drain before `poll()`.** One iteration is `drainCommands()` then `poll()`
  then `publishStatus()`, so a render posted this period is pumped this period.

### 4b.3 A queue, not a mutex

Callbacks fire from inside `poll()` and are explicitly permitted to call back into the
library (§4.3). A non-recursive mutex deadlocks on the first such call; a recursive one
lets an application hold the library locked while it blocks on something else; and either
puts a lock in `core/`, which would end its portability. So a render made from another
task is **copied into a command queue** that the owned task drains — no lock anywhere, and
the single-caller invariant is untouched.

Two consequences a caller can see:

* **Arguments are copied**, bounded by `AFFA_TASK_ARG_MAX` (48). No pointer into a
  caller's stack ever crosses a task boundary.
* **A render made from inside a callback takes the DIRECT path**, because it is already on
  the owned task. `AffaTask` detects this. Self-enqueueing there would deadlock §4.3's
  documented preemption pattern against a full queue.

### 4b.4 `TxRequest`, not `TxTicket`

`AffaTask`'s render methods return a **`TxRequest`**, and the different name is the point:
a `TxTicket` is issued by `enqueue()`, which for a posted render has not run yet. A
`TxRequest` is issued at post time and mapped to the real ticket when the command drains.

* non-zero — accepted;
* `kNoRequest` — **refused**, never "accepted but untracked". The queue was full
  (`Status::queueDropped` counts it) or the render was refused outright.

Completions arrive through `AffaTask::onComplete(cb, ctx)` with the same `TxRequest` the
caller was given. `AffaTask` takes over `AffaDisplayBase::onComplete` to do the
translation, so **do not install your own on the display in this mode** — there is one
handle space, and this is it. A completion reported with `kNoRequest` is a render the
*library* made (a menu redraw, the close banner); it is forwarded rather than swallowed.

### 4b.5 Observers become a snapshot

`synced()`, `busy()`, `registered()`, `syncState()` and `stats()` still exist and are
still correct **from the owned task's own callbacks**. Off-task they are reads racing a
writer, and `Stats` is a seven-field struct: read field by field it can return a mixture
of two moments.

The owned task publishes an `affa::rtos::Status` once per iteration under a seqlock.
**Every reader that is not the owned task should use `task.status()`** — one snapshot, one
moment, lock-free, and it can never block an HTTP handler.

### 4b.6 The numbers that prove it is working

| Field | What a healthy board reports | What a bad number means |
| --- | --- | --- |
| `foreignPolls` | **0** | somebody still calls `poll()`. Those calls did nothing (`poll()` refuses a non-owner and counts it), which is why this is a counter and not a crash. |
| `pollLateMaxUs` | near the period — 472 µs measured at a 2 ms period | a callback is blocking the owned task. Past `AFFA_TASK_LATE_FACTOR × period` it also logs, rate-limited to once a second. |
| `queueDropped` | **0** | you are posting renders faster than the wire drains them. Gate on `status().busy`, as `examples/17_mediascreen` does. |
| `stackFreeBytes` | 2036 measured on the C3 with the marquee widget and a rendering `KeyCb` — i.e. about half of `AFFA_TASK_STACK` unused | shrink `AFFA_TASK_STACK` at your own risk; a deep callback chain lives on this stack. |

### 4b.7 Failure modes, and what each one does

| | Behaviour |
| --- | --- |
| `start()` before `begin()` | refused, `false`, logged. A task polling an un-begun display transmits nothing and reports no reason. |
| `start()` twice | refused, `false`. Two tasks polling one display is the same corruption as an application that never stopped calling `poll()`. |
| task or queue creation fails | refused, `false`, logged. Never a silent success that never polls. |
| the application also calls `poll()` | the call does **nothing** and increments `foreignPolls`. |
| the command queue is full | `kNoRequest`, `queueDropped++`. Never blocks the caller, never drops silently. |
| a callback blocks | cannot be prevented; recorded in `pollLateMaxUs` and logged. |
| `stop()` | drains, drops unstarted renders as `Cancelled`, lets a message already on the wire finish (bounded by two ACK timeouts), then joins. No torn ISO-TP left on the panel. |
| `stop()` from a callback | it is on the owned task and cannot join itself: the stop is requested and `stop()` returns. |

### 4b.8 What it does not change

`core/` and `util/` are untouched and still compile on the host
against nothing but C++17. `src/rtos/` is the only FreeRTOS-dependent directory in the
library and the only one a port omits . `AFFA_ENABLE_TASK=1` on a
non-FreeRTOS target is still an `#error`.

---


---

## 5. Configuration

**`src/AffaConfig.h` is the list.** It is 548 commented lines and every knob carries its
own reasoning, including the ones that were deleted and why. This section is a map of it,
not a copy — the copy is what went stale last time.

Two rules the file enforces that are worth knowing before you set anything:

* **Every gate is `#define`d to 0 rather than left undefined**, so the library uses
  `#if AFFA_X` and `-Wundef` catches a misspelling *inside* the library. It cannot catch
  one in a **consumer's** `build_flags` — `-D AFFA_ENALBE_NAV=0` defines a macro nothing
  reads — so panel selection is covered by a second mechanism: **silence is an `#error`.**
  Name at least one panel or the build stops.
* **Each optional `.cpp` gates its entire body.** PlatformIO's Library Dependency Finder
  compiles every `.cpp` under a `lib_deps` library; the consumer's `build_src_filter`
  cannot reach into it. The preprocessor is the only thing that can remove a translation
  unit, so an unselected panel compiles to an empty object file. That, plus
  `-ffunction-sections -fdata-sections -Wl,--gc-sections`, is what makes an unused panel
  cost **zero** flash rather than "not much".

| Group | Knobs | Notes |
| --- | --- | --- |
| Panel selection | `AFFA_PANEL_CARMINAT`, `AFFA_PANEL_UPDATELIST`, `AFFA_PANEL_CLUSTER`, `AFFA_PANEL_DEFAULT_ALL` | `DEFAULT_ALL` covers the first two only. **The cluster is never on by default** — everything it claims is inference from one capture. |
| Feature gates | `AFFA_ENABLE_POPUP`, `_FULLSCREEN`, `_CONFIRMBOX`, `_INFOPOPUP`, `_BIGMENU`, `_NAV` | Off means the call returns `NotSupported`, not that it silently does nothing. |
| Text and logging | `AFFA_ENABLE_TRANSLITERATION`, `AFFA_ENABLE_LOG`, `AFFA_LOG_LEVEL`, `AFFA_TEXT_MAX` | **Transliteration off is dangerous**: UTF-8 reaching the wire is garbage on the glass, and that is a visual failure rather than a compile error. |
| Link | `AFFA_ENABLE_CANCOMMON_LINK`, `AFFA_RX_RING_DEPTH`, `AFFA_RX_STALL_MS`, `AFFA_LINK_RECOVER_MS`, `AFFA_LINK_RECOVER_MAX_MS` | |
| Transmit | `AFFA_TX_QUEUE_DEPTH`, `AFFA_MAX_PAYLOAD`, `AFFA_MAX_EXTERNAL_PAYLOAD`, `AFFA_ACK_TIMEOUT_MS`, `AFFA_TX_MAX_RETRIES`, `AFFA_TX_RETRY_MS`, `AFFA_TX_RETRY_MAX_MS`, `AFFA_TX_HOLD_MS`, `AFFA_TX_COALESCE`, `AFFA_TX_DIRTY_QUIET_MS` | |
| The opening | `AFFA_SYNC_INTERVAL_MS`, `AFFA_HELLO_MIN_MS`, `AFFA_PING_REPLY_MIN_MS`, `AFFA_PEER_TIMEOUT_MS` | The rest of the opening is **data**, not knobs: see `SyncProfile`. |
| Owned task | `AFFA_ENABLE_TASK`, `AFFA_TASK_PERIOD_MS`, `_PRIO`, `_CORE`, `_STACK`, `_LATE_FACTOR`, `AFFA_DISPATCH_DEPTH`, `AFFA_TASK_ARG_MAX`, `AFFA_CALLBACK_BUDGET_MS` | §4b. `AFFA_ENABLE_TASK` is an `#error` off ESP-IDF / Arduino-ESP32. |

**`AFFA_DISPATCH_DEPTH` is the one to understand before tuning.** It sizes the cross-task
ring described in §4 — the slots a render uses when it is called from a task that does not
own `poll()`. Costs `sizeof(DispatchItem)` ≈ `AFFA_MAX_PAYLOAD + 20` per slot and **nothing
in CPU when unused**, because a call already on the owning task never touches it. Must be a
power of two. `0` removes the ring entirely, which is correct only for a build with no
owned task at all.

Sizing it **deeper than `AFFA_TX_QUEUE_DEPTH` is a mistake that looks like a fix**: it only
defers `QueueFull` to a worse place, where the caller has already been told the render was
accepted.
## 6. What a panel can do, and how big it is

Two questions, two answers, and they are different questions. `supports()` asks whether a
call will do anything; `panelGeometry()` asks how much will fit.

### 6.1 `supports(Feature)`

```cpp
bool supports(Feature f) const;   // pure virtual on AffaDisplayBase; each panel answers
```

| Feature | Carminat | UpdateList | Cluster |
| --- | :---: | :---: | :---: |
| `Text` | yes | yes | **no** — the one capture contains no text frame, so the encoding is unknown |
| `Time` | yes | no | no |
| `Power` | yes | yes | yes |
| `Menu` | yes | no | no |
| `Popup` | yes (if `AFFA_ENABLE_POPUP`) | no | no |
| `Fullscreen` | yes (if `AFFA_ENABLE_FULLSCREEN`) | no | no |
| `ConfirmBox` | yes (if `AFFA_ENABLE_CONFIRMBOX`) | no | no |
| `InfoPopup` | yes (if `AFFA_ENABLE_INFOPOPUP`) | no | no |
| `KeyTx` | yes (`0x1C1`) | yes (`0x0A9`) | no |

`supports()` reflects both the panel **and** the compile-time gates: a Carminat built with
`AFFA_ENABLE_POPUP=0` reports `supports(Feature::Popup) == false`, and `showPopupText`
returns `Result::NotSupported`.

> **`Feature::RadioText` was removed and is not coming back in that form.** It reported a
> *compile gate*, not a panel capability — and a capability query that answers a question
> about your own build tells the caller nothing about the glass, which is the only thing
> `supports()` is for. §7.7.
>
> Inbound text is still delivered: `UpdateListBase` decodes the radio's `0x121` and reports
> it through the protected virtual `onRadioText(bool isAux)`. That hook is real, exercised,
> and stays. `docs/NOTES.md` §8 has the AUX pattern table.

> **The one deliberate behaviour change versus the code that was extracted.** The legacy
> `IDisplay` gave `showInfoPopup`, `showConfirmBox`, `showFullscreenText`, `showPopupText`
> and friends **silently no-op default bodies returning `AffaError::NoError`** — calling
> one on a panel that could not do it looked exactly like success. Here every unsupported
> call returns `Result::NotSupported`, and `supports()` lets you ask first.

### 6.2 `panelGeometry()`

```cpp
PanelGeometry g = panel.panelGeometry();
```

Every field is **zero unless the panel actually has that surface**, and zero means "do not
render this here", not "unknown". A fitter written against Carminat's 26-character rows
produces nothing an 8-cell segment display can show, and this is how it finds out without
knowing which panel it is talking to.

| | `mainChars` | `menuRows` × `menuRowChars` | `infoRows` × `infoRowChars` | `listMaxItems` | image |
| --- | :---: | :---: | :---: | :---: | :---: |
| Carminat | 8 | 2 × 26 | 3 × 8 | 10 | 48 × 48 |
| UpdateList | 8 | 0 | 0 | 0 | — |
| Cluster | 0 | 0 | 0 | 0 | — |

**UpdateList's 8 is a promise, not a measurement**, and the distinction matters. The frame
always carries a 12-cell `new text` field, so a wider glass in that family shows more for
free — but the radio cannot tell which glass answered, and eight cells are what every
panel in the family is known to render. A fitter that trusts 12 writes text the segment
display silently truncates. §7.6.

---

---

## 7. What 2.0 deleted, and why you will not find it

This section exists because old code, old branches and old documents still name these
things. Each one is here so that "where did it go" has an answer that is not "git log".

`src/` went from ~14,000 lines to 8,500. Nothing on the wire changed.

### 7.1 The widgets — `widget/MenuModel`, `MenuController`, `IPage`, `CarminatMenuRenderer`, `Marquee`, `RowScreen`

**The rule, owner's, 2026-08-08: the library is a plain implementation of the transport
protocol. It is not UI.** Which item is selected, what a hold-`Load` gesture means, how
fast a title scrolls and when to repaint are decisions about a *product*, and a CAN driver
that makes them is a CAN driver you cannot use for a different product.

Nothing replaced them, because nothing was needed. The panel's contract is the render
calls, and they are unconditional: `showMenu`, `showMenuN`, `highlightItem`,
`selectMenuItem`, `setText`, `showInfoMenu`. An application that wants a scrolling title
calls `setText` with a different window every 400 ms — which is exactly what `Marquee`
did, except on the library's task, where it did not belong.

`AFFA_ENABLE_MENU` and `AFFA_ENABLE_MARQUEE` went with them, and so did the 384-line
document that described the design.

### 7.2 `proto/` — `IsoTp::Reassembler`, `ScreenModel`, `ScreenDecode`

The receive-direction decoder. It existed to be a **test oracle** — to prove that what the
builders emit is what a reader would read back — and that is a test's job, not a shipped
library's. It now lives in `test/affa_decode.h`, where it is used by exactly the code that
needed it and costs a consumer nothing.

The transmit-side layout it shared with the TX FSM was never separable in practice and
lives in `AffaTx.cpp`, where the frames are actually built.

`AFFA_ENABLE_ISOTP_RX` went with it.

### 7.3 `link/Esp32CanLink`

The raw-TWAI seam. **Nothing built against it**: every shipped example constructs
`CanCommonLink`, which is the stack proven end to end on the bench and the one most
existing Renault/ESP32 code already uses. Two implementations of one interface, one of them
untested by anything, is a place for the two to disagree — and the disagreement surfaces on
a bus, at a customer, not in CI.

A build that wants raw TWAI writes an `ICanLink` of its own. The interface is four methods
and `LoopbackLink` is a worked example in ninety lines.

### 7.4 The observation seam, layers 1 and 2 — `subscribe()`, `FrameCb`, `FrameMatch`, `SubHandle`, `onEvent()`, `EventKind`, `Event`, `LinkErrorKind`

Three layers of observation were specified: a raw frame tap, a filtered subscription table,
and a semantic event sink. **Layer 0, the tap, is the one that was ever used.**

Layers 1 and 2 cost a `FrameMatch` table (~256 B of static RAM plus a linear scan per frame
*per direction*), an event enum that had to be extended every time anything new happened,
and `AFFA_MAX_SUBSCRIPTIONS` to size it — in exchange for what `onFrame()` already does in
four lines at the call site. An abstraction whose only user is its own test is a liability
with documentation.

`onFrame()`, `onKey()`, `onText()`, `onRadioText()` and `onComplete()` remain. §3b.7 and
§4 give their threading rules; `CbKind` and `cbName()` are how a slow one is *named* in
`Status` rather than merely counted.

### 7.5 `rtos/AffaCommand.h` — `TxRequest`, `Op`, `Command`, `applyCommand`, `RequestTable`

**This is the deletion that fixed the original complaint**, so it is worth stating plainly.

The command queue sat one layer too high. Every render had to be re-transcribed by hand
into an `Op` enum and a `Command` struct before it could cross a task boundary, which meant
"callable from any task" was true for the handful of calls somebody remembered to mirror
and false for every render written afterwards. That is why applications ended up setting
`affa-task = 0` and driving the panel from `loop()`.

2.0 moved the boundary **down into `enqueue()`**, where every render already funnels, and
which the audit found no Carminat render touches a member to reach. The queue now carries
finished bytes rather than an intention, so there is nothing to transcribe and all 22+
renders are safe from any task — including ones written after this sentence. §4.

`AFFA_TASK_QUEUE_DEPTH` sized that queue and is gone; `AFFA_DISPATCH_DEPTH` sizes what
replaced it.

### 7.6 `UpdateListMenuDisplay` and `AFFA_PANEL_UPDATELIST_MENU`

A subclass and a build gate that existed to override exactly one method, with a name that
lied twice: nothing about it was a menu, and what it selected was a different **command
flavour**, not a different panel.

Byte `[2]` of the `0x121` text command has been seen as `0x76`, `0x7E` and `0x7F`, and both
the `0x76` and `0x7F` forms have been driven into UpdateList displays by independent
projects. One radio, one frame, every glass. `docs/WIRE.md` §9.2 keeps the evidence,
including the two bytes where our reconstructed copy of the `0x7F` form contradicted the
only real capture of it.

### 7.7 `Feature::RadioText`

It reported a **compile gate**, not a panel capability — and after `onText()` landed, a
gate that bought nothing. A capability query that answers a question about your own build
tells the caller nothing about the glass, which is the only thing `supports()` is for.

`onRadioText(bool isAux)` is real, is exercised, and stays.

---

## 8. The input seam — settled

This section used to be 373 lines weighing up whether menu navigation belonged in the
library or the application. It was a real question and it has an answer, so what follows is
the answer rather than the argument.

**Keys come out. Decisions stay out.**

The library decodes `0x1C1` / `0x0A9` into `(Key, KeyEdge)`, acknowledges the frame on the
bus because the protocol requires an acknowledgement, and hands the pair to `onKey()`. It
does not know what `Key::Load` means, does not track which row is selected, and has no
opinion about what a long press should do.

```cpp
panel.onKey([](affa::Key k, affa::KeyEdge e, void* ctx) {
  if (e == affa::KeyEdge::Click && k == affa::Key::Down) app->next();
}, app);
```

Everything the deleted `MenuController` did — the page stack, the `(Key, KeyEdge)` →
intent map, the re-render after a selection change — is ten lines in an application that
knows what its own screens are, and was several hundred in a library that had to guess.
`examples/17_mediascreen` and `examples/18_aiscreen` each do it differently, which is the
point.

**The one thing the library will not give up** is the acknowledgement. A key frame must be
answered on the wire within the panel's window or the panel stops sending them, and that is
transport, not UI. The `0x03 89` guard in front of the key decoder is part of the same
job: it is what stops a frame that merely *looks* like a key from being reported as one.
