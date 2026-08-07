# Refactor 2.0 — one surface, one handle, one task, and no way to hold it wrong

**Status: PLANNED, 2026-08-07.** Nothing below has landed yet. Written after 1.0.0, against
the evidence in §1, which is the first time the cost of the two-surface design could be
counted rather than argued about.

`docs/REFACTOR-PLAN.md` (2026-08-04, EXECUTED) fixed the *protocol*: eighteen hedge flags
collapsed into one `Phase` and the opening became a measured machine. This one fixes the
*contract*. They are the same kind of problem one layer up — a place where two halves of the
system are allowed to disagree — and the same rule applies: the flags can go, but the reasons
they existed belong in the new code.

---

## 1. Why now — the evidence, and it is not a discipline problem

The threading contract has been one sentence since 0.1: *call `poll()` from one task and
never block that task*. `AFFA_ENABLE_TASK` (0.3.0) exists to make that unbreakable by owning
the task itself. It works. **And almost nobody uses it.**

| | count |
|---|---|
| PlatformIO envs in this repository | 19 |
| …that set `AFFA_ENABLE_TASK=0` and call `poll()` from `loop()` | **13** |
| …including the flagship demo, `17_mediascreen` | yes |

The standard reading of that table is that consumers are careless. **It is wrong, and the
table has a mechanical explanation.**

### 1.1 The owned task cannot do what the panels can do

`AffaTask` publishes ten render calls. `CarminatDisplay` publishes twenty-two. The
difference is the entire interesting half of the library:

`setTextStyled`, `showMenuIcon`, `showMenuN`, `showMessageBox`, `selectBoxButton`,
`selectMenuItem`, `showNavBitmap`, `navTick`, `showInfoMenu`, `nav()`, `pushPage`/`popPage`,
`enqueueExternal`, `enqueueSplit` — plus UpdateList's `setScrollText`, `setScrollActive`,
`reassert`.

None of them are reachable through `AffaTask`. So the moment an application wants a styled
main line, an icon in the list gutter, or the nav pane, it **must** take an
`AffaDisplayBase&` or a `CarminatDisplay&` and call it directly. And once the application is
holding the raw display anyway, the owned task stops looking like protection and starts
looking like an extra object with a second vocabulary. Turning it off is the *rational*
response to the surface it was given.

Every new render call this library has added since 0.3.0 has widened the gap, because adding
one means editing `Op`, `applyCommand()`, `AffaTask.h` and `AffaTask.cpp` — four places, none
of which the person adding a Carminat screen is thinking about. Twelve calls have been added.
Zero were mirrored.

### 1.2 The gap has already produced a live data race, in our own demo

`examples/17_mediascreen` — the demo that exists to show the library off:

* `/api/cmd` is a `PsychicHttp` handler. It runs on the **httpd task**.
* It calls `g_carminat->showMenuN(...)`, `g_panel->setPower(...)`,
  `g_base->enqueueExternal(...)`, `g_carminat->showInfoMenu(...)` and about twenty more —
  directly, on that task (`main.cpp:534`–`712`).
* `loop()` calls `g_base->poll()` on the **Arduino loop task** (`main.cpp:1036`).

Both mutate `_queue`, `_qCount`, `_tx`, `_nextTicket` and `_lastEnqueued`. There is no lock,
no owner check and no atomic anywhere on that path: `AffaDisplayBase.cpp:98` guards `poll()`
and **`AffaTx.cpp`'s `enqueue()` guards nothing**. `setPollOwner()` protects the pump and
leaves the producer wide open.

It has not visibly bitten yet because the window is narrow and the demo is driven by a human
clicking a web page. That is luck, not design. The library handed out a loaded gun and
documented the safety catch on a different page.

### 1.3 The other two costs, briefly

**Three handle spaces for one action.** `Result` (acceptance) from the direct call,
`TxTicket` (`lastEnqueued()`, read immediately or lose it) from the base, `TxRequest` from
the task, with a translation table between the last two. An application cannot answer *"which
handle do I hold and which callback reports it?"* without first knowing which mode it was
compiled in.

**A blocking callback is measured but not identified.** `Status::pollLateMaxUs` says an
iteration took 340 ms. It does not say whether that was `KeyCb`, `SyncCb`, `CompleteCb`, the
frame tap, a subscription or `onText` — which is the only fact that shortens the search. The
three incidents in `docs/CR-0.3.0-OWNED-TASK.md` §2 all presented as *"the panel is frozen"*
with every error counter at zero.

