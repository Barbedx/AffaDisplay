# can-link Specification

## Purpose

`ICanLink` is the library's only seam to a CAN controller. `Esp32CanLink` is the only
class in AffaDisplay that knows a driver exists; everything above it is portable and is
compiled for the host by the `native` environment.

The capability covers bring-up, the pull-based receive path, the software transmit gate,
recovery from bus-off, and the counters an operator reads before deciding whose fault a
silent bus is.

## Requirements

### Requirement: Driver Bring-Up Is A Fixed, Verified Sequence

`begin()` SHALL run one sequence — `setCANPins(rx, tx)`, `begin(bitrate)`,
`setGeneralCallback()`, `watchFor()` LAST — and SHALL verify the outcome by reading
`twai_get_status_info()` rather than by trusting a return value.

`CAN0.begin()` returns the *requested* bitrate even for a rate absent from
`valid_timings[]`, having installed nothing, so its return value is not a health check.

#### Scenario: Controller reaches RUNNING

- **WHEN** `begin(pins, 500000, 250)` is called on a healthy board
- **THEN** the controller state reads `TWAI_STATE_RUNNING`
- **AND** `begin()` returns true

#### Scenario: Unsupported bitrate installs nothing

- **WHEN** `begin()` is called with a rate absent from the driver's `valid_timings[]`
- **THEN** the status read does not report RUNNING
- **AND** `begin()` returns false rather than reporting the requested rate as success

#### Scenario: Pin order is RX first

- **WHEN** the caller constructs `CanPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 }`
- **THEN** the named fields make the order explicit at the call site
- **AND** the driver is configured RX-first, matching `setCANPins(rxPin, txPin)`

#### Scenario: A second begin is refused

- **WHEN** `begin()` is called on a link that already came up
- **THEN** it returns false and does not reinstall the driver on a live bus

### Requirement: The Driver Is Not Touched After Bring-Up

After `begin()` returns, the link SHALL make no driver call other than `send()`,
`twai_get_status_info()`, and the single permitted `recover()` path.

Every runtime mode setter in `esp32_can` is `disable()` + `enable()` underneath — a driver
reinstall on a live bus which, called from inside the general callback, deletes its own
caller.

#### Scenario: Listen-only is refused, not faked

- **WHEN** `begin(pins, bitrate, forceRecoveryMs, LinkMode::ListenOnly)` is called
- **THEN** the call logs an error and returns false
- **AND** no link is brought up in a degraded mode

Rationale: `setListenOnlyMode()` before `begin()` starts tasks that block on queues only
`_init()` creates — a board dead before WiFi, i.e. before OTA. After `begin()` it is the
forbidden live reinstall. The header MUST document the refusal and point at the software
TX gate, because a caller who follows a header promising a working diagnostic gets no link
at all.

### Requirement: Bus-Off Recovery Is Armed Before Bring-Up

`begin()` SHALL accept a `forceRecoveryMs` and, when non-zero, arm
`setForceRecovery(true, ms)` BEFORE the driver is installed.

#### Scenario: Zero leaves the controller stopped

- **WHEN** `forceRecoveryMs` is 0 and the controller goes bus-off
- **THEN** it ends in `TWAI_STATE_STOPPED` and nothing restarts it
- **AND** the symptom is `rxFrames` 0 with counters frozen — indistinguishable by eye from
  a dead transceiver

#### Scenario: Recovery delay must not accumulate inside setup

- **WHEN** `forceRecoveryMs` is 2000 on a bus that is failing continuously
- **THEN** each cycle costs a full disable/delay/enable and they accumulate inside
  `setup()`
- **AND** the board's first HTTP response is minutes late — measured at 185 999 ms

#### Scenario: The value that works

- **WHEN** `forceRecoveryMs` is 250
- **THEN** a panel that wakes a second late is still caught
- **AND** a genuinely dead bus costs seconds of bring-up rather than minutes

### Requirement: Receive Is Pull-Based Through A Ring

The driver callback SHALL copy the frame into a fixed ring and return. It SHALL NOT log,
allocate, block, read a clock, take a lock, transmit, or call user code.

It runs in `task_CAN` at priority 15, on the critical path of every frame, behind a
callback queue only 16 entries deep which overruns in 1.5–3.6 ms of back-to-back traffic
at 500 kbit/s and drops silently.

