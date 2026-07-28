# ESP32CAN contract

What `collin80/esp32_can` actually guarantees, as read from its source, and what
AffaDisplay is therefore allowed to do. This is not a summary of the library's README;
several statements below contradict it. Every claim carries a file:line citation so it
can be re-checked when the dependency is bumped.

## Provenance of this reading

| Item | Value |
|---|---|
| Driver | `collin80/esp32_can`, `library.properties` version `0.3.1`, commit **`c329e6be6931e86f82e38e0f982c9ed951c45cca`**, committed 2026-07-09, first cloned 2026-07-25. PlatformIO spells it `ESP32_CAN@0.3.1+sha.c329e6b`. |
| Base class | `collin80/can_common` **`0.4.0`** (registry package, `.piopm` `{"version": "0.4.0"}`) |
| Commit re-verified | 2026-07-27, `git rev-parse HEAD` in `AffaDisplay/.pio/libdeps/*/ESP32_CAN` and in `MegaOpen/.pio/libdeps/c3/ESP32_CAN` — both `c329e6be…`, and a fresh clone of `master` that day resolved to the same commit |
| Vendored at | `AffaDisplay/.pio/libdeps/<env>/ESP32_CAN/src/` and `.../can_common/src/`; the original reading was taken in `MegaOpen/.pio/libdeps/c3/` |
| Framework | `framework-arduinoespressif32` 3.20017.241212 (Arduino core 2.0.17) |
| ESP-IDF | 4.4.7 — `tools/sdk/esp32c3/include/esp_common/include/esp_idf_version.h:22-26` |
| Target | ESP32-C3, `CONFIG_FREERTOS_UNICORE=y`, `CONFIG_FREERTOS_HZ=1000`, `CONFIG_FREERTOS_ISR_STACKSIZE=2096` — `tools/sdk/esp32c3/sdkconfig:1199,1206,1218` |

**Two levels, cited differently.** The `esp32_can`/`can_common` wrapper is source, and every
claim about it below is read off that source with a `file:line`. The TWAI driver underneath
it ships **precompiled** with the Arduino core (`tools/sdk/esp32c3/lib/libdriver.a`; no
`twai.c` exists anywhere in the package), so every claim about `twai_*` behaviour is quoted
from the IDF 4.4.7 **header contract** in `driver/twai.h` — the documented guarantee, not the
implementation. Where the distinction could matter it is stated inline. Anything that depends
on undocumented driver internals is not asserted here at all.

**The version gate matters.** Every `#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)`
branch in the driver is dead code for us: at IDF 4.4.7 the `_v2` handle-based TWAI API,
the `controller_id` field and the second controller `CAN1` are all absent. Note also that
`SOC_TWAI_CONTROLLER_NUM` is not defined at all in IDF 4.4 (it is not in
`soc/esp32c3/include/soc/soc_caps.h`), so `#if SOC_TWAI_CONTROLLER_NUM == 2`
(`esp32_can.h:11`) evaluates as `0 == 2`, i.e. false, by the usual undefined-identifier
rule. There is exactly one global bus object, `CAN0`, and it is declared weak
(`esp32_can.cpp:9`), so a project may replace it wholesale rather than call
`setCANPins()`.

**The dependency is pinned, and this document is why.** `0.3.1` in the driver's
`library.properties` has not moved in years — it identifies nothing — so the only usable
handle is the commit. Every declaration in this repository names it:

| Where | Spec |
|---|---|
| `library.json` | `https://github.com/collin80/esp32_can.git#c329e6be6931e86f82e38e0f982c9ed951c45cca`, `can_common` `0.4.0` |
| `platformio.ini` (`[esp32c3]`, `[env:ex90_bench_ota]`) | the same two |
| `platformio_footprint.ini` (`[c3]`, `[env:b_can]`) | the same two |
| README installation snippet, EN and UK | the same two |

Every `file:line` below is an offset into **that** commit's source. A bare
`https://github.com/collin80/esp32_can.git` — which is what MegaOpen still declares,
`MegaOpen/platformio.ini:24` — takes whatever `master` is on the day of a clean build, and
under that arrangement every line citation here rots silently: the numbers still look
authoritative and no build fails. Rule 21 makes re-reading this document part
of any bump, and the pins are what make that rule enforceable rather than a hope.

---

## 1. Task topology, and where the general callback runs

Everything the driver creates, in creation order:

| Task | Name | Prio | Stack (bytes) | Core | Created in |
|---|---|---|---|---|---|
| `CAN_WatchDog_Builtin` | `"CAN_WD_BI"` | 10 | 2048 | any (unicore) | `_init()` — `esp32_can_builtin.cpp:339,343` |
| `task_CAN` | `"CAN_RX_CAN"` | 15 | 8192 | any (unpinned `xTaskCreate`) | `enable()` — `esp32_can_builtin.cpp:494,499` |
| `task_LowLevelRX` | `"CAN_LORX_CAN"` | 19 | 4096 | any (unicore branch) | `enable()` — `esp32_can_builtin.cpp:495,503` |
| `ForceRecoveryTask` | `"ForceRecoveryTask"` | 10 | 4096 | any | spawned on bus-off, only if force-recovery is enabled — `esp32_can_builtin.cpp:177` |

ESP-IDF FreeRTOS takes `usStackDepth` in **bytes**, not words, so those figures are the
real stack sizes.

On a dual-core part `task_LowLevelRX` is pinned to core 1 (`:506`) and the watchdog is
pinned to core 1 (`:345`); on the C3 `CONFIG_FREERTOS_UNICORE` selects the `xTaskCreate`
branches at `:343` and `:503`. `task_CAN` is **never** pinned on any part (`:499`) — it is
free to run on either core of a dual-core device. That single fact decides the memory
model of our RX ring (see rule 7).

Queues:

| Queue | Depth | Element | Bytes | Created in |
|---|---|---|---|---|
| `callbackQueue` | 16 | `CAN_FRAME` (24 B) | 384 | `_init()` — `esp32_can_builtin.cpp:330` |
| `rx_queue` | `rxBufferSize` = `BI_RX_BUFFER_SIZE` = 64 | `CAN_FRAME` | 1536 | `_init()` — `:331`, size from `esp32_can_builtin.h:50` |
| TWAI driver TX queue | `BI_TX_BUFFER_SIZE` = 16 | driver-internal | — | `twai_driver_install`, len set in ctor `:48`, size `esp32_can_builtin.h:51` |
| TWAI driver RX queue | **6** | driver-internal | — | ctor `:49` (overrides the macro default of 5) |

All four queues, and the watchdog task, are created from **the caller's own task** —
`begin()` -> `init()` -> `_init()` (`:352-355`), i.e. normally `loopTask` inside `setup()`.
`task_CAN` and `task_LowLevelRX` are created from the caller's task too, because
`init()` -> `set_baudrate()` -> `enable()` (`:357`, `:441`).

The RX path in full:

```
TWAI ISR  ->  driver RX queue (6)
              |
              v  twai_receive(&message, pdMS_TO_TICKS(100))      esp32_can_builtin.cpp:213
        task_LowLevelRX (prio 19)  ->  processFrame()            :217, :575
              |                        software filter match     :589-593
              v  xQueueSend(callbackQueue, &msg, 0)              :599 / :605
        callbackQueue (depth 16)
              |
              v  xQueueReceive(..., portMAX_DELAY)               :243
        task_CAN (prio 15)  ->  sendCallback(&rxFrame)           :245, :252
              |
              v  (*cbGeneral)(frame)                             :274
        OUR general callback
```

**The general callback registered by `setGeneralCallback()` executes in `task_CAN`, at
priority 15, on `task_CAN`'s 8192-byte stack.** `setGeneralCallback()` itself only stores
the pointer (`can_common.cpp:250-253`); the dispatch is `sendCallback()`:

```cpp
    else // C function callback
    {
        if (mb > -1)
            (*cbCANFrame[mb])(frame);
        else
            (*cbGeneral)(frame);                    // esp32_can_builtin.cpp:274
    }
```

It is *not* an ISR context, and it is *not* the caller's loop task. It preempts `loop()`
(Arduino `loopTask` runs at priority 1) at any instruction boundary.

Two consequences that shape our design:

* The `CAN_FRAME*` handed to the callback points at `task_CAN`'s stack local
  `CAN_FRAME rxFrame` (`:238`). It is valid only for the duration of the call. Anything
  we keep must be **copied**, not referenced.
* `processFrame()` never writes `msg.timestamp` (it sets id/length/rtr/extended/data only,
  `:582-587`), so `timestamp` is always the 0 left by the `CAN_FRAME` constructor
  (`can_common.cpp:17`). Arrival time must be stamped by us, from `IClock::millis()`, at
  the moment we take the frame off the ring — the driver supplies none.

**Ordering is guaranteed** as long as nothing is dropped: one RX task, one FIFO, one
consumer. ISO-TP consecutive frames therefore arrive in wire order.

**Silent loss is guaranteed when it is not.** `processFrame()` posts with a zero timeout
(`:599`, `:605`) and returns `true` regardless of whether the post succeeded. A full
`callbackQueue` discards frames with no counter, no return code and no alert. Sixteen
slots is not much elasticity: at 500 kbit/s one bit is 2 µs, an 8-byte standard data frame
is 111 bit-times before stuffing (222 µs) and a zero-length one is 47 (94 µs), so
back-to-back traffic fills the queue in **1.5 ms to 3.6 ms** depending on frame size.
That is the entire budget for any stall in `task_CAN`. The driver's own RX queue is only 6
deep (`:49`) and its overruns are counted by the TWAI driver, not by the wrapper. So the
only way to tell "the panel never answered" from "we threw its answer away" is to read
`twai_get_status_info().rx_missed_count` / `.rx_overrun_count` from the consumer side and
to count our own ring overflows.

**A third loss mechanism sits below all of that: the TWAI ISR runs from flash.**
`# CONFIG_TWAI_ISR_IN_IRAM is not set` (`tools/sdk/esp32c3/sdkconfig:776`), and the driver
is installed with `.intr_flags = ESP_INTR_FLAG_LEVEL1` (`twai.h:36`), not
`ESP_INTR_FLAG_IRAM`. While the flash cache is disabled the ISR cannot execute at all, so
frames arriving during an OTA write, an NVS commit or a LittleFS write accumulate in the
controller's hardware RX FIFO until it overruns; the driver's own RX queue is only 6 deep
(`esp32_can_builtin.cpp:49`) behind it. The losses land in `rx_missed_count` /
`rx_overrun_count` and nowhere else. The library cannot prevent this and must not
misdiagnose it: a flash-write blackout looks exactly like a panel that stopped talking.
The 5000 ms peer deadline is what absorbs it, and that is the reason the deadline is
milliseconds rather than a count of polls. It is also what `examples/90_bench_ota`
measures — the example exists to size this window, not to demonstrate a feature.