---

## 2. The root cause: the command queue is one layer too high

Today's queue carries an `Op` enum with fourteen values and three 48-byte strings
(`rtos/AffaCommand.h`). It is a *transcription* of the render surface, so it has to be
re-transcribed every time the surface grows, and it never was.

But look at what a render call actually **is**. `CarminatDisplay::setTextStyled`
(`CarminatDisplay.cpp:192`) builds eight header bytes and fourteen text cells into a local
array and hands them to `submit()`. It reads `const` constants and the caller's arguments.
**It touches no member of anything.**

That is not a happy accident of one function. It was checked across the whole family:

> **Not one of `CarminatDisplay`'s twenty-two render calls writes a member.** The class holds
> exactly three pieces of mutable state — `_menuRenderer`, `_menu`, `_menuCtrl` — and they
> are touched only by `onPoll`, `initializeMenu`, `menuOpen`, `openMenu`, `routeKeyToMenu`
> and `onMenuClosed`: the key-routing path, never a render.
>
> `UpdateListDisplay` has four exceptions and they are known by name: `setScrollText`,
> `setScrollActive`, `reassert` and `onRadioText` touch `_marquee` and `_needsRedraw`.

So the renders are **pure functions producing bytes**, and every one of them — in both
families, present and future — funnels into a single choke point: `AffaDisplayBase::enqueue()`
in `AffaTx.cpp`. `CarminatDisplay::submit()` and `UpdateListBase::enqueueRender()` are both
two-line wrappers over it.

### 2.1 …and the one thing that gives the panel any state at all is a UI widget

The three members above deserve their own answer, because the owner asked the right question
about them: *we are a library, we do not organise anyone's UI — let them draw their own menu.*

The code already agrees on paper. `AffaConfig.h:78`, on `AFFA_ENABLE_MENU`: *"the panel's
whole menu contract is `showMenu(header,row0,row1,scroll)` + `highlightItem()`, both always
available regardless of this flag, and everything above them is one opinion about UI state."*
The flag defaults to **0**.

What the audit adds is how far that has drifted from practice:

| | |
|---|---|
| lines in the menu widget block | **1092** |
| PlatformIO envs that compile it | **1** — `[env:native]`, the host test env |
| shipping examples that use it | **none of the eighteen** |

And there is an inconsistency underneath it. This library already has the right pattern for a
widget, stated in `AffaDisplay.h:44` about `Marquee` and `RowScreen`: *"Both transmit nothing
and hold no display — the application samples them and calls the render primitive itself."*
`MenuModel` is the one widget that was embedded into the panel instead, and that embedding is
**the sole reason `CarminatDisplay` has mutable state**, which in turn is the sole reason §3.4
needs a Tier-1/Tier-2 split for this family at all.

**DECISION 2026-08-07 (owner): unembed it, do not delete it.** It moves to `widget/`, owned
and wired by the application exactly as `Marquee` is. `getMenu()`, `setMenuHotkey()`,
`clearMenuHotkey()`, `menuOpen()`, `openMenu()` and `routeKeyToMenu()` leave the panel and the
base. The code, its tests and `docs/MENU-WIDGET.md` all survive; what ends is a display driver
owning a UI state machine.

Consequence, and it is why this goes early: **`CarminatDisplay` becomes completely stateless**,
so every one of its twenty-two renders is Tier 1 with nothing to check. The Tier-2 list for the
whole library collapses to `pressKey`, `nav`, `abortPending`, `abortAll`, `resync` and
UpdateList's three marquee calls.

**Put the task boundary at that choke point instead, and the whole class of problems
evaporates.** A render called from any task builds its bytes on that task's own stack — which
is safe, because the builder is pure — and then crosses into the owned task as *a payload*,
not as an op code. Nothing needs transcribing. A render added in 2027 is thread-safe on the
day it is written, by a developer who never read this document.

The queue also gets *smaller*: `AFFA_MAX_PAYLOAD + ~12` ≈ 131 bytes replaces
`3 × 48 + ~10` ≈ 160.

---

## 3. The target

### 3.1 One surface

`AffaDisplayBase` and the panels **are** the async API. There is no second vocabulary to
learn and nothing to mirror.

