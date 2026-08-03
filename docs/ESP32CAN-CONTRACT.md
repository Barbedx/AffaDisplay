# Direct ESP-IDF TWAI contract

> This historical filename is retained for existing links. It documents AffaDisplay's
> direct ESP-IDF TWAI transport, not `collin80/esp32_can` or `can_common`.

`Esp32CanLink` is the Arduino/ESP32 implementation of the library's non-blocking
`ICanLink` seam. It uses the ESP32 Arduino core's built-in `<driver/twai.h>` driver and
has no third-party CAN-library dependency.

## Ownership

One `Esp32CanLink` owns one process-global TWAI controller, its lifecycle mutex, and one
RX task. A second link cannot take that controller while the first one owns it.

Application code must not call `twai_driver_install`, `twai_start`, `twai_stop`,
`twai_driver_uninstall`, `twai_initiate_recovery`, `twai_transmit`, or direct mode changes
for the controller used by `Esp32CanLink`. Competing lifecycle calls can race a controlled
recovery or delete the controller under the RX task.

Use the link surface instead:

```cpp
link.setTxEnabled(false);       // quiet period; does not reconfigure TWAI
link.setTxEnabled(true);
link.setListenOnly(true);       // controlled restart into listen-only mode
link.setListenOnly(false);      // controlled restart back to normal mode
```

`setListenOnly()` may briefly take the controller down while it replaces the driver. It is
not a substitute for a passive electrical tap on a live vehicle bus.

## Pins, bitrate, and startup

The public pin type follows signal direction:

```cpp
CanPins pins{GPIO_NUM_3, GPIO_NUM_4};  // { rx, tx }
link.begin(pins, 500000);
```

ESP-IDF's `TWAI_GENERAL_CONFIG_DEFAULT` macro takes **TX then RX**. `Esp32CanLink` performs
that inversion in one place, so callers must continue to pass `{rx, tx}`.

`begin()` accepts the driver's standard timing presets (25 kbit/s through 1 Mbit/s,
including 500 kbit/s), installs an accept-all filter, starts TWAI, and starts the bounded
RX task. It returns false for an unsupported bitrate, a driver/start failure, a task
creation failure, or a second owner. `forceRecoveryMs` remains a source-compatible argument;
the library's poll/backoff recovery owns the actual recovery policy.

## RX contract

The RX task reads `twai_receive()` and copies only 11-bit data frames into the bounded
AffaDisplay RX ring. It drops RTR and extended frames because AFFA traffic uses standard
data frames. It calls no application callbacks and never advances the protocol state.

`AffaDisplayBase::poll()` is the sole consumer through `ICanLink::recv()`. Keep calling
`poll()` (or enable the library-owned poll task) so the protocol can drain the ring,
recognise the panel's session request, perform function registration, and process `0x74`
replies.

`stats()` reports the transport-facing counters; `driverState()` exposes an explicit
best-effort TWAI status snapshot for diagnostics. A missing snapshot during reconfiguration
is expected, not an application error.

## TX contract

`trySend()` makes a zero-timeout `twai_transmit()` offer:

| Result | Meaning |
| --- | --- |
| `Accepted` | TWAI took the frame into its local TX queue. |
| `Busy` | A momentary local resource shortage, such as a full TX queue or lifecycle mutex. The protocol leaves its bytes pending and retries later. |
| `Rejected` | Transmission cannot proceed now: not started, TX gated, listen-only, reconfiguration, invalid frame, or non-transient driver failure. |

`send()` is the compatibility boolean form of `trySend() == Accepted`. Neither result proves
that the panel displayed a message. For AFFA transfers, the expected `0x74` protocol reply
is the delivery proof.

The direct transport never waits for TX queue space. That is why a request storm cannot
block the application loop merely because the controller queue is full; pacing and retries
remain in the protocol state machine.

## Health, quiet periods, and recovery

`isLive()` answers “may the protocol transmit now?” It is false while TX is gated, in
listen-only mode, during reconfiguration, or when the controller is not running.

`healthy()` answers “does a usable controller exist?” It remains true for deliberate TX
gating and listen-only mode so those application choices do not trigger recovery.

The library calls `ICanLink::recover()` only from its poll/recovery path. A recovery can
briefly block there: a full direct-TWAI restart first waits for the RX task to exit, then
stops/uninstalls/reinstalls/starts the driver and RX task under the lifecycle mutex. The
restart is deliberately rate-limited by `AFFA_LINK_RECOVER_MS`,
`AFFA_LINK_RECOVER_MAX_MS`, and the RX-stall policy.

For bus-off, the link initiates the ESP-IDF recovery procedure and waits for a later poll to
observe the legal stopped state before starting again. `recover(true)` permits a controlled
full restart when the library has evidence of a running-but-deaf link. Do not add a second
application-level TWAI watchdog.

## Operational rules

1. Construct and `begin()` one `Esp32CanLink` for the controller.
2. Pass pins as `{rx, tx}`; for the bench C3 wiring this is `{GPIO_NUM_3, GPIO_NUM_4}`
   (`CRX/RXD -> GPIO3`, `CTX/TXD -> GPIO4`).
3. Call `poll()` regularly, or use `AFFA_ENABLE_TASK=1`.
4. Let the display initiate the Carminat/AFFA3 NAV session. The protocol waits for the
   display authorization request before sending its registration/hello sequence.
5. Treat a direct TX accept as queue admission only; wait for protocol completion (`0x74`).
6. Use `setTxEnabled()` for OTA or other quiet periods. Do not stop/uninstall TWAI directly.
7. When investigating a silent bus, record `stats()` and `driverState()` before changing
   configuration; the direct transport is intentionally observable without a wrapper.

## Validation surface

The normal library and examples `ex01_bringup`, `ex03_hello`, `ex04_rows`, and
`ex06_authclock` build without `ESP32_CAN` or `can_common`. `ex02_canspy` and
`ex05_pingpong` deliberately retain the legacy wrapper as comparison diagnostics; they are
not dependencies of the library or normal examples.
