# AffaDisplay API

Namespace `affa`, C++17, `-fno-exceptions`, `-fno-rtti`. Public signatures take `const char*`,
never Arduino `String`.

This document describes contracts that span several files. The declarations themselves are
in the headers, which are commented; when this file and a header disagree about a
declaration, the header is right.

Related: [`WIRE.md`](WIRE.md) (frame bytes), [`NOTES.md`](NOTES.md) (open questions),
[`../CHANGELOG.md`](../CHANGELOG.md) (release notes), [`../src/AffaConfig.h`](../src/AffaConfig.h)
(every knob).

---

## 1. Source map

`AffaDisplay.h` is the header a consumer includes. `AffaTwai.h` gives the CAN link alone.
`AffaConfig.h` holds every gate and size, and everything includes it first.

**Host** below means the code builds for `platform = native` against the C++17 standard
library alone: no Arduino, no FreeRTOS, no heap after `begin()`.

| Path | Contents | Host |
| --- | --- | :---: |
| `core/AffaTypes.h` | `Frame`, `Key`, `KeyEdge`, `Result`, `Submitted`, `SyncState`, `Feature`, `TxTicket`, `RenderSlot`, `Priority`, `TxOptions`, `Stats` | ✔ |
| `core/ICanLink.h`, `core/IClock.h` | The two ports: a pull-style CAN link, and a clock that gives `millis()` | ✔ |
| `core/IDisplay.h`, `core/PanelGeometry.h` | The panel-agnostic surface and the size query | ✔ |
| `core/AffaDisplayBase.*`, `AffaSync.cpp`, `AffaTx.cpp`, `AffaObserve.cpp` | `poll()`, the opening and heartbeat, the transmit queue and ISO-TP, the frame tap | ✔ |
| `core/AffaDispatch.h` | Bounded multi-producer ring for renders from other tasks | ✔ |
| `core/AffaSyncProfile.h` | The opening sequence as data; one instance per panel family | ✔ |
| `core/WakePacer.h` | How the owner task sleeps between polls | ✔ |
| `carminat/`, `updatelist/`, `cluster/` | One folder per panel family, each behind `AFFA_PANEL_*` | ✔ |
| `link/TwaiLink.*` | `ICanLink` on the ESP-IDF TWAI driver | — |
| `link/TxGate.*` | Decides when to stop transmitting to a bus nobody ACKs | ✔ |
| `link/LoopbackLink.h` | Test double: records TX, injects RX | ✔ |
| `util/AffaLog.*`, `util/AffaText.*` | `ILogSink`, `setLogSink()`, the `AFFA_LOG*` macros; `toAscii`, `normalizeTitle` | ✔ |
| `rtos/AffaTask.*` | The library-owned poll task. The only FreeRTOS code. | — |

An unselected panel or feature compiles to an empty object file, so it costs no flash.

---

## 2. Calling a panel

```cpp
affa::TwaiLink        link;
ArduinoClock          clk;                    // your IClock
affa::CarminatDisplay panel(link, clk);
affa::rtos::AffaTask  task;

affa::TwaiConfig cfg; cfg.rx = GPIO_NUM_5; cfg.tx = GPIO_NUM_4;
link.begin(cfg);
panel.begin();
task.start(panel);                            // or call panel.poll() yourself, §4

if (const affa::Submitted s = panel.setText("HELLO")) { /* s.ticket */ }
else                                                  { /* s.result says why */ }
```

- Every render returns `Submitted{ticket, result}`. It converts to `bool` and is `[[nodiscard]]`.
- Renders only enqueue. Nothing in the library sleeps or waits for an ACK.
- `supports(Feature)` says whether a call will do anything on this panel with this build;
  `panelGeometry()` says how much fits (§6).
- `showNavBitmap()`, `showMenuN()`, `enqueueExternal()` and `enqueueSplit()` copy their data
  into one of `AFFA_BULK_SLOTS` pool slots, so your buffer is free when the call returns.

---

## 3. Results

The return value answers **was it accepted**. `onComplete(ticket, result)` answers **did the
panel take it**.