```cpp
affa::CarminatDisplay display(link, clock);
display.onKey(&onKey, nullptr);
display.begin();                     // starts the owned task itself

// from the HTTP handler task, the loop task, anywhere:
auto s = display.setTextStyled("HELLO", icon, src, fmt, fmt2);
```

`AffaTask` stops being an application-facing type and becomes the internal engine that
`begin()` starts. It keeps its file, its gate and its tests; it loses its render surface,
because the render surface is now the display's.

### 3.2 One handle, returned rather than fetched

Every render call returns the same thing:

```cpp
struct [[nodiscard]] Submitted {
  TxTicket ticket = kNoTicket;   // non-zero == accepted
  Result   result = Result::Ok;  // WHY, when it is not
  explicit operator bool() const { return ticket != kNoTicket; }
};
```

Four bytes, returned in registers. It replaces all three of today's spellings at once:

* `TxRequest` and `RequestTable` are **deleted**. The ticket is minted at submit time from an
  atomic counter and the drained job reuses it, so there is exactly one handle space and no
  translation.
* `lastEnqueued()` is **deleted**. Its whole contract was *"READ IT IMMEDIATELY: the next
  enqueue overwrites it, including a render the menu makes for you"* — a documented footgun
  that exists only because the ticket was not returned. Now it is.
* `lastResult()` survives for diagnostics but stops being load-bearing.

`onComplete(TxTicket, Result, void*)` becomes the one completion callback in both modes.

### 3.3 One task, on by default, started by `begin()`

```cpp
struct DisplayOptions {
  bool     ownPollTask = true;      // ESP32 default. false = caller-owned, poll() yourself
  uint16_t periodMs    = AFFA_TASK_PERIOD_MS;
  uint16_t stack       = AFFA_TASK_STACK;
  uint8_t  priority    = AFFA_TASK_PRIO;
  int8_t   core        = AFFA_TASK_CORE;
  bool     passive     = false;
  bool     autoPower   = true;
  bool     selfAck     = false;
};
bool begin(const DisplayOptions& opt = {});
```

`AFFA_ENABLE_TASK` defaults to **1** on `ARDUINO_ARCH_ESP32`. `docs/CR-0.3.0-OWNED-TASK.md`
§11.1 deferred exactly this decision — *"keep the default 0 for 0.3.0, document loudly,
revisit for 1.0"* — on the grounds that it silently changes which task callbacks run on. That
was right then and it is spent now: 1.0.0 has shipped, this is 2.0, and §1's table is what
the loud documentation achieved.

Caller-owned mode is preserved exactly, because it is the only mode a non-FreeRTOS port has.
It becomes `ownPollTask = false` — one explicit field, not the absence of a build flag.

### 3.4 Cross-task dispatch, transparent

Inside `enqueue()` / `enqueueExternal()` / `enqueueSplit()`:

```
mint the ticket (atomic)
if (the owned task is running && this is not the owned task)
    copy funcId + opts + bytes into the dispatch ring   -> return {ticket, Ok}
else
    the existing direct path, using the minted ticket
```

**A call from a foreign task never blocks and never fails for being on the wrong task.** It
returns its ticket immediately; the verdict arrives in `onComplete` exactly as it always did
for the direct path. The one honest consequence, and it goes in the API doc in bold: *a
refusal that the direct path reports synchronously (`NotSupported`, `TooLong`, `UnknownFunc`)
is reported through `onComplete` when the call came from another task* — because nothing has
been enqueued yet at the moment of return. `BadArgument` for a null buffer is still checked
on the calling task, since it needs no library state.

`enqueueExternal`/`enqueueSplit` post the **pointer**, not a copy. That is already their
contract — the bytes must stay valid until the ticket completes — so it needs no new rule.

The ten stateful calls that are not byte-builders keep an op record, and the list is closed
and short: `pressKey`, `nav`, `abortPending`, `abortAll`, `resync`, `pushPage`, `popPage`,
`setScrollText`, `setScrollActive`, `reassert`.

### 3.5 Off-task reads: a published snapshot, not a racing read

The seqlock currently in `AffaTask::publish`/`status()` moves down into `AffaDisplayBase`,
where it belongs — it describes the display, not the task. `display.status()` is then the
documented off-task read in **both** modes, and the direct accessors (`phase()`, `stats()`,
`queued()`, `busy()`) keep their meaning for the owned task's own callbacks.