Related, and easy to trip over: once a general callback is registered, `rx_queue` is never
written at all — `processFrame()` returns at `:602-607` before reaching the
`xQueueSend(rx_queue, ...)` at `:632`. `can_common.cpp:248` states this as intended
behaviour ("If this function is used to set up a callback then no buffering of frames will
ever take place"). `CAN0.available()`, `rx_avail()` and `read()` will therefore report
nothing forever. Those 1536 bytes are dead RAM in our configuration; `setRXBufferSize(1)`
**before** `begin()` reclaims all but 24 of them, and is safe only because we never call
`available()`/`get_rx_buff()` afterwards. Note the floor: the argument must be ≥ 1.
`rx_avail()` and `available()` null-check `rx_queue` (`:686`, `:693`) but `get_rx_buff()`
does not (`:702`), so `setRXBufferSize(0)` — a queue FreeRTOS refuses to create — turns a
`read()` into an assertion failure rather than a `false`.

---

## 2. Is `sendFrame()` blocking? What does it return?

`sendFrame()` **blocks for up to 4 FreeRTOS ticks**, and **always returns `true`**.

```cpp
    result = twai_transmit(&__TX_frame, pdMS_TO_TICKS(4));      // esp32_can_builtin.cpp:660
    switch (result)
    {
    case ESP_OK:            if (debuggingMode) Serial.write('<'); break;   // :664-667
    case ESP_ERR_TIMEOUT:   if (debuggingMode) Serial.write('T'); break;   // :668-671
    case ESP_ERR_INVALID_ARG:
    case ESP_FAIL:
    case ESP_ERR_INVALID_STATE:
    case ESP_ERR_NOT_SUPPORTED:
                            if (debuggingMode) Serial.write('!'); break;   // :672-678
    }

    return true;                                                // :681
```

* With `CONFIG_FREERTOS_HZ=1000`, `pdMS_TO_TICKS(4)` is 4 ticks, i.e. up to ~4 ms of
  blocking — but only when the driver's 16-deep TX queue is already full. A frame that
  fits in the queue is copied and returned from immediately; the actual transmission is
  done by the TWAI ISR (`driver/twai.h:196-203`).
* On timeout the frame is **dropped**, not retried, not queued elsewhere.
* Because `sendFrame()` returns a literal `true` on every path, there is **no failure
  signal in the return value at all**. Not queue-full, not "driver not installed", not
  "listen-only mode refuses transmissions". Calling `sendFrame()` before `begin()`, or
  after the controller has been left stopped (see finding 3), succeeds silently.
* The only observable failure signals available to a caller are, from
  `twai_get_status_info()` (`driver/twai.h:106-117`): `state`, `msgs_to_tx` (frames queued
  or awaiting completion), `tx_failed_count`, `tx_error_counter`, `arb_lost_count`,
  `bus_error_count`. Plus the debug characters above, which require
  `setDebuggingMode(true)` and are useless in production.
* Note the driver also never checks `readyForTraffic` in `sendFrame()` — that flag gates
  only `task_LowLevelRX` (`:207`).
* Latent, not currently biting: `__TX_frame` is an uninitialised local, and `sendFrame()`
  writes only `extd`, `rtr`, `ss`, `self` and `dlc_non_comp` of the flags union
  (`:647-651`), leaving its 27 `reserved` bits as whatever was on the stack
  (`hal/twai_types.h:103`). The IDF 4.4.7 driver reads the named bitfields individually and
  never the deprecated `flags` word (`:106`), so this is harmless today. It stops being
  harmless the moment a driver revision validates `flags`. Nothing can be done about it
  from outside the driver; it is recorded here so that a mystery `ESP_ERR_INVALID_ARG`
  after a dependency bump has a first suspect.

So `ICanLink::send()` "never blocks" must be stated in the library as **bounded**, not
absolute: worst case ~4 ms, and only under a TX backlog we ourselves create. Our own
frame rate — a heartbeat at 1 Hz plus at most a handful of ISO-TP frames per screen —
cannot fill a 16-deep queue on a healthy bus. It *can* fill it instantly on a bus where
nobody acknowledges: `sendFrame()` hard-codes `__TX_frame.ss = 0` (`:650`), i.e. not
single-shot, so the controller retransmits an unacknowledged frame forever and the head of
the TX queue never retires. One absent panel therefore turns every subsequent `sendFrame()`
into a 4 ms stall followed by a silent drop, with `true` returned each time. (This is the
other half of the ~2000 frames/s seen during MegaOpen's bring-up — that was our own
retransmissions, not a busy bus: `MegaOpen/src/main.cpp:534-537`.) That is the case worth
defending against: `Esp32CanLink` must treat "TX queue depth is not draining" as the
trigger for its own reporting, since the return code will never tell it.

---

## 3. The watchdog task: what it does on bus-off, and what `setForceRecovery()` changes

`CAN_WatchDog_Builtin` (`esp32_can_builtin.cpp:147-194`) wakes every 200 ms (`:149,:154`),
increments `cyclesSinceTraffic` (`:159`) and reads `twai_get_status_info()` (`:165`; the
`_v2` call at `:163` is the dead IDF ≥ 5.2 branch). It acts on exactly one condition:

```cpp
        if (status_info.state == TWAI_STATE_BUS_OFF)              // :169
        {
            espCan->cyclesSinceTraffic = 0;
            if (espCan->forceRecoveryEnabled && !espCan->forcedRecoveryInProgress)
            {
                espCan->forcedRecoveryInProgress = true;
                xTaskCreate(ForceRecoveryTask, "ForceRecoveryTask", 4096, espCan, 10, NULL);  // :177
            }
            else
            {
                result = twai_initiate_recovery();                // :184
            }
        }
```

* **On "no traffic" it does nothing.** `cyclesSinceTraffic` is incremented and reset
  (`processFrame()` resets it at `:580`) but is only ever *read* by `beginAutoSpeed()`
  (`:408`). A silent bus produces no action whatsoever. There is no keep-alive, no
  re-arm, no restart on idle.
* **Default path (force recovery off, which is the default —** `:58`, `:84`**)**: it calls
  `twai_initiate_recovery()`. Per `driver/twai.h:286-290`, that makes the controller wait
  for 128 occurrences of the bus-free signal "before returning to the **stopped** state",
  and it clears the TX queue.

  **Nothing ever restarts it.** The watchdog only matches `TWAI_STATE_BUS_OFF`; it has no
  `TWAI_STATE_STOPPED` branch, and no code path in the whole library calls `twai_start()`
  outside `enable()`. After one bus-off the controller therefore ends up **stopped for
  good**: `twai_receive()` in `task_LowLevelRX` fails, `twai_transmit()` in `sendFrame()`
  returns `ESP_ERR_INVALID_STATE` — and `sendFrame()` still returns `true` (finding 2).
  The link is dead and every API in the library reports success. This is the exact
  failure mode ("the controller was left stopped") that dogged the previous project, and
  it is a defect in the driver's watchdog, not in the wiring.
* **`setForceRecovery(true, delayMs)`** (`:88-92`) switches the bus-off branch to spawning
  `ForceRecoveryTask`, which calls `forceDriverRestart()` (`:94-104`): `disable()`,
  `vTaskDelay(forceRecoveryDelay)` (default 2000 ms, `:59`), `enable()`. That is a full
  uninstall/reinstall plus task teardown and recreation — see finding 4 for what that
  costs — but it does at least end with a `twai_start()` and a running controller. It
  also `printf()`s unconditionally (`:96`, `:103`).

So the honest position is: the driver's watchdog **owns** bus-off detection, and we must
not add a second one that also calls `twai_initiate_recovery()` — two recovery initiators
racing on the same controller is how you get a half-recovered peripheral. But the driver's
default recovery **does not restore service**. AffaDisplay's answer:

* Never call `twai_initiate_recovery()`, `twai_start()`, `twai_stop()`, `enable()` or
  `disable()` ourselves.
* `Esp32CanLink::isLive()` reads `twai_get_status_info().state` and returns `true` only for
  `TWAI_STATE_RUNNING`. That is a pure read; it changes nothing. `AffaDisplayBase` treats
  `!isLive()` as loss of sync and stops enqueueing, so the application sees the truth
  instead of frames vanishing into a stopped controller.
* If an application wants automatic restoration, it enables the driver's **own** supported
  path — `CAN0.setForceRecovery(true)` — **before** `begin()`, and accepts the 2-second
  outage. AffaDisplay documents this and does not do it on the application's behalf,
  because it is a policy decision about a shared bus, and because of the reinstall hazards
  in finding 4.

`Esp32CanLink::begin(pins, bitrate, forceRecoveryMs, mode)` is the one path the prohibition
permits, because `setForceRecovery()` is armed **before** `begin()` rather than after.

**Why the forced path and not standard recovery, on a two-node bus.**
`twai_initiate_recovery()` waits for 128 occurrences of 11 consecutive recessive bits —
i.e. it needs *bus traffic* to complete. On a two-node bus there is none to wait for: once
we stop ACKing, the panel goes quiet as well, and standard recovery never finishes. The
forced path is a timed uninstall/reinstall and needs no traffic at all, which is why it is
the only one that works here. Bench value **250 ms** — see the bench notes for why 0 and
2000 are both wrong.

### 3.1 Reading the controller during bring-up

`Esp32CanLink::driverState()` is a pure `twai_get_status_info()` read, exposed for bring-up.
Nothing in the library reacts to it.

| Field | Meaning |
|---|---|
| `valid` | false means `twai_get_status_info()` itself failed — usually the driver is **not installed** (mid-recovery, or `begin()` never ran) |
| `state` | `twai_state_t`: 0 stopped, 1 running, 2 bus-off, 3 recovering |
| `msgsToRx` | frames queued **in the driver**, not yet taken by `esp32_can`. Climbing while `Stats::rxFrames` stays flat means the controller receives and `esp32_can` does not deliver; **both** at zero means nothing is arriving at the peripheral at all |
| `txErr` / `rxErr` | the controller's TEC and REC. 128 is the bus-off threshold; ~128 on REC is error-passive |
| `busErr` | bus error count — form, stuff, CRC. The one that moves when the wire is wrong |

**A failing bus under forced recovery cycles, and one sample will mislead you.** The
counters reset on every reinstall, so the sequence looks like: all zero → `rxErr` 129 →
`busErr` climbing → `txErr` 128 → `state` 2 → `valid:false` → all zero → repeat. Sample
over ~40 s before concluding anything.

**To decide whether a fault is ours or external, stop transmitting and keep watching.**
With our own TX gate shut (`setTxEnabled(false)`), `txFrames` freezes and we emit nothing:

| With TX off | Verdict |
|---|---|
| `busErr` still climbing, `rxFrames` 0 | **external** — we are silent and cannot be the cause. Stop debugging firmware; suspect a stuck-dominant line, termination, or a bitrate mismatch |
| `busErr` frozen, `rxErr` 0, `rxFrames` 0 | the bus is genuinely **silent** — the other node is unpowered or not transmitting |
| `rxFrames` climbing | RX is fine; the problem is in our transmit path |

Measured on the bench 2026-07-28: **~1550 bus errors/second with our transmitter disabled**,
which settled a fault that had been mistaken for a firmware regression across several
reflashes.

---

## 4. `setListenOnlyMode` / `setNoACKMode` / `enable` / `disable`: confirmed, it is a reinstall

The claim is **confirmed**, verbatim:

```cpp
void ESP32CAN::setListenOnlyMode(bool state)                 // esp32_can_builtin.cpp:450
{
    disable();
    twai_general_cfg.mode = state ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL;
    enable();
}

void ESP32CAN::setNoACKMode(bool state)                      // :457
{
    disable();
    twai_general_cfg.mode = state ? TWAI_MODE_NO_ACK : TWAI_MODE_NORMAL;
    enable();
}
```

`disable()` (`:535-572`) does, in order: `twai_get_status_info()` — and **returns early
without doing anything if that fails** (`:566-569`), leaving `readyForTraffic` true;
`twai_stop()` if running (`:548`); `vTaskDelete()` on `task_CAN_handler` and
`task_LowLevelRX_handler` (`:552-559`); `twai_driver_uninstall()` (`:564`).

`enable()` (`:464-533`) does: `twai_driver_install()` (`:475`) — **returns early on
failure** (`:482`); creates a brand-new `task_CAN` and a brand-new `task_LowLevelRX`
(`:499`, `:503`/`:506`); `twai_start()` (`:520`) — returns early on failure (`:527`);
`readyForTraffic = true`.

So a single `setListenOnlyMode(false)` "just to be sure we are in normal mode" performs:
stop the peripheral mid-frame (`driver/twai.h:184-186`: "A message currently being
transmitted/received on the TWAI bus will be ceased immediately. This may lead to other
TWAI nodes interpreting the unfinished message as an error"), destroy both RX tasks,
uninstall the driver, reinstall it, create two new tasks, and start it. On a live bus,
with a panel mid-handshake. That is not a mode change; it is a reboot of the CAN
subsystem.

Four additional defects make it worse than merely disruptive:

1. **The task handles are never cleared.**
   ```cpp
        for (auto task : {task_CAN_handler, task_LowLevelRX_handler})   // :552
        {
            if (task != NULL)
            {
                vTaskDelete(task);
                task = NULL;                                            // :557  writes the loop COPY
            }
        }
   ```
   `task` is a copy of the element of a temporary `initializer_list`; assigning `NULL` to
   it does nothing to the members. They are normally overwritten by the next `enable()`
   (`xTaskCreate` writes them at `:499`/`:503`). But if `enable()` bails out early —
   install failed, or start failed — the members retain **dangling** handles, and the next
   `disable()` calls `vTaskDelete()` on freed TCBs. That is memory corruption, not a
   clean failure.
2. **A failed `enable()` leaves no tasks at all** and, if `twai_start()` was the step that
   failed, leaves `readyForTraffic` false with an installed-but-stopped driver.
3. **`disable()`'s early return path** (status read fails, e.g. driver not installed)
   skips the task deletion entirely, so a later `enable()` creates a *second* pair of
   tasks — two `task_LowLevelRX` racing on `twai_receive()` and two `task_CAN` racing on
   `callbackQueue`. 12 KB of stack leaked per occurrence and non-deterministic dispatch.
   `enable()` has no guard of its own: the watchdog's creation is protected by
   `if (!CAN_WatchDog_Builtin_handler)` (`:333`), the two CAN tasks are not (`:499`,
   `:503`).
4. **`disable()` deletes the task that may be calling it.** `vTaskDelete(task_CAN_handler)`
   (`:552-556`) is unconditional. Reached from the general callback — which runs *in*
   `task_CAN` (finding 1) — it destroys the running task mid-function, so control never
   returns to `disable()`: `twai_driver_uninstall()` at `:564` is never executed and
   `readyForTraffic` is never cleared at `:571`. The result is an installed, running driver
   with no consumer, a `readyForTraffic` flag that lies, and a `task_LowLevelRX` filling a
   `callbackQueue` nobody will ever drain again. Any API that transitively reaches
   `disable()` — both mode setters, `set_baudrate()`, `beginAutoSpeed()`,
   `forceDriverRestart()` — is therefore not merely discouraged inside the callback but
   structurally fatal there.

There is one benign instance of this machinery, and it is unavoidable: `init()` calls
`set_baudrate()`, which calls `disable()` then `enable()` (`:431-448`). At first boot the
`disable()` early-returns harmlessly because no driver is installed. This is why
`begin()` must be called **exactly once** — a second `begin()` is a live reinstall, and it
additionally re-runs `_init()`, which calls `xQueueCreate()` again (`:330-331`), leaking
the old queues while `task_CAN` is still blocked on the old `callbackQueue`. `_init()` also
clears all 32 filter slots back to `configured = false` (`:321-327`), so the second `begin()`
silently discards the software filter as well: frames keep arriving at the ISR and are then
thrown away by `processFrame()` for want of a match (`:638`), with no callback and no error
anywhere. Three independent breakages from one repeated call, none of them observable through
a return code.

**And `begin()` does not report its own failure.** `set_baudrate()` walks the
`valid_timings[]` table (`esp32_can_builtin.cpp:18-37`) and calls `enable()` only on a
match; an unmatched bitrate falls out of the loop and returns 0 having installed nothing
(`:436-447`). `init()` ignores that 0, sets `readyForTraffic = true` regardless (`:381`)
and returns the requested `ul_baudrate` (`:382`). So `CAN0.begin(499000)` returns 499000
while no driver is installed, neither `task_CAN` nor `task_LowLevelRX` exists, the general
callback is never invoked, and `sendFrame()` still returns `true` (finding 2) — a
completely dead bus that every API in the library calls a success. The supported set is
exactly 1M, 800k, 500k, 250k, 125k, 100k, 80k, 50k, 33333 and 25k, plus a 20k entry that
the table comments mark as ESP32-ECO2/S3-only (`:20-35`); 500 kbit/s is on it.
`getBusSpeed()` is no help either: `ESP32CAN` never assigns `busSpeed`, so it returns for
ever the 0 set by the `CAN_COMMON` constructor (`can_common.cpp:113`). The only truthful
post-`begin()` check is `twai_get_status_info(&s) == ESP_OK && s.state ==
TWAI_STATE_RUNNING`.

Finally, the mode used by `begin()` comes from the member initialiser and nowhere else:

```cpp
                                    //tx,         rx,           mode
  twai_general_config_t twai_general_cfg =
      TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_17, GPIO_NUM_16, TWAI_MODE_NORMAL);   // esp32_can_builtin.h:109
```

`twai_general_cfg.mode` is written **only** by `setListenOnlyMode()`/`setNoACKMode()`.
`begin()`, `init()`, `set_baudrate()` and `setCANPins()` never touch it. So after
`begin()` the controller is in `TWAI_MODE_NORMAL` by construction, and any call intended
to "confirm" that is pure damage. Runtime silent mode in AffaDisplay is a software gate
in `Esp32CanLink::send()` — a boolean that skips `CAN0.sendFrame()` — and nothing else.
Note that this is *not* equivalent to listen-only at the wire level: we still acknowledge
other nodes' frames. On a two-node bus that is exactly what we want, since the panel needs
an acknowledger or it retransmits forever.

---

## 5. `setCANPins()` and `TWAI_GENERAL_CONFIG_DEFAULT()`: opposite argument orders

This is the one that has already produced two wrong-direction wirings. Both orders are
real; they are simply not the same.

**`setCANPins()` is `(rx, tx)` — RX FIRST.**

```cpp
  void setCANPins(gpio_num_t rxPin, gpio_num_t txPin);        // esp32_can_builtin.h:93

void ESP32CAN::setCANPins(gpio_num_t rxPin, gpio_num_t txPin)  // esp32_can_builtin.cpp:132
{
    twai_general_cfg.rx_io = rxPin;
    twai_general_cfg.tx_io = txPin;
}
```

The constructor agrees: `ESP32CAN(gpio_num_t rxPin, gpio_num_t txPin, uint8_t busNumber)`
assigns `rx_io = rxPin; tx_io = txPin` (`esp32_can_builtin.cpp:39,44-45`), and the global
is built as `CAN0(GPIO_NUM_16, GPIO_NUM_17, 0)` under the comment `//rxpin txpin`
(`esp32_can.cpp:8-9`).

**`TWAI_GENERAL_CONFIG_DEFAULT()` is `(tx, rx, mode)` — TX FIRST.**

```c
#define TWAI_GENERAL_CONFIG_DEFAULT(tx_io_num, rx_io_num, op_mode) \
    {.mode = op_mode, .tx_io = tx_io_num, .rx_io = rx_io_num, \
     .clkout_io = TWAI_IO_UNUSED, .bus_off_io = TWAI_IO_UNUSED, \
     .tx_queue_len = 5, .rx_queue_len = 5, \
     .alerts_enabled = TWAI_ALERT_NONE, .clkout_divider = 0, \
     .intr_flags = ESP_INTR_FLAG_LEVEL1}
        // framework-arduinoespressif32/tools/sdk/esp32c3/include/driver/include/driver/twai.h:32-36
```

which is why `esp32_can_builtin.h:108-109` carries the comment `//tx, rx, mode` above its
use of the macro.

Summary, to be quoted rather than re-derived:

| Call | Order |
|---|---|
| `ESP32CAN::setCANPins(a, b)` | `a = RX`, `b = TX` |
| `ESP32CAN(a, b, bus)` ctor | `a = RX`, `b = TX` |
| `TWAI_GENERAL_CONFIG_DEFAULT(a, b, mode)` | `a = TX`, `b = RX` |
| `twai_general_config_t` field order in memory | `.mode, .tx_io, .rx_io, ...` |

For the record, `MegaOpen/platformio.ini:59` states `CAN0.setCANPins(tx, rx)` in a
comment. That comment is **wrong**; `MegaOpen/src/board/BoardProfile.h:24` and
`src/bus/TwaiBus.cpp:23-26` state it correctly. Do not carry the platformio.ini wording
forward.

This board (ESP32-C3 SuperMini, the panel rig): **RX = GPIO4, TX = GPIO3**. The MeganeCAN
board is the same module soldered mirrored: RX = GPIO3, TX = GPIO4. Two positional
`gpio_num_t` arguments that are interchangeable at the call site and differ only by
swapping is precisely the defect class the named `CanPins{ .rx =, .tx = }` struct exists
to eliminate: designated initialisers make the wrong order a compile error rather than a
silent dead bus.

---

## 6. `watchFor()` overloads and the resulting filter state

The overloads are declared at `can_common.h:220-223` and implemented at
`can_common.cpp:333-351`, plus `watchForRange()` at `:357`:

| Call | Effect |
|---|---|
| `watchFor()` | `setRXFilter(0, 0, false)` then `setRXFilter(0, 0, true)` — accept everything, standard and extended |
| `watchFor(id)` | one filter: `(id, 0x7FF, false)` for `id <= 0x7FF`, else `(id, 0x1FFFFFFF, true)` |
| `watchFor(id, mask)` | one filter, extended flag auto-chosen by `id > 0x7FF` |
| `watchFor(id, mask, ext)` | **does not exist.** Declared `can_common.h:223`, defined nowhere — `can_common.cpp` implements only the three above plus `watchForRange` (`:333, :340, :347, :357`). Calling it compiles and fails at link. See rule 20. |
| `watchForRange(id1, id2)` | computes an id/mask pair covering the range; iterates every id in between, so a wide range is slow |

**The library calls the no-arg form.** It consumes filter slots 0 and 1 of the 32
available (`_setFilter()` takes the first unconfigured slot, `esp32_can_builtin.cpp:301-315`;
`BI_NUM_FILTERS` = 32, `esp32_can_builtin.h:48`). Slot 0 = `{id 0, mask 0, extended false}`,
slot 1 = the same with `extended true`. Because `(msg.id & 0) == 0` for every id, both
match everything of their respective id width.

Three properties worth stating:

* **Hardware filtering is never used.** `twai_filters_cfg` is fixed at
  `TWAI_FILTER_CONFIG_ACCEPT_ALL()` at construction (`esp32_can_builtin.h:111`) and is
  never modified by any `watchFor`/`setRXFilter` path. All filtering is software, inside
  `processFrame()` (`:589-593`), after the frame has already cost an ISR, a queue and a
  task switch. Narrowing `watchFor()` therefore saves callback work, not bus load — and
  we do not narrow it, because both panel families plus their reply ids must pass, and a
  wrong filter is a silent no-frames failure that costs an hour to find.
* **Slot 0 collides with the per-mailbox callback array.** `processFrame()` prefers
  `cbCANFrame[i]` over `cbGeneral` (`:596-607`). If the application calls
  `CAN0.setCallback(0, ...)` or `attachCANInterrupt(0, ...)`, every standard frame is
  routed to that callback and our general callback stops being called. Documented as a
  prohibition for consumers of AffaDisplay.
* **Repeated `watchFor()` calls consume more slots** and never free them; there is no
  `clearFilters()`. They are also pointless: `processFrame()` returns on the **first**
  matching filter (`:600`, `:606`, `:635`), so slots 2..31 can never be reached once slots
  0 and 1 match everything. Call it once.

**Order matters, and only in one direction.** `readyForTraffic` is set true inside `init()`
(`:381`), i.e. by `begin()`, so `task_LowLevelRX` is already delivering frames to
`processFrame()` before the caller's next statement runs. With no filter configured, every
frame fails the loop and is discarded (`processFrame()` returns `false` at `:638`) — the
window is harmless. Configure the filter **last**, because it is the gate: with a filter
configured and `cbGeneral` still null, `processFrame()` falls through to
`xQueueSend(rx_queue, ...)` (`:632`) and quietly fills a 64-deep queue that, once a general
callback is registered, nothing will ever drain.

That is exactly the sequence MeganeCAN ran for months on this hardware
(`MeganeCAN/src/main.cpp:413-416`):

```cpp
    CAN0.setCANPins(GPIO_NUM_3, GPIO_NUM_4);   // that board: rx=3, tx=4. Ours is mirrored.
    CAN0.begin(CAN_BPS_500K);
    CAN0.setGeneralCallback(gotFrame);
    CAN0.watchFor();
```

It is the field-proven order and rule 2 fixes it verbatim.

---

## 7. Alerts: the driver does not read them, so ours is the only reader

`grep` over the whole driver source finds exactly two references to the alert API, both
`twai_reconfigure_alerts()` (`esp32_can_builtin.cpp:366,368`), and both inside
`if (debuggingMode)` in `init()` (`:359-378`). **`twai_read_alerts()` is never called
anywhere in `esp32_can` or `can_common`.**

Therefore:

* No alert is consumed behind our back, and an application that calls
  `twai_read_alerts()` steals nothing from the driver — because the driver would never
  have read it. Alerts are latched by the TWAI driver into a bitfield plus a semaphore and
  simply accumulate until read (`driver/twai.h:246-265`).
* Conversely, alerts are **disabled by default**: `TWAI_GENERAL_CONFIG_DEFAULT` sets
  `.alerts_enabled = TWAI_ALERT_NONE` (`twai.h:35`) and the driver only overrides that in
  debugging mode. So an application that wants alerts must call
  `twai_reconfigure_alerts()` itself. That call is safe on a running driver and changes no
  operating state.
* AffaDisplay will not read or configure alerts. Everything it needs — state,
  `tx_failed_count`, `rx_missed_count`, error counters — is available from
  `twai_get_status_info()`, which is a snapshot read with no side effects and no
  consumption semantics, so it cannot conflict with an application that *does* use alerts.
  This keeps `Esp32CanLink` compatible with a host application that owns the alert register —
  and one already does: MegaOpen calls `twai_reconfigure_alerts(TWAI_ALERT_ALL, nullptr)`
  straight after `begin()` (`MegaOpen/src/main.cpp:576`) to feed its `/api/can` diagnostics.
  Had the library taken the alert register for itself, that page would have gone blind the
  day the display code moved into it. This rule is not hypothetical tidiness.
* Do **not** call `CAN0.setDebuggingMode(true)` in production: besides the `Serial.write()`
  calls sprinkled through `sendFrame()` and `processFrame()` (`:634`, `:665-677`), it
  enables `TWAI_ALERT_AND_LOG` (`:362`), which makes the driver log from interrupt
  context.

---

## 8. Calling `CAN0.sendFrame()` from inside the general callback

Short answer: it **cannot deadlock**, but it is **not safe enough to build on**, and
AffaDisplay must not do it.

What is genuinely fine:

* `twai_transmit()` is thread-safe and has no dependency on `task_CAN`. The TX path is
  ISR-driven and completely independent of the RX callback path, so a send issued from
  `task_CAN` can complete while `task_CAN` is blocked in it. There is no lock inversion.
* `processFrame()` posts to `callbackQueue` with a zero timeout (`:599`), so a slow
  `task_CAN` can never block `task_LowLevelRX` — it can only cause frames to be dropped.
  No stall propagates back to the driver.

What makes it unacceptable anyway:

1. **It can block for 4 ms in the one task that must keep draining.** `sendFrame()` waits
   up to `pdMS_TO_TICKS(4)` for TX queue space (`:660`). While `task_CAN` is blocked, the
   16-deep `callbackQueue` continues to fill and then **silently discards** frames
   (finding 1). At 500 kbit/s back-to-back traffic overruns those 16 slots in 3.6 ms of
   8-byte frames, or 1.5 ms of short ones — **both inside the 4 ms blocking window**. The
   AFFA panel sends DLC-8 frames throughout, so 3.6 ms is the figure that applies; a
   retransmit storm from an unacknowledged node (the ~2000 frames/s seen during MegaOpen's
   bring-up, `MegaOpen/src/main.cpp:536-537`) closes that gap further. The failure is
   invisible:
   a lost frame, no counter, and — since our ACK state machine has a 2000 ms deadline — it
   surfaces much later as a spurious timeout. This is the same shape as the defect being
   removed from the legacy code (a blocking wait inside the only path that could satisfy
   it), and it would reintroduce it one layer down.
2. **`task_CAN` can be deleted underneath us, precisely while we are blocked in
   `sendFrame()`.** If force-recovery is enabled, `ForceRecoveryTask` (priority 10) calls
   `disable()`, which calls `vTaskDelete(task_CAN_handler)` (`:552-556`). On the unicore
   C3, a priority-10 task cannot preempt priority 15 — it only runs when `task_CAN`
   blocks, and blocking in `twai_transmit()` is exactly that. So the single most likely
   moment for `task_CAN` to be destroyed mid-callback is while our callback is waiting on
   a full TX queue. Any callback that holds a mutex, owns a half-written buffer or is
   partway through a multi-frame sequence dies in that state, unrecoverably.
3. Reentrancy: our own transmitted frames are not looped back (`__TX_frame.self = 0`,
   `:649`), so there is no self-feeding loop — but that is the only reason there isn't one.

**What AffaDisplay does instead.** The general callback does exactly one thing: copy the
frame into a lock-free single-producer/single-consumer ring and return. No send, no log,
no allocation, no user code, no `millis()`. Every transmission — including the automatic
`5C1 74 00...` DONE acknowledgement to the panel's `1C1 70 ...` registration request —
is issued from `poll()`, on the caller's task, after the frame has been drained from the
ring. Because `poll()` is the only writer to the TX path and it never waits for an
answer, the ACK is emitted on the very next `poll()` and the round trip is bounded by the
caller's poll period, not by any blocking wait. The observed panel handshake tolerates
this comfortably: its sync request repeats at ~1 Hz and its `1C1` registration request is
retried until answered.

Point 2 above is also why the ring must survive its producer being killed at an arbitrary
instruction: a single-producer ring that publishes by writing the payload first and the
head index last loses at most the in-flight frame if `task_CAN` disappears, and never
corrupts the buffer. No mutex, no critical section, no `xQueueSend` from the callback.

---

## 9. Stack budget inside the general callback

The callback runs on `task_CAN`'s stack: **8192 bytes**, `xTaskCreate(ESP32CAN::task_CAN,
canHandlerTaskName, 8192, this, 15, &task_CAN_handler)` (`esp32_can_builtin.cpp:499`).
Consumed before we are called: `task_CAN`'s own frame (a `CAN_FRAME rxFrame`, 24 bytes,
plus the `xQueueReceive` call) and `sendCallback()`'s frame (three locals). Call it
under 200 bytes. Nominally ~7.9 KB is free.

Two corrections to that headline number:

* On the ESP32-C3 (RISC-V) interrupts are serviced on a dedicated interrupt stack —
  `CONFIG_FREERTOS_ISR_STACKSIZE=2096` (`tools/sdk/esp32c3/sdkconfig:1218`) — so ISR
  nesting does not eat into `task_CAN`'s stack. On Xtensa parts (classic ESP32, S3) that
  is not true of all interrupt levels, and the usual ESP-IDF headroom rules apply. The
  library must be portable to both, so budget as if the interrupt reserve came out of the
  task stack.
* 8192 is the *driver's* choice, not ours. We cannot change it (`enable()` hard-codes it),
  we do not own the task, and nothing stops a future version of the driver from lowering
  it. A library that quietly depends on 8 KB of someone else's stack is a library that
  breaks on a dependency bump.

Therefore the hard ceiling AffaDisplay imposes on itself is far below the nominal figure:
**the general callback uses no more than 128 bytes of stack** — one `Frame` copy (16
bytes: id, len, 8 data bytes, flags) plus index arithmetic, no callees other than the
inlined ring push. Explicitly excluded, each for a measurable reason:

| Forbidden in the callback | Why |
|---|---|
| `Serial.print` / `printf` / `ESP_LOG*` | `vfprintf` with any format costs hundreds of bytes to well over 1 KB, and takes a lock |
| `String`, `new`, `malloc` | heap lock, unbounded time, and the "zero allocation after `begin()`" rule |
| Recursion, or any call into user callbacks | unbounded depth and unbounded time in a task we do not own |
| `CAN0.sendFrame()` | finding 8 |
| `millis()` / `micros()` | not a stack issue, but the timebase belongs to `IClock`, sampled in `poll()` |
| Any mutex, semaphore or critical section | the task can be deleted while holding it (finding 8, point 2) |

---

## 10. What including `esp32_can.h` drags in, and four symbols that do not exist

`Esp32CanLink.cpp` is the single translation unit permitted to include this header
(rule 1). That privilege comes with three traps that only that file can hit.

**Four declared symbols have no definition anywhere in the library.**

```cpp
extern volatile uint32_t biIntsCounter;             // esp32_can.h:10
extern volatile uint32_t biReadFrames;              // esp32_can.h:11
extern QueueHandle_t callbackQueue;                 // esp32_can_builtin.h:133
int watchFor(uint32_t id, uint32_t mask, bool ext); // can_common.h:223
```

`grep` over both `src/` trees finds no definition of any of them. The first three are
leftovers from the pre-TWAI interrupt-driven implementation; the fourth is an overload that
was declared and never written (finding 6). Naming any one of them compiles cleanly and
fails at link with an undefined reference — which, in a PlatformIO build, surfaces as a
linker error in *our* file about a symbol we never wrote. `callbackQueue` is the worse one:
the real queue is the **member** `ESP32CAN::callbackQueue` (`esp32_can_builtin.h:113`), and
the file-scope `extern` of the same name sits in the global namespace waiting to shadow it
in any diagnostic code that reaches for "the callback queue". There is no supported way to
inspect that queue's depth from outside the class; do not try.

**The header pulls a large and unrelated dependency set into whatever includes it:**
`Arduino.h`, `driver/adc.h`, `esp_adc_cal.h`, `driver/twai.h`, `freertos/*` and `<sstream>`
(`esp32_can_builtin.h:34-45`). The `<sstream>` include exists solely for the
`std::ostringstream` task-name construction in the dead IDF ≥ 5.2 branch (`:335-337`,
`:487-492`); at IDF 4.4.7 it is pure parse cost, but it is a `std::` streams header in an
embedded build and it is one dependency bump away from being instantiated. The ADC headers
are vestigial. This is a second, independent reason for the `AFFA_ENABLE_ESP32CAN_LINK`
gate and for the whole-body `#if` idiom: a consumer on a different CAN driver must not pay
for this include graph, and with the gate at 0 the translation unit is empty, so it does
not.

**`CAN_FRAME` cannot cross the library boundary.** It is a class with a constructor
(`can_common.h:135-149`), 24 bytes, and it lives in the global namespace. `affa::Frame`
must be a distinct type and the conversion must happen inside `Esp32CanLink.cpp` in both
directions. Reusing `CAN_FRAME` in a public signature would drag `can_common.h` into the
umbrella header and make rule 1 unenforceable.

---

## Rules for AffaDisplay

1. **`Esp32CanLink.cpp` is the only translation unit in the library that may
   `#include <esp32_can.h>`.** Nothing else may name `CAN0`, `CAN_FRAME`, `twai_*` or any
   ESP-IDF type. (Whole document; enforced by review, and by `AFFA_ENABLE_ESP32CAN_LINK`
   so a project on another driver never pulls the header in at all.)
2. **`begin()` runs exactly this sequence, exactly once, and the driver is never touched
   again:** `CAN0.setCANPins(pins.rx, pins.tx)` -> `CAN0.begin(bitrate)` ->
   `CAN0.setGeneralCallback(&trampoline)` -> `CAN0.watchFor()`. `watchFor()` is last
   because it is the gate that opens the software filter; the callback must be in place
   before it. This is verbatim the sequence MeganeCAN ran for months
   (`MeganeCAN/src/main.cpp:413-416`). A second `begin()` is a live driver reinstall plus a
   queue leak. (Findings 4, 6.)
3. **Pins are passed as `CanPins{ .rx =, .tx = }` and forwarded to `setCANPins(rx, tx)`,
   RX first.** Never accept two bare `gpio_num_t` positionals. `TWAI_GENERAL_CONFIG_DEFAULT`
   is `(tx, rx, mode)` and must never be confused with it. This board: `.rx = GPIO_NUM_4,
   .tx = GPIO_NUM_3`. (Finding 5.)
4. **Forbidden after `begin()`, without exception:** `setListenOnlyMode()`,
   `setNoACKMode()`, `enable()`, `disable()`, `set_baudrate()`, `beginAutoSpeed()`,
   `forceDriverRestart()`, `setDebuggingMode(true)`, `setCallback()`/
   `attachCANInterrupt()` on mailbox 0 or 1, any further `watchFor()`, and every direct
   `twai_*` call that mutates state (`twai_start`, `twai_stop`, `twai_driver_install`,
   `twai_driver_uninstall`, `twai_initiate_recovery`, `twai_clear_transmit_queue`,
   `twai_reconfigure_alerts`). Each of the first four is a driver reinstall on a live bus,
   with a dangling-handle path if it fails; and every one of them reaches `disable()`,
   which `vTaskDelete()`s `task_CAN` — so from inside the general callback any of them
   deletes its own caller and leaves the driver installed with no consumer.
   (Findings 3, 4, 6, 7.)
5. **No competing bus-off recovery.** The driver's watchdog owns bus-off. `Esp32CanLink`
   only *reads* `twai_get_status_info()` — a side-effect-free snapshot — and reports.
   `isLive()` returns `state == TWAI_STATE_RUNNING`; `AffaDisplayBase` treats `false` as
   loss of sync and stops enqueueing rather than pushing frames into a stopped controller.
   Automatic restoration, if wanted, is the application calling
   `CAN0.setForceRecovery(true)` **before** `begin()`, documented with its 2-second
   outage. (Finding 3.)
6. **The general callback does one thing: copy the frame into the RX ring and return.**
   No send, no log, no allocation, no lock, no clock read, no user callback, no recursion;
   ≤128 bytes of stack. The `CAN_FRAME*` is a pointer to `task_CAN`'s stack and is invalid
   the moment we return, so the copy is mandatory. (Findings 1, 8, 9.)
7. **The RX ring is a genuine lock-free SPSC ring, publish-last.** Producer is `task_CAN`
   at priority 15, unpinned (so possibly a different core from the consumer); consumer is
   the caller's task at priority 1. Indices are `std::atomic<uint16_t>` with
   release/acquire, payload written before the head store. It must remain consistent if
   the producer task is deleted mid-push — losing at most the in-flight frame. (Findings
   1, 4, 8.)
8. **Every frame taken off the ring is stamped by `IClock::millis()` on the consumer
   side.** `CAN_FRAME::timestamp` is always 0; the driver provides no arrival time.
   (Finding 1.)
9. **Sanitise on ingress:** clamp `length` to 8 (the driver copies `data_length_code`
   unchecked), drop `rtr` frames, drop `extended` frames. AFFA is 11-bit data frames only.
   (Finding 1.)
10. **Treat `sendFrame()` as "queued, unknown outcome".** Its `bool` return is a literal
    `true` on every path and carries no information; do not test it, do not derive
    `txDropped` from it. Delivery evidence comes from the panel's ACK on `funcId|0x400`
    and from `twai_get_status_info()` (`msgs_to_tx`, `tx_failed_count`, `state`).
    (Finding 2.)
11. **`ICanLink::send()` is documented as bounded, not instantaneous:** worst case ~4 ms,
    only when 16 frames are already queued. `Esp32CanLink` counts `txFrames`, and counts a
    TX backlog (`msgs_to_tx` not draining across successive polls) as the observable
    substitute for the return code the driver refuses to give. (Finding 2.)
12. **All transmission happens in `poll()`, on the caller's task — including the automatic
    `5C1 74 00 00 00 00 00 00` acknowledgement.** Never from the general callback.
    (Finding 8.)
13. **Count our own losses and expose the driver's.** `ringOverflow` (ours) plus
    `rx_missed_count` / `rx_overrun_count` / `rx_error_counter` / `tx_error_counter`
    (driver, read-only). Without both, a dropped frame is indistinguishable from a panel
    that never answered — and the 16-deep `callbackQueue` drops silently. (Finding 1.)
14. **Never read the TWAI alert register, and never enable alerts.** The driver never
    consumes them either, so the application is free to own them; the library staying out
    keeps that true. `twai_get_status_info()` supplies everything the library needs.
    (Finding 7.)
15. **"Silent mode" is a software TX gate in `Esp32CanLink::send()`** — a boolean that
    skips `CAN0.sendFrame()`. Never a driver mode change. Document that this still
    acknowledges other nodes, which on a two-node bus is required: without our ACK the
    panel retransmits every frame forever. (Finding 4.)
16. **`setRXBufferSize(1)` before `begin()` is permitted** to reclaim ~1.5 KB, since
    `rx_queue` is never written once a general callback is registered; if it is used, the
    library must never call `available()`, `rx_avail()` or `get_rx_buff()`. This is the
    only pre-`begin()` knob allowed beyond pins and bitrate. (Finding 1.)
17. **Consumers are told, in the README:** do not register a per-mailbox callback on
    mailbox 0 or 1, do not call `watchFor()` again, and do not call any mode setter. Those
    three are the ways an application can break the link from outside the library.
    (Findings 4, 6.)
18. **`begin()`'s return value is not a health check.** Pass only a bitrate present in
    `valid_timings[]` — 500 000 for this bus — and verify afterwards with
    `twai_get_status_info(&s) == ESP_OK && s.state == TWAI_STATE_RUNNING`.
    `Esp32CanLink::begin()` returns `false` when that check fails, which is the only way an
    application can learn that the driver was never installed. Never call `getBusSpeed()`;
    it always returns 0. (Finding 4.)
19. **Expect and tolerate RX blackouts during flash writes.** The TWAI ISR is not in IRAM,
    so an OTA image write, an NVS commit or a filesystem write stops reception outright for
    its duration. The library must not read a burst of missed frames as a protocol error:
    the ISO-TP transmit machine re-times from its own deadlines, and only the peer deadline
    may declare the link lost. `AFFA_PEER_TIMEOUT_MS` must therefore never be lowered below
    the longest flash write the application performs — measure it with
    `examples/90_bench_ota` before touching that macro. (Finding 1.)
20. **Never name `biIntsCounter`, `biReadFrames`, the file-scope `callbackQueue`, or
    `watchFor(id, mask, bool)`.** All four are declared by the driver headers and defined
    nowhere; referencing one is a link error in our file about a symbol we did not write.
    `CAN_FRAME` stays inside
    `Esp32CanLink.cpp` — `affa::Frame` is a separate type and the conversion is that file's
    job in both directions, which is what keeps rule 1 enforceable. (Finding 10.)
21. **Re-verify this document on any dependency bump.** Every finding is tied to
    `esp32_can` commit `c329e6b`, `can_common` `0.4.0` and ESP-IDF 4.4.7. That pair is
    pinned in `library.json`, `platformio.ini`, `platformio_footprint.ini` and the
    README's installation snippet; changing any one of those four without re-reading this
    document invalidates it silently, which is the whole reason the pins exist. In
    particular the IDF ≥ 5.2 branches
    (handle-based `_v2` API, per-controller task names built from a dangling
    `std::ostringstream::str().c_str()` at `:337,491-492`, second controller) are dead
    code today and would all become live at once.
