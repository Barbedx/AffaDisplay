# owned-task Specification

## Purpose

`AffaTask` (`src/rtos/`, gated on `AFFA_ENABLE_TASK`) lets the library own its own poll
task, so the threading contract cannot be broken by addition. It is the only part of the
library that knows FreeRTOS exists.

## Requirements

### Requirement: Exactly One Task Polls

`poll()` SHALL be a no-op when called by a task that is not the registered owner, and each
refusal SHALL be counted.

Two tasks pumping one instance corrupts the transmit FSM, and it does it silently.

#### Scenario: Off-task polling is refused and counted

- **WHEN** a task other than the owner calls `poll()`
- **THEN** nothing is pumped and `foreignPolls()` increments

#### Scenario: Caller-owned mode is unchecked

- **WHEN** no owner is registered
- **THEN** the check is disabled entirely and the caller is trusted to poll from one task

### Requirement: Poll Order Is RX Before TX, Always

`poll()` SHALL run link recovery first, then receive, then sync, then transmit, then the
panel's own `onPoll()`.

Receive strictly precedes transmit so that key latency is bounded by the poll period alone
and not by queue depth or a message in flight. Link recovery precedes receive because a
controller that is down delivers no frames, so receive loses nothing by being second.

#### Scenario: Key latency is independent of queue depth

- **WHEN** a key frame arrives while a multi-frame render is in flight
- **THEN** the key callback fires on that same `poll()`
- **AND** it does not wait for the transfer to finish

### Requirement: Nothing Counts Poll Calls

Every time-dependent behaviour SHALL be driven by deadlines against the injected clock.

#### Scenario: Poll rate does not change protocol timing

- **WHEN** the poll period changes from 2 ms to 20 ms
- **THEN** heartbeat interval, acknowledgment timeout and peer timeout are unchanged

### Requirement: Renders May Be Called From Any Task

`AffaTask` SHALL accept render calls from any task, copy them into a command queue, and
return a `TxRequest` handle without blocking. A refusal SHALL return `kNoRequest`, increment
`queueDropped`, and be logged — never silently dropped.

#### Scenario: A web handler renders directly

- **WHEN** an HTTP handler calls `task.setText("HELLO")` from the server's task
- **THEN** the call returns a handle immediately without a mailbox, mutex or hand-off

#### Scenario: A call from the owned task takes the direct path

- **WHEN** a render is issued from inside a `KeyCb`, `SyncCb` or `CompleteCb`
- **THEN** it is applied directly instead of being queued

Rationale: this is not an optimisation. Self-enqueueing from a callback would deadlock
against a full queue, and the documented
`poll() → KeyCb → abortPending() → CompleteCb` pattern is exactly that shape.

### Requirement: Callbacks Still Fire Synchronously Inside Poll

Key delivery SHALL remain synchronous inside `poll()`, before any transmit pumping. It
SHALL NOT be routed back to an application task through a queue.

This is the acceptance criterion the owned-task design lives or dies by: what changes is
only WHICH task the callback runs on.

#### Scenario: Key latency is unchanged by adopting the owned task

- **WHEN** the same application is built with and without `AFFA_ENABLE_TASK`
- **THEN** the measured key-to-callback latency is bounded by the poll period in both cases

### Requirement: The Cost Of Blocking Is Made Visible

The task SHALL publish the worst iteration duration seen AND the timestamp at which it
occurred.

A callback that blocks blocks the library. That cannot be prevented, so it is measured.

#### Scenario: A peak at boot is distinguishable from a bad callback

- **WHEN** the worst iteration is recorded at t=0
- **THEN** it is attributable to WiFi associating
- **AND** a peak whose timestamp keeps moving indicates a callback that blocks — without the
  timestamp the two are identical

### Requirement: Status Is Published As A Consistent Snapshot

The task SHALL publish a `Status` snapshot once per iteration, readable lock-free by any
task without observing a mixture of two moments.

Direct accessors remain correct from the owned task's own callbacks, but off-task they are
reads racing a writer, and `Stats` is a seven-field struct.

#### Scenario: A reader never sees a torn Stats

- **WHEN** a web handler reads `status()` while the owned task is publishing
- **THEN** it either retries or returns one coherent moment
- **AND** it never returns half of one iteration and half of the next

### Requirement: Start Refuses Rather Than Silently Not Polling

`start()` SHALL return false, with a log line naming the reason, when the display was never
begun, when it is already started, or when task or queue creation failed.

#### Scenario: A display that was never begun

- **WHEN** `start()` is called before `begin()`
- **THEN** it returns false and logs why
- **AND** it does not create a task that polls a display which will transmit nothing and
  report no reason

### Requirement: Stop Joins Without Tearing A Transfer

`stop()` SHALL let a message already on the wire finish, complete pending and un-started
renders as `Cancelled`, and be safe to call twice and from a callback.

#### Scenario: Called from the owned task

- **WHEN** `stop()` is called from inside a callback, where it cannot join itself
- **THEN** it requests the stop and returns rather than deadlocking

#### Scenario: Not used to gate an OTA update

- **WHEN** firmware is about to be written
- **THEN** the application SHOULD gate the transmitter rather than call `stop()`
- **AND** the reason is that `stop()` joins, and joining waits up to two acknowledgment
  timeouts — seconds of a stalled upload handler on the only route back into the board