`rtos::Status` becomes `affa::Status`, gains the §3.6 fields, and stops being a type an
application has to reach into `rtos/` for.

### 3.6 A blocking callback names itself

The seven callback sites are all in `src/core/` — `AffaDisplayBase.cpp:410`,
`AffaObserve.cpp:91/116/124/180`, `AffaSync.cpp:696`, `AffaTx.cpp:780`. Each gets a scoped
guard that stamps entry and, on exit, records the duration against the callback's identity:

```cpp
enum class CbKind : uint8_t { Key, Sync, Complete, FrameTap, Subscription, Event, Text };
```

New in `Status`: `slowestCb`, `slowestCbUs`, `slowestCbAtMs`, `cbOverruns`. The log line
becomes

```
[AFFA-T] KeyCb blocked the owned task for 340 ms (budget 16 ms) — 3rd overrun
```

instead of today's anonymous *"a callback is blocking the owned task"*.

**Callbacks stay synchronous, and that is not a compromise.** Key delivery inside `poll()`
before any TX pumping is the acceptance criterion the owned-task design lives or dies by
(`openspec/specs/owned-task/spec.md`, "Callbacks Still Fire Synchronously Inside Poll"). A
callback that blocks still blocks the library. It cannot be prevented — it can only be made
impossible to misattribute, which is what this does.

Compile-time free: `AFFA_CALLBACK_BUDGET_US = 0` removes the guard entirely.

---

## 4. What this deletes

Net negative lines, which is the point.

| gone | why |
|---|---|
| **Layer 1** — `subscribe` / `unsubscribe` / `subscriptions`, `FrameMatch`, `SubHandle`, `kNoSub`, `AFFA_MAX_SUBSCRIPTIONS` | **DONE.** ~256 B and a table walked twice per frame per direction, dispatching to nobody in nineteen examples. Everything it did is three lines inside a Layer 0 tap — `test_seam` now proves that, because its two re-entrancy tests were rewritten onto the tap and assert the same things |
| **Layer 2** — `onEvent`, `Event`, `EventKind`, `EventCb`, `LinkErrorKind`, `emit()`, `reportLinkError()` | **DONE.** Every arm duplicated a callback that already existed: SyncChanged/Registered/PeerLost = `SyncCb`, TxComplete = `CompleteCb` byte for byte, Key = `KeyCb` (and disagreed with it — the event fired even when the menu had consumed the key), LinkError = counters already on `Stats`. `setSync()` loses the `extra` parameter that existed only to fire a second one |
| `TxRequest`, `kNoRequest`, `RequestTable<N>` | one handle space |
| `Op`, `applyCommand()`, the 14-case switch | the queue carries bytes |
| `AffaTask`'s 14 render forwarders | the display is the surface |
| `Command::s0/s1/s2` (144 B) | payloads, not strings |
| `lastEnqueued()` | the ticket is returned |
| `AFFA_TASK_ARG_MAX` | nothing truncates at a string boundary any more |
| `setPassive` / `setAutoPower` / `setSelfAck` as post-`begin()` setters | `DisplayOptions` |

---

## 5. Sequencing, and how each step is proved

The library works and is on glass. Each step must keep it working, and "it compiles" is not
evidence. Host suite green at every step; `test_carminat_wire` and `test_updatelist_wire`
must not move a byte at any point — **no step in this plan changes the wire.**

1. **`Submitted` and the atomic ticket.** Mint from `__atomic_add_fetch`; render calls return
   `Submitted`; `lastEnqueued()` removed. Mechanical, wide, no behaviour change.
   *Proved by:* full host suite unchanged; a new test asserting two tickets minted from
   simulated concurrent callers never collide.

2. **Unembed the menu widget (§2.1).** `MenuModel` / `MenuController` / `CarminatMenuRenderer`
   / `IPage` move out of the panel into `widget/`, owned by the application. `getMenu()`,
   the hotkey surface and the three `menuOpen`/`openMenu`/`routeKeyToMenu` seams leave
   `AffaDisplayBase` and `CarminatDisplay`. **`CarminatDisplay` ends this step with no
   members.**
   *Proved by:* `test_menu_widget` green against the widget driven directly rather than
   through the panel; `test_carminat_wire` unmoved; the `[env:native]` build is the only one
   whose flags change, because it is the only one that compiled this code.