| `Result` | At the call | Via `onComplete` | Meaning |
| --- | :---: | :---: | --- |
| `Ok` | ✔ | ✔ | Call: accepted. Complete: the panel ACKed the last frame (`0x74`). |
| `NoSync` | only if `AFFA_TX_HOLD_MS=0` | ✔ | No session. By default the render is held and completes `NoSync` if the opening has not finished within `AFFA_TX_HOLD_MS` (8 s). |
| `LinkDown` | only if `AFFA_TX_HOLD_MS=0` | ✔ | `ICanLink::isLive()` is false (no controller, bus-off, listen-only, TX gate shut). Held like `NoSync`. |
| `UnknownFunc` | ✔ | | The function id is not in this panel's table. |
| `TooLong` | ✔ | | Over `AFFA_MAX_PAYLOAD` (119) or `AFFA_MAX_EXTERNAL_PAYLOAD` (320). Text longer than its field is cut to fit, not refused. |
| `BadArgument` | ✔ | | Null pointer, zero length, row or index out of range, a `RenderSlot` on a long payload. |
| `NotSupported` | ✔ | | `supports()` is false for this call. |
| `QueueFull` | ✔ | ✔ | Call: the transmit queue, the dispatch ring or the pool is full. Complete: a render posted from another task found the queue full when the owner admitted it. |
| `SendFailed` | | ✔ | The panel answered with something other than an ACK or flow control, or the link refused a frame while reporting live. Not retried. |
| `Timeout` | | ✔ | No ACK within `AFFA_ACK_TIMEOUT_MS` (2 s) after `AFFA_TX_MAX_RETRIES` (3) retries. |
| `Cancelled` | | ✔ | `begin()` ran again while the job was queued. |
| `Aborted` | | ✔ | Dropped before any byte went out: `abortPending()`, `abortAll()`, `AffaTask::stop()`, or a newer render of the same slot (§3.3). |

- Every issued ticket completes exactly once. A refused call (`ticket == kNoTicket`) never
  completes; its return value is the verdict.
- Tickets are issued in increasing order but may complete out of order (`Urgent`,
  coalescing). Match the exact ticket.
- `lastResult()` / `lastTicket()` report the most recent completion. They are diagnostics, not a
  way to learn about the call you just made.
- `pressKey()` also returns `Submitted`, and it never queues behind display traffic.
  - On the owner task, a refusal (`NotSupported`, `NoSync`, `LinkDown`, `SendFailed`) is the
    return value, and an accepted press completes `Ok` before the call returns.
  - From another task, the press is posted, and the owner's verdict arrives through `onComplete`.

### 3.1 Retries and holding

The library does the recovery work, so the application does not have to:

| Situation | What the library does |
| --- | --- |
| no ACK | retries up to `AFFA_TX_MAX_RETRIES`, backoff from `AFFA_TX_RETRY_MS` doubling to `AFFA_TX_RETRY_MAX_MS`; reports once at the end |
| a transfer torn mid-way | waits `AFFA_TX_DIRTY_QUIET_MS` before the next attempt |
| link down or no session | holds the render for up to `AFFA_TX_HOLD_MS` and sends it when the link is back; if the hold runs out first, it completes `NoSync` or `LinkDown` |
| panel lost and found again | keeps the queue; re-registers in front of the held payload |

`SendFailed`, `Aborted` and `Cancelled` are never retried. One ticket can therefore take about
11 s at the defaults before its verdict arrives, so use `Stats` / `AffaTask::Status` for
health, not completion latency. A job waiting out a backoff holds the queue behind it, which
keeps renders in order; an `Urgent` job still goes ahead of a waiting `Normal` one.

To get one attempt with every refusal at the call, set `AFFA_TX_MAX_RETRIES=0` and
`AFFA_TX_HOLD_MS=0`.

If you retry a refused call yourself, retry against a deadline from `IClock::millis()`. A
refusal returns at once, so a bare retry in a loop spins at loop rate while the panel is
unplugged:

```cpp
if (pending && affa::expired(now, retryAt)) {
  if (panel.setTime("1000")) pending = false;
  else                       retryAt = now + 1000;
}
```

### 3.2 Order inside `poll()`

```
poll():
  1. pumpLink()  the link's own service: recovery, the RX-stall watchdog
  2. pumpRx()    up to AFFA_RX_BUDGET (64) frames: sync, ACKs, keys -> onKey / onComplete
  3. pumpSync()  heartbeat, sync request, peer watchdog
  4. admit       renders posted from other tasks
  5. pumpTx()    send at most one frame; check the ACK deadline
  6. the Settling -> Ready phase edge, then onPoll()
```