#### Scenario: Frames are drained by the poll owner

- **WHEN** the poll task calls `recv(out)`
- **THEN** it pops one frame from the ring
- **AND** returns false when the ring is empty

#### Scenario: Ring overflow is counted, not hidden

- **WHEN** frames arrive faster than the poll task drains them
- **THEN** the overflow is counted in `Stats` and surfaced as a link error

### Requirement: The Transmit Gate Is Software Only

`setTxEnabled(false)` SHALL cause `send()` to refuse and count a drop. It SHALL NOT change
the driver mode.

The controller therefore continues to acknowledge other nodes in hardware, which a
two-node bus requires, and which is what makes the gate a valid diagnostic rather than a
way to leave the bus.

#### Scenario: The gate refuses our frames

- **WHEN** the gate is shut and the library transmits
- **THEN** `send()` returns false, `txDropped` increments, and `txFrames` does not move

#### Scenario: The gate answers whose fault a noisy bus is

- **WHEN** the gate is shut and `busErr` keeps climbing while `rxFrames` stays 0
- **THEN** the fault is not ours — we are emitting nothing
- **AND** measured on the bench rig 2026-07-28, three separate boots: ~1504 bus errors per
  second with the transmitter gated shut

### Requirement: Send Never Infers Delivery From The Driver

`send()` SHALL NOT test the return value of `sendFrame()`.

`ESP32CAN::sendFrame()` is a literal `return true` on every path, including
timeout-and-drop, driver-not-installed, and listen-only-refuses. Deriving `txDropped` from
it produces a link that reports success while transmitting nothing.

#### Scenario: Delivery evidence comes from the panel

- **WHEN** a message is sent
- **THEN** the only accepted evidence of delivery is the panel's acknowledgment on
  `funcId | 0x400`, plus the controller counters read from `stats()`

### Requirement: Health Is Counted, Never Sampled

The link SHALL expose cumulative counters — recoveries, failures, flaps, and total downtime
— rather than requiring callers to infer health from an instantaneous state read.

With force-recovery armed, a continuously failing bus cycles through
`counters 0 → rxErr 129 → busErr climbing → txErr 128 → bus-off → reinstall → counters 0`
roughly every four seconds. Any single sample lands somewhere in that cycle and is equally
consistent with a healthy link, a dead one, and a flapping one.

#### Scenario: A single sample is not the truth

- **WHEN** an operator reads `driverState()` once and sees `txErr` 0 with the controller
  RUNNING
- **THEN** that reading alone does NOT establish that transmissions are being acknowledged
- **AND** the same link sampled 1.5 s later may show `state` 2 with `txErr` 128

Established 2026-07-28 by sampling every 1.5 s for 20 s. An earlier diagnosis that read
`txErr = 0` from one sample and concluded "the panel is alive, only our receive side is
broken" was wrong for exactly this reason.

#### Scenario: Flaps and downtime distinguish the three cases

- **WHEN** the controller leaves the live state and returns
- **THEN** `flaps` increments and the outage duration is added to `downMs`
- **AND** a caller can distinguish "healthy", "down once", and "flapping 300 times" without
  sampling luck

### Requirement: Recovery Runs On The Poll Task Only

`recover()` SHALL restart the driver in place, SHALL refuse while the controller is
RECOVERING, and SHALL return whether the controller is RUNNING afterwards — never merely
that a call was made.

It blocks for the restart's settle delay, so it MUST NOT be called from `task_CAN`.

#### Scenario: Refused mid-recovery

- **WHEN** `recover()` is called while the controller state is RECOVERING
- **THEN** it declines and returns false, leaving the caller's backoff to try again
- **AND** no half-torn-down driver is produced

### Requirement: The Library Never Reboots The Board

Link recovery SHALL back off and cap. It SHALL NOT give up permanently, and it SHALL NOT
restart the processor.

A reboot is application policy. On a bus fault that is not ours it fixes nothing, churns
flash, loses the diagnostic log, and makes the console the operator is watching flap too.

#### Scenario: Backoff caps instead of escalating

- **WHEN** recovery attempts keep failing
- **THEN** the retry interval doubles up to a cap and keeps retrying at that cap
- **AND** the link continues to report its counters throughout