3. **The dispatch ring.** A lock-free SPSC-safe ring of payload records in `core/`, portable,
   host-testable, with the op-record variant. Not yet wired to anything.
   *Proved by:* new `test_dispatch` — round-trip fidelity, full-ring refusal counted, op and
   payload variants, `AFFA_MAX_PAYLOAD` boundary.

4. **Cross-task dispatch inside `enqueue()`.** The branch in §3.4. Caller-owned mode takes
   the direct path exactly as today.
   *Proved by:* host test driving `enqueue()` with a faked "foreign task" identity, asserting
   the job appears after one drain and the ticket matches; existing suite unchanged.

5. **`begin(DisplayOptions)` starts the task; `AffaTask` goes internal.** `AFFA_ENABLE_TASK`
   defaults to 1 on ESP32.
   *Proved by:* `test_owned_task` retargeted; **bench flash of `09_golden`** — it already
   uses the owned task, so it is the cheapest real check that the lifecycle still opens a
   session.

6. **The callback guard and the new `Status` fields.**
   *Proved by:* a host test installing a deliberately slow `KeyCb` and asserting
   `slowestCb == CbKind::Key`; `AFFA_CALLBACK_BUDGET_US=0` builds to identical size.

7. **Delete the old surface.** `Op`, `applyCommand`, `RequestTable`, `TxRequest`, the
   forwarders.
   *Proved by:* it does not build if anything still refers to them, which is the intent.

8. **Collapse nineteen examples to three, and migrate those.** See §5.1 — this is not
   cleanup, it is the deliverable, because the examples are what consumers copy and thirteen
   of them currently teach the unsafe pattern.
   *Proved by:* every surviving env builds; the demo on the bench, driving the web UI from
   its HTTP handlers while `foreignPolls` and `cbOverruns` both stay at zero — which is
   §1.2's race, now impossible rather than merely unobserved.

   **Two console features the demo owes, both requested by the owner 2026-08-08:**
   * **A nav-header sweep card.** `showNavBitmapWithHeader()` exposes the fourteen bytes;
     ten are unmeasured, and the reported symptom — a stripe on the nav pane after a display
     power-cycle, a `setText`, then a bitmap — has no capture behind it. The card needs ±1
     stepping per byte and a one-click "restore captured", because a sweep whose baseline is
     lost is a sweep that proves nothing.

     **HUMAN-IN-THE-LOOP, NOT AUTOMATED, AND THE REASON IS THE ORACLE.** A fully scripted
     sweep is the obvious idea and it does not work here: the library can report that the
     panel ACKed, and an ACK is not a render. A panel that is not powered acknowledges a
     screen it never lights — every counter says success and the glass stays black, which is
     the failure mode with no symptom this project has already paid for twice. So 256
     automated steps produce 256 rows of "ACK ok" and no information.

     What works is the hybrid: the console drives the sequence and writes a row per step —
     the exact bytes sent, in wire order — and the operator supplies the one bit no machine
     on this bus has, "did the glass change", as a single click. One click per step instead
     of retyping a byte, and the output is a LABELLED dataset.

     Two things it has to get right or the table lies:
     * **The preamble is part of the experiment.** The reported symptom needs power-cycle →
       `setText` → bitmap, so the sweep must replay that before each value. Without it the
       first few steps run in one panel state and the rest in another.
     * **One byte at a time, never a pair.** Three captures in which two bytes moved
       together are what produced the "one icon field" misreading that the list-screen sweep
       took apart. `test_each_nav_header_byte_moves_its_own_byte_and_nothing_else` is the
       library-side half of that rule.

     Log rows should land in the shape of `docs/captures/*.csv` so `tools/decode_oem_csv.py`
     and the existing corpus analysis apply unchanged.
   * **A wire preview before every send.** The menu icon/thumb cards already show the bytes
     they are about to put on the bus; it should be the rule, not one card's feature. It is
     also what makes a sweep safe to run against glass: you see what you are about to change
     before it changes.

9. **Docs.** `docs/API.md` §4 (the threading contract becomes one mode with an opt-out),
   §3b.4 (`Submitted`), the knob table; `README.md`; `openspec/specs/owned-task/spec.md` and
   `render-queue/spec.md`; `CHANGELOG.md` with every BREAKING item and its migration line.