A key that is in the RX queue when `poll()` starts reaches `onKey()` during that same `poll()`,
whatever is queued or in flight. Frames over the budget wait for the next `poll()`.

### 3.3 Coalescing

A render replaces a queued render that has **not started** (no frame sent yet) when the two share
a `RenderSlot` and a function id. The newer render takes over the older one's place in the
queue, and the older ticket completes `Aborted`. A transfer that has started is never changed.

| Slot | Calls |
| --- | --- |
| `Text` | `setText` |
| `Clock` | `setTime` |
| `Menu` | `showMenu` |
| `Highlight` | `highlightItem`, `selectMenuItem`, `selectBoxButton` (separate from `Menu` on purpose) |
| `Popup` | `showPopupText`, `hidePopup` (a queued show followed by a hide sends nothing) |
| `Fullscreen`, `ConfirmBox`, `InfoPopup` | the matching call |
| `Control` | `setPower` (queued `Urgent`, and replayed after a session reset) |
| `None` | raw `enqueue()`, never coalesced |

With `TxOptions::coalesce = false`, a message neither replaces nor is replaced; use it for an
animation, where every frame should show. `AFFA_TX_COALESCE=0` turns coalescing off everywhere.
With coalescing on, a render repeated in one slot takes up one queue entry however often you
call it. That is why a queue depth of 6 is enough.

### 3.4 Preemption

| Call | Effect |
| --- | --- |
| `abortPending()` | Drops every unstarted render; each one completes `Aborted`. Returns the count. Leaves the frame on the wire and registration alone. |
| `abortAll()` | As above, and also abandons the job in flight at the next frame boundary. Whether the panel recovers cleanly from the partial transfer has not been verified on hardware. |
| `TxOptions::priority = Priority::Urgent` | Goes after the job in flight and any registration, ahead of every queued `Normal` job. Urgent jobs are FIFO among themselves. |

Usual pattern: in `onKey()`, call `abortPending()` and then the reaction render. Coalescing
already handles repeated renders of the same slot.

**Key latency:** the key reaches `onKey()` within one poll. The reaction reaches the wire after
whatever frames remain of the transfer in flight, plus one ACK round-trip per frame.

**Test coverage:** the wire vectors, the opening, the RX budget, cross-task dispatch, the pool
and the TX gate have host tests, and so do `Urgent` ordering (FIFO, behind registration) and
the coalescing of two `setPower` calls. The other coalescing and preemption rules above do
not have tests of their own yet.

---

## 4. Threading

| | `AFFA_ENABLE_TASK=1` (default on ESP32) | `AFFA_ENABLE_TASK=0` |
| --- | --- | --- |
| Who calls `poll()` | `affa::rtos::AffaTask` | you, from one task only |
| Who may render | any task | that same task |
| What breaks it | a callback that blocks | anything that blocks that task |

### 4.1 From any task

When a poll owner is registered (`AffaTask`, or your own `setPollOwner()`), calls made off the
owner task are posted to the dispatch ring (`AFFA_DISPATCH_DEPTH`, 8) and return at once. This
covers every render, `enqueue*()`, `abortPending()`, `abortAll()`, `pressKey()` and `begin()`.
The owner applies them inside `poll()`.

- A full ring returns `QueueFull`.
- Long payloads cross as a pool slot. No pointer into your memory reaches the owner task.
- Calls made from inside a callback are already on the owner task, so they run directly.
- Getters such as `syncState()`, `busy()`, `stats()` and `supports()` are plain reads and may be
  one poll stale. Read `task.status()` instead for one consistent snapshot.
- `poll()` from any task other than the owner does nothing and counts in `foreignPolls()`.
- Install `onKey`, `onComplete`, `onSync` and `onFrame` before `begin()`.
- Nothing is safe from an ISR. Queue the event and replay it with `pressKey()` from a task.

### 4.2 Callbacks

| Callback | Runs on | May call into the library? |
| --- | --- | --- |
| `onKey` | the poll task (or the `pressKey` caller on the owner) | yes, except `poll()` |
| `onComplete` | the poll task, possibly nested inside `onKey` after an abort | yes, except `poll()` |
| `onSync` | the poll task | yes, except `poll()` |
| `onFrame` | the poll task (RX), or the task that sent (TX) | yes, but it sees every frame: keep it short |
| `ILogSink::write` | any of the above, and `TwaiLink::begin()` | **no** |

