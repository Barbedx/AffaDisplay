# CR 0.3.0 — the library owns the poll task

> **Status:** design request, not implemented. Written from a consumer's failure log.
> **Target:** v0.3.0 (a new capability and a new directory, so a minor bump, not a patch).
> **Requested by:** the MegaOpen integration, 2026-07-28.

---

## 1. What is being asked for

Make `AFFA_ENABLE_TASK` real: the library creates and owns a FreeRTOS task that calls
`poll()`, and the application never calls `poll()` at all. The application may then call
render methods **from any task** without arranging anything.

`AffaConfig.h` currently answers this with an `#error`:

> `AFFA_ENABLE_TASK is not implemented. The library owns no task; call poll() from exactly
> one task of your own (docs/API.md §4).`

and `docs/API.md` §2403 states the reason:

> there is no `vTaskCreate` under `src/`, and there will not be one while "no `vTaskDelay`
> in `src/`" (§4) is the contract.

**That contract is right and this CR does not propose breaking it.** See §4: the task goes
in a new `src/rtos/` layer that is compiled only on FreeRTOS targets. `core/`, `util/`,
`proto/` and `widget/` do not change and keep compiling on the host against nothing but
C++17.

---

## 2. Why — the evidence

The threading contract is one sentence ("call `poll()` from one task and never block that
task") and it is evidently hard to keep. In a **single day**, one competent consumer broke
it three times, each time in a different place, each time with the same symptom on the
glass and a different-looking cause on the bench:

| # | What was put on the poll task | How it presented |
|---|---|---|
| 1 | `BleHub::Service()` — allowed to block on GATT; ANCS does a Control Point write per notification, and 36 arrived at once | sync stuck at `0x08`, `PEER_ALIVE` never credited, **401 of 644 renders `Timeout`** while every controller counter read zero and the RX ring never overflowed |
| 2 | Retry sites that advanced their backoff only on success | a refused enqueue retried at loop rate: **21 261 frames transmitted against 9 625 received**, `BUS_OFF` + `ERR_PASS` latched, registration lost, every later render failed |
| 3 | `WsStream::loop()` → `_ws.sendAll()` — a blocking network write | *"it froze again — because I opened the web interface? I did not even click anything."* Two browser tabs, registration lost twice, 115 failed renders |

In all three the frames were arriving correctly and on time. The consumer was simply not
awake to consume them, and **the deadlines that matter are wall clock**: `AFFA_ACK_TIMEOUT_MS`
and `AFFA_PEER_TIMEOUT_MS` are evaluated *inside* `poll()`, so an ACK that arrives on time
and sits in the ring for two seconds is a `Timeout`.

### 2.1 A doc sentence that actively misleads

`docs/API.md` §4.4 says:

> There is no minimum call rate for **correctness** — only for latency and for keeping
> `Stats::ringOverflow` at zero.

That is true **of the transmitted frame sequence** — and the two frequency-independence
tests prove exactly that. It is **not** true of delivery. A late `poll()` does not merely
delay a result, it *changes* it: `Ok` becomes `Timeout`, and `PEER_ALIVE` expiry tears down
`FUNCSREG` and cancels the queue. Whatever else this CR does, **§4.4 should be amended** to
separate "the frames we emit are frequency-independent" from "whether a transfer completes
is not".

### 2.2 Why the consumer cannot simply be more careful

Each of the three fixes above was correct and none was sufficient, because the contract is
violated by *addition*: it is broken by the next feature someone puts in `loop()`, not by
the code that was reviewed. The library already prescribes the answer in §4.5 —

> give the second task a queue and drain it from the task that owns `poll()`

— and the consumer had already built that queue (a FreeRTOS command mailbox) and still
got starved, because the queue solves *concurrent access* and not *scheduling*. The half
that is missing is the one the library is better placed to own.

---

## 3. Hard requirement: key latency must not regress

**This is the acceptance criterion the design lives or dies by.** A steering-wheel button
must reach BLE/UART with no added delay. The library's own headline guarantee (§4, `poll()`)
is:

> RX strictly precedes TX so key latency is bounded by the poll period ALONE, not by queue
> depth or a message in flight.

The owned task must **preserve that exactly**, which forbids the obvious lazy design:

* **FORBIDDEN:** routing decoded keys back to an application task through a queue. That
  would add a task hop and make key latency depend on the application's scheduling — the
  precise property this CR exists to remove.
* **REQUIRED:** `KeyCb` continues to fire **synchronously inside `poll()`**, on the owned
  task, before any TX pumping, exactly as today. The only thing that changes is *which*
  task that is — and it changes for the better, because the owned task is not shared with
  a web server.

Consequences to write into the contract:

1. **Key latency ≈ the task period.** `AFFA_TASK_PERIOD_MS` therefore defaults to **2 ms**,
   not 10 or 20. At 500 kbit/s a full frame is ~228 µs; 2 ms is comfortably below anything
   a human perceives and keeps `ringOverflow` at zero with margin.
2. **The owned task must outrank the application.** `AFFA_TASK_PRIO` defaults **above** the
   Arduino loop task (which is 1), i.e. **2**. A key must not wait behind an application
   that is busy.
3. **`KeyCb` now runs on the library's task, so it must not block.** Today a consumer's key
   sink might do a blocking UART write and get away with it, because it was on their own
   loop. This becomes a documented contract with the same force as "callbacks must not call
   `poll()`". §7 proposes a diagnostic so a violation is visible rather than mysterious.
4. **Commands drain BEFORE `poll()`, not after.** One iteration is `drainCommands()` then
   `poll()`, so a render enqueued this period is pumped this period rather than next.
   Key latency is unaffected either way — keys arrive in `poll()`'s RX phase regardless —
   but render latency improves by one period for free.

---

## 4. Design

### 4.1 Layering — the portability contract is preserved

```
src/core/    src/util/    src/proto/    src/widget/     UNCHANGED. No FreeRTOS. Host-testable.
src/rtos/                                               NEW. FreeRTOS only, gated.
    AffaCommandQueue.h/.cpp    the command mailbox
    AffaTask.h/.cpp            owns the task; drains the queue; calls poll()
```

`src/rtos/` is compiled only when `AFFA_ENABLE_TASK` is 1, which itself must be an `#error`
unless a FreeRTOS target is detected (`ESP_PLATFORM` / `ARDUINO_ARCH_ESP32`). The existing
`#error` therefore stays for every host build and for any non-FreeRTOS port — it just stops
being unconditional.

**Nothing in `core/` gains a lock, a task, or a `vTaskDelay`.** The 131 host tests in the
consumer and the library's own `test/` are untouched by construction.

### 4.2 The locking problem, and why the answer is a queue rather than a mutex

The library is deliberately unlocked. The moment it owns the task, an application calling
`setText()` from an HTTP handler runs `enqueue()` concurrently with the transmit FSM that
`poll()` is mutating. Two ways out:

| | verdict |
|---|---|
| Mutex around every entry point | **Rejected.** Callbacks fire from inside `poll()` and are explicitly permitted to call back into the library (§4.3 "State first, callbacks second"). A non-recursive mutex deadlocks on the first such call; a recursive one silently permits an application to hold the library locked while it blocks on something else. It also adds a lock to `core/`, breaking §4.1. |
| A command queue drained by the owning task | **Chosen.** No lock anywhere, the owned task remains the only caller, and the re-entrancy guarantees in §4.3 are preserved untouched because nothing about the single-caller invariant changes. |

This is the shape the consumer already proved: a FreeRTOS queue of copied request structs,
drained from the poll task, with the verdict reported through a callback. It has been
carrying every web-originated render in MegaOpen for a week.

### 4.3 Public surface

```cpp
#if AFFA_ENABLE_TASK
namespace affa {

struct TaskOptions {
  uint16_t periodMs  = AFFA_TASK_PERIOD_MS;   // 2
  uint16_t stack     = AFFA_TASK_STACK;       // 4096
  uint8_t  priority  = AFFA_TASK_PRIO;        // 2 — above the Arduino loop task
  uint8_t  queueDepth= AFFA_TASK_QUEUE_DEPTH; // 8
  int8_t   core      = -1;                    // tskNO_AFFINITY; the C3 is single-core
};

class AffaTask {
 public:
  // Starts the task. The display must already be constructed and its callbacks
  // installed — see §6.1 on why start() is separate from begin().
  bool start(AffaDisplayBase& d, const TaskOptions& = {});
  void stop();                                  // joins; safe to call twice

  // Every render call, callable FROM ANY TASK. Returns a ticket exactly as the
  // direct API does, or kNoTicket when the queue is full — never silently dropped.
  TxTicket setText(const char*);
  TxTicket setTime(const char*);
  TxTicket setPower(bool);
  TxTicket showMenu(const char*, const char*, const char*, uint8_t scroll = 0);
  TxTicket showPopupText(const char*, uint8_t = 0x09, uint8_t = 0xFF, uint8_t = 0x60);
  TxTicket hidePopup();
  TxTicket showFullscreenText(const char*, const char*, const char*);
  TxTicket hideFullscreenText();
  TxTicket showConfirmBox(const char*, const char*, const char*);
  TxTicket showInfoPopup(const char*, const char*, const char*);
  TxTicket hideInfoPopup();
  TxTicket highlightItem(uint8_t);
  TxTicket pressKey(Key, KeyEdge, KeySource = KeySource::Local);
  void     abortPending();
  void     resync();                            // re-runs begin() on the owned task

  // A SNAPSHOT, not live accessors — see §4.4.
  struct Status {
    SyncState sync;
    bool      registered, busy;
    uint8_t   queued;
    Stats     stats;
    uint32_t  stampMs;        // when the owned task published this
    uint32_t  pollLateMax;    // worst observed iteration overrun, ms (§7)
    uint32_t  queueDropped;   // commands refused because the queue was full
  };
  Status status() const;
};
}
#endif
```

### 4.4 Observers must become a published snapshot

Today `synced()`, `busy()`, `registered()`, `syncState()` and `stats()` are documented safe
from any task. With an owned task they are reads racing the writer. Individual `uint32_t`
and enum reads are benign on a 32-bit target, but `Stats` is a six-field struct and reading
it field-by-field can return a mixture of two moments.

**The owned task publishes a `Status` once per iteration into a double-buffered slot; every
external reader gets `status()`.** The direct accessors remain, are still safe to call from
the owned task's own callbacks, and gain a doc note that off-task readers should prefer
`status()`.

This matters concretely: the consumer's `/api/health` reads `registered()`, `busy()` and the
sync flags from an HTTP task on every request.

### 4.5 One iteration, exactly

```
for (;;) {
    drainCommands(upTo: queueDepth);   // renders enqueued this period pump this period
    display.poll();                    // RX -> KeyCb (sync, in-task) -> TX pump
    publishStatus();
    vTaskDelay(periodMs);
}
```

`vTaskDelay`, and *only* here — one call, in `src/rtos/`, never in `core/`.

---

## 5. Config knobs (restored, with defaults chosen for §3)

| Knob | Default | Why this value |
|---|---|---|
| `AFFA_ENABLE_TASK` | `0` | Opt-in. `#error` on non-FreeRTOS targets, as today. |
| `AFFA_TASK_PERIOD_MS` | `2` | Key latency is bounded by this. See §3.1. |
| `AFFA_TASK_PRIO` | `2` | Above the Arduino loop task (1). See §3.2. |
| `AFFA_TASK_STACK` | `4096` | `poll()` plus the deepest documented callback nesting (§4.3 of API.md). To be measured with `uxTaskGetStackHighWaterMark` and the figure recorded here. |
| `AFFA_TASK_QUEUE_DEPTH` | `8` | Matches `AFFA_TX_QUEUE_DEPTH`; a deeper command queue than transmit queue only defers `QueueFull` to a worse place. |

---

## 6. Failure modes this design must engineer out

The request is explicitly *"design the lib so it will not break by its own"*. Each of these
is a way an owned task can fail that a caller-owned `poll()` cannot, and each needs an
answer in the implementation, not a sentence in a README.

| # | Failure | Required behaviour |
|---|---|---|
| 6.1 | `start()` called before the display's callbacks are installed → the first `poll()` fires events into null sinks | `start()` is a separate call from `begin()`; document "install callbacks, then `start()`". Assert `d.begin()` has run. |
| 6.2 | Application *also* calls `poll()` | `poll()` must detect it is not on the owned task and return immediately having done nothing, incrementing a counter surfaced in `Status`. Silent double-polling corrupts the FSM. |
| 6.3 | Queue full | `kNoTicket` returned, `Status::queueDropped` incremented. **Never** a silent drop, never a block — a render call that blocks its caller reintroduces the whole problem in the other direction. |
| 6.4 | A user callback blocks the owned task | Cannot be prevented; must be *visible*. Measure each iteration and record `Status::pollLateMax`; log once (rate-limited) past a threshold. See §7. |
| 6.5 | `start()` twice | Second call returns false and does not create a second task. Two tasks polling is the same corruption as 6.2. |
| 6.6 | `stop()` while a transfer is in flight | Drain, cancel pending with `Result::Cancelled`, then join. No torn ISO-TP transfer left on the wire. |
| 6.7 | Task creation fails (heap) | `start()` returns false and says so through the log sink. The current code path would otherwise appear to work and never poll — the exact failure the `#error` was added to prevent. |
| 6.8 | Application calls a render from inside a `KeyCb` | Already legal and must remain so: the call is on the owned task, so it takes the **direct** path, not the queue. `AffaTask`'s methods must detect "already on the owned task" and call through directly rather than self-enqueueing (which would deadlock a full queue). |

**6.8 is the subtle one** and the most likely implementation bug. The intended and tested
shape in §4.3 of `API.md` is `poll()` → `KeyCb` → application calls `abortPending()` →
`CompleteCb`. If `AffaTask::abortPending()` blindly enqueues, that pattern breaks.

---

## 7. Diagnostics the CR should ship with

The three consumer incidents all presented as "the panel is frozen" with every error counter
at zero. The owned task removes the *cause*; it must not remove the *evidence*.

* `Status::pollLateMax` — worst iteration duration seen, reset on read. A user callback that
  blocks shows up here immediately, which is the single number that would have identified
  incidents 1 and 3 in seconds instead of hours.
* `Status::queueDropped`, `Status::doublePolled` — see 6.2, 6.3.
* A rate-limited `AFFA_LOGW` when an iteration exceeds `8 × periodMs`.

---

## 8. What the consumer deletes

Evidence that this is a *move* of proven code rather than new invention. In MegaOpen:

* `src/display/AffaMailbox.{h,cpp}` — ~250 lines, 15 ops, report callback. Becomes
  `src/rtos/AffaCommandQueue`.
* `bleServiceTask` / `netServiceTask` — needed only because the poll task had to be kept
  clean. The *reason* for them disappears; whether they stay is then a normal
  responsiveness decision rather than a correctness one.
* Every `#if FEATURE_DISPLAY_CAN` render call in `loop()` and its gating.

---

## 9. Testing

Host (`test/`, no FreeRTOS — these test the *queue*, not the task):

* a command queue round-trip preserves op, arguments and ticket;
* a full queue returns `kNoTicket` and counts the drop;
* draining N commands enqueues N messages in submission order.

Target (`examples/`, a new `16_owned_task`):

* **key latency**, the acceptance test for §3: inject a key via `LoopbackLink` and assert
  the `KeyCb` fires within `2 × periodMs`, with a render in flight and the command queue
  full. This is the test that must fail if anyone ever routes keys through the queue;
* renders from three tasks at once arrive intact and in order;
* `stop()` mid-transfer leaves no partial ISO-TP on the wire;
* an 8-hour soak with a deliberately blocking callback, asserting `pollLateMax` reports it.

---

## 10. Documentation changes required

* `docs/API.md` §4 — rewrite the threading contract for the two modes (caller-owned and
  library-owned), keeping the existing text as the caller-owned case.
* `docs/API.md` §4.4 — **amend the misleading sentence**, per §2.1 above.
* `docs/API.md` §2403 knob table — `AFFA_ENABLE_TASK` stops being "NOT IMPLEMENTED".
* `docs/PORTING.md` — `src/rtos/` is the one directory a non-FreeRTOS port omits.
* `README.md` — the owned-task mode as the recommended default for ESP32 consumers.

---

## 11. Open questions for the implementer

1. **Should `AFFA_ENABLE_TASK=1` become the default for `ARDUINO_ARCH_ESP32`?** Argues for:
   it is the safe mode and the failure it prevents is expensive. Argues against: it silently
   changes which task existing consumers' callbacks run on, which is a breaking change to
   observable behaviour and belongs in 1.0, not 0.3.0. **Recommendation: keep the default 0
   for 0.3.0**, document loudly, revisit for 1.0.
2. `TxTicket` from a queued command cannot be returned synchronously (the enqueue has not
   happened yet). Options: return a queue-local handle mapped to the real ticket on drain,
   or return `kNoTicket` and require `onComplete` for correlation. The consumer's existing
   mailbox chose the latter and found it honest — *"a posted render cannot return a Result,
   because nothing has been enqueued yet"* — but a ticket is genuinely more useful. Decide
   and state it.
3. Does `stop()` need to be safe from a callback (i.e. from the owned task)? Probably yes
   for a clean shutdown path; it cannot join itself, so it would have to defer.

---

## 12. Relationship to the other open feedback

Independent of this CR, and both from the same bench session:

* `WIRE-SPEC.md` §8.6 `[CAP]` has **fullscreen and popup swapped**. Tested on a real
  Carminat: `setText("PLAINTXT")` over a live `FS-ONE/FS-TWO/FS-THREE` **replaced it**, with
  no `hideFullscreenText()`. The *popup* is the true overlay — with one up, redrawing the
  fullscreen underneath landed (`BASE-A` → `BASE-B`, visible either side of the overlay) and
  the popup stayed on top until `hidePopup()`. See `MegaOpen-feedback.md`.
* `docs/API.md` §4.4, per §2.1 above.