10. **Soak.** `09_golden` long, on the bench, with `sessionsLost`, `cbOverruns`,
    `foreignPolls` and `pollLateMaxUs` on the status page.

**Steps 1–4 are the load-bearing ones and they are independent of 5–7.** If step 4 soaks
badly, the suspect list is one branch long — which is the lesson `docs/REFACTOR-PLAN.md` §"why
step 4 was not done with 2 and 3" paid for the first time.

### 5.1 The examples: nineteen become three

**DECIDED 2026-08-07 (owner).** Nineteen envs and ~18 400 lines of examples, of which one
already covers nearly the whole surface. The survivors:

| | what it is | why it lives |
|---|---|---|
| `01_quickstart` | **new**, ~100 lines: link, display, `begin()`, `setText` — and **no `poll()` anywhere** | the shortest honest answer to "how do I use this", and the shape every consumer should copy |
| `02_demo` | `17_mediascreen`, extended until every endpoint is exposed | the full spectrum: both families, the web console, the wire log, OTA |
| `03_bringup` | `01_bringup` | **a tool, not a demo.** First flash of a bare board, recovery, OTA. It has to work when the demo is what is broken |

Plus `09_golden` **temporarily**, through step 10: it is already on the owned task and it is
the cheapest check that the opening still completes, so it is the reference the soak measures
regression against. Once the soak is green its instrumentation — `sessionsLost`,
`/deregistered.txt`, the driver counters — moves onto the demo's status page and it goes.

The sixteen that go are genuinely redundant, and the two that looked load-bearing are not:

* **The UpdateList family keeps its coverage.** `17_mediascreen` already selects between
  `Family::Carminat` and `Family::UpdateList` at boot, so deleting `10_updatelist` and
  `12_ulclock` costs no family. What the demo does *not* yet expose is UpdateList's own
  surface — `setScrollText`, `setScrollActive`, `reassert`, the AMS hotkey — and step 8 owes
  those endpoints before those two examples are deleted, not after.
* **The nav pane keeps its lab.** `showNavBitmap` / `navTick` are already on the demo's
  console with a live image upload, which is what `16_navlab` existed to provide.

---

## 6. Decided, 2026-08-07 (owner)

| question | answer |
|---|---|
| foreign-task render calls | **transparently queued**, never refused, never blocking |
| callbacks | **synchronous**, plus per-callback budget and the culprit's name |
| compatibility | **clean 2.0** — one handle, one contract, no deprecation window |
| the menu widget (§2.1) | **unembedded**, not deleted — it moves to `widget/`, the application owns it |
| the examples (§5.1) | **nineteen become three** — quickstart, full demo, bringup |
| process | plan in `docs/`, then code |

---

## 7. Open, and not to be guessed

* **Dispatch ring depth.** `AFFA_TASK_QUEUE_DEPTH` is 8 today, chosen to match
  `AFFA_TX_QUEUE_DEPTH + 2` so that a deeper command queue could not defer `QueueFull` to a
  worse place. That reasoning still holds and the default carries over — but a payload record
  is 131 bytes, so eight slots is ~1 kB of RAM where the old command queue was ~1.3 kB. No
  change, recorded so the number is not re-derived from nothing later.
* **Does a foreign-task `pressKey(Local)` still fire `KeyCb` synchronously?** It cannot — the
  op has to cross to the owned task first. Today `AffaTask::pressKey` has the same property
  and it is undocumented. State it: **`pressKey` from a foreign task fires `KeyCb` on the
  owned task, one dispatch later.** The wire-latency guarantee is about keys arriving *from
  the panel* and is untouched.
* **`showMenuN(scratch, cap, ...)` takes a caller buffer.** It builds into the caller's
  scratch and submits from it, so it is already copy-on-submit and safe. Verify this in step
  3 rather than assuming it; it is the one render whose bytes do not start on its own stack.

---

## 8. What must not be lost

The same clause `docs/REFACTOR-PLAN.md` closed with, and it applies harder here because this
refactor deletes more than it adds. The value of this codebase is disproportionately in the
prose that says *why* a byte is what it is and *what was measured to find out*. Every comment
on `_helloPending`, on the single-shot TX failure, on `AFFA_HELLO_MIN_MS`, on why holding a
render beats rejecting it — none of that is touched by anything above, and none of it may be
lost to a tidy-up that happens to be passing.