The library updates its own state before it fires a callback, so a callback sees a consistent
state and may call back in. A nested `poll()` returns at once. Callbacks must not block: they run
inside `poll()`.

### 4.3 Timing

Timers are wall-clock deadlines against `IClock::millis()`, and nothing counts calls. The
frames sent therefore do not depend on how often `poll()` runs. Delivery does: an ACK that waits
in the RX queue past `AFFA_ACK_TIMEOUT_MS` still counts as a `Timeout`. If anything can stall
your poll task that long, use the owned task.

`TwaiLink`'s RX queue holds 64 frames, about 14 ms of a saturated 500 kbit/s bus between polls.

### 4.4 The owned task

```cpp
panel.onKey(&onKey, nullptr);
panel.onComplete(&onDone, nullptr);
panel.begin();
task.start(panel);           // TaskOptions: period, priority, core, stack
```

- Between polls it sleeps in `ICanLink::waitRx()`, so a key frame wakes it at once. A link that
  cannot wait uses the fixed `AFFA_TASK_PERIOD_MS` (2 ms). When frames keep arriving
  back-to-back, `WakePacer` yields a tick on every second early wake.
- Priority `AFFA_TASK_PRIO` is 2, above Arduino's `loop()`. The stack is `AFFA_TASK_STACK`
  (4096).
- `onKey()` runs on this task, inside `poll()`. Put slow work (WiFi, NVS) on your own task.
- `stop()` drops unstarted renders (they complete `Aborted`), lets the frame on the wire finish (at most two ACK
  timeouts), and then joins. When called from a callback it only requests the stop.
- `start()` before `begin()`, a second `start()`, or a failed task creation each return `false`
  and log the reason.
- There is no after-poll hook. An application that must run code after every `poll()` keeps its
  own loop: call `waitRx()`, use `WakePacer`, and register itself with `setPollOwner()`.

| `task.status()` field | Healthy | Otherwise |
| --- | --- | --- |
| `foreignPolls` | 0 | something else still calls `poll()` |
| `pollLateMaxUs` | near the period (472 µs measured at 2 ms) | a callback blocks; logged once a second past `AFFA_TASK_LATE_FACTOR` × period |
| `postDropped` | 0 | renders are posted faster than they drain; gate on `status().busy` |
| `stackFreeBytes` | about half of the stack | the callback chain is deeper than the stack |

### 4.5 Your own loop (`AFFA_ENABLE_TASK=0`)

Call `poll()` from exactly one task, and make every other call from that task too. Keep the
cheap part of a key reaction in `onKey()`, that is `abortPending()` and the reaction render.
Queue the slow part to the same task:

```cpp
for (;;) {
  panel.poll();                                   // may fire onKey
  KeyEvent ev;
  while (xQueueReceive(g_keys, &ev, 0) == pdTRUE) handleSlowly(ev);
  vTaskDelay(pdMS_TO_TICKS(2));
}
```

---

## 5. Configuration

Set knobs in your `build_flags`. `src/AffaConfig.h` supplies the defaults and documents each one.

- Every gate is always defined, as `0` or `1`, which lets `-Wundef` catch a misspelling inside the
  library. It cannot catch one in your flags, so naming no panel is an `#error`.
- Each optional `.cpp` gates its whole body, so an unselected panel costs nothing.

| Group | Knobs |
| --- | --- |
| Panels | `AFFA_PANEL_CARMINAT`, `AFFA_PANEL_UPDATELIST`, `AFFA_PANEL_CLUSTER` (never on by default), `AFFA_PANEL_DEFAULT_ALL` |
| Features | `AFFA_ENABLE_POPUP`, `_FULLSCREEN`, `_CONFIRMBOX`, `_INFOPOPUP`, `_BIGMENU`, `_NAV`; off means `NotSupported` |
| Text, log | `AFFA_ENABLE_TRANSLITERATION` (keep on: UTF-8 on the wire is garbage on glass), `AFFA_ENABLE_LOG`, `AFFA_LOG_LEVEL`, `AFFA_TEXT_MAX` |
| Link | `AFFA_RX_BUDGET`, `AFFA_RX_STALL_MS`, `AFFA_LINK_RECOVER_MS`, `AFFA_LINK_RECOVER_MAX_MS`; `TwaiLink` itself takes `TwaiConfig` at run time (pins, bitrate, listen-only, queue depths) |
| Transmit | `AFFA_TX_QUEUE_DEPTH` (6), `AFFA_MAX_PAYLOAD` (119; above that needs `AFFA_UNSAFE_LONG_PAYLOAD`), `AFFA_MAX_EXTERNAL_PAYLOAD`, `AFFA_BULK_SLOTS` (2), `AFFA_ACK_TIMEOUT_MS`, `AFFA_TX_MAX_RETRIES`, `AFFA_TX_RETRY_MS`, `AFFA_TX_RETRY_MAX_MS`, `AFFA_TX_HOLD_MS`, `AFFA_TX_COALESCE`, `AFFA_TX_DIRTY_QUIET_MS` |
| Opening | `AFFA_SYNC_INTERVAL_MS`, `AFFA_HELLO_MIN_MS`, `AFFA_AWAIT_PEER_MS`, `AFFA_PING_REPLY_MIN_MS`, `AFFA_PEER_TIMEOUT_MS`; the rest is `SyncProfile` data |
| Owned task | `AFFA_ENABLE_TASK`, `AFFA_TASK_PERIOD_MS`, `_PRIO`, `_CORE`, `_STACK`, `_LATE_FACTOR`, `AFFA_DISPATCH_DEPTH` (power of two; 0 removes the ring), `AFFA_CALLBACK_BUDGET_MS` |

Set the log sink with `affa::setLogSink(ILogSink*)`, which works with or without a display.
`AffaDisplayBase::setLogSink()` does the same thing.

---

## 6. Capabilities and geometry

| `Feature` | Carminat | UpdateList | Cluster |
| --- | :---: | :---: | :---: |
| `Text` | ✔ | ✔ | — (encoding unknown) |
| `Time` | ✔ | — | — |
| `Power` | ✔ | ✔ | ✔ |
| `Menu` | ✔ | — | — |
| `Popup`, `Fullscreen`, `ConfirmBox`, `InfoPopup`, `NavBitmap` | ✔ if its gate is on | — | — |
| `KeyTx` | ✔ (`0x1C1`) | ✔ (`0x0A9`) | ✔ (`0x1C1`) |

`supports()` reflects the compile gates too. A Carminat built with `AFFA_ENABLE_POPUP=0` reports
no `Popup`, and `showPopupText` returns `NotSupported`.

`panelGeometry()`: a field that is zero means the panel has no such surface.

| | `mainChars` | menu rows × chars | info rows × chars | `listMaxItems` | image |
| --- | :---: | :---: | :---: | :---: | :---: |
| Carminat | 8 | 2 × 26 | 3 × 8 | 10 | 48 × 48 |
| UpdateList | 8 | 0 | 0 | 0 | — |
| Cluster | 0 | 0 | 0 | 0 | — |

UpdateList's 8 is the width every panel in that family is known to show. The frame carries 12
cells, and a wider glass will show them.

`infoCells` is how many cells `showInfoMenu()` puts in each row's frame by default (8 on
Carminat, the OEM list's declared length 0x0B). `showInfoMenu(..., cells)` takes 1 to 16; the
declared length is then 3 + cells. Only 8 has been seen on glass; the box looks wider, and a
glass sweep decides whether a wider count shows.

---

## 7. Keys

The library decodes `0x1C1` / `0x0A9` into `(Key, KeyEdge)`, acknowledges the frame on the bus
within the panel's window, and calls `onKey()`. It ignores frames that only look like keys
(the `0x03 89` guard). It does not track selection, and it does not interpret long presses.

```cpp
panel.onKey([](affa::Key k, affa::KeyEdge e, void* ctx) {
  if (e == affa::KeyEdge::Click && k == affa::Key::Down) static_cast<App*>(ctx)->next();
}, app);
```

`pressKey(key, edge, KeySource)` injects a key. `Local` (the default) runs your `onKey`; with no
`onKey` installed it returns `NotSupported`. `Wire` only puts the key frame on the bus, where the
panel has a key-transmit id (`Feature::KeyTx`), and does not run `onKey`. `Both` does both.
