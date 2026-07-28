# render-queue Specification

## Purpose

Every render becomes a transmit job. This capability covers the queue's admission rules,
latest-value-wins coalescing, priority, retry and backoff, preemption, and the two-verdict
contract that keeps "was it accepted" separate from "did it arrive".

## Requirements

### Requirement: A Render Result Is An Acceptance Verdict

Every render call SHALL copy its bytes, enqueue, and return immediately. The returned
`Result` SHALL mean "was this accepted into the queue", never "did this reach the glass".

Delivery verdicts arrive later through `onComplete`.

#### Scenario: Bytes need not outlive the call

- **WHEN** a caller passes a stack buffer to `setText()`
- **THEN** the payload is copied into the job
- **AND** the caller may reuse or destroy the buffer immediately

#### Scenario: Rejection is signalled by kNoTicket

- **WHEN** the queue is full
- **THEN** `enqueue()` returns `kNoTicket` and `lastResult()` holds `QueueFull`
- **AND** the render call returns that reason rather than `Ok`

#### Scenario: A dropped return value is a silent failure

- **WHEN** a caller ignores the result of a render
- **THEN** the compiler warns, because render entry points are `[[nodiscard]]`

Rationale: `kNoTicket` is the only signal of rejection. Dropping it turns a `QueueFull`
into a screen that never appears and no diagnostic anywhere.

### Requirement: Latest Value Wins Per Render Slot

A job carrying a `RenderSlot` SHALL replace an existing, NOT-YET-STARTED job with the same
`funcId` and slot. The superseded job's ticket SHALL complete `Aborted`.

#### Scenario: A fast producer does not build a backlog

- **WHEN** `setText()` is called ten times faster than the wire can carry it
- **THEN** at most one un-started text job is queued at any moment
- **AND** what reaches the panel is the most recent text, not a ten-deep history

#### Scenario: A started job is never coalesced

- **WHEN** the head job has already had one byte handed to the link
- **THEN** a new render of the same slot queues behind it rather than replacing it
- **AND** `started` is the single authority for this — never queue position, transmit state
  or frame index

#### Scenario: Different slots do not displace each other

- **WHEN** a text render and a clock render are queued together
- **THEN** both survive, because `RenderSlot::Text` and `RenderSlot::Clock` are distinct
- **AND** a highlight does not replace a pending full menu redraw, nor the reverse

### Requirement: Urgent Jobs Splice Ahead Of Normal Ones

`Priority::Urgent` SHALL insert after started and registration jobs but ahead of queued
`Normal` jobs.

#### Scenario: Registration is never jumped

- **WHEN** an urgent render is enqueued while registration probes are pending
- **THEN** it is placed after the probes
- **AND** a payload cannot overtake the registration its function depends on

### Requirement: Transient Failures Retry, Deliberate Ones Do Not

A failed job SHALL be retried only when the failure is transient and attempts remain.
Backoff SHALL double per attempt. A job whose bytes had already started SHALL additionally
owe the panel `AFFA_TX_DIRTY_QUIET_MS` of silence before restarting.

`SendFailed` — the panel answered something that was neither DONE nor PARTIAL — is
deliberately NOT retryable: the panel rejected the content, and sending it again produces
the same rejection.

#### Scenario: A link fault spends no attempt

- **WHEN** a job fails because the link reported itself down
- **THEN** the result is `LinkDown`, not `SendFailed`
- **AND** no retry attempt is consumed, because the failure was not the render's fault

#### Scenario: The hold window is bounded

- **WHEN** a job waits for a usable link until its hold window expires
- **THEN** the expiry reports `LinkDown` through the give-up path with retry disabled
- **AND** the job leaves the queue

Rationale: routing that expiry through the normal path re-armed the job forever, because
`LinkDown` is exactly the result the retry branch treats as "not the render's fault". That
produced a hold that could never end.

### Requirement: Preemption Drops Queued Work Without Tearing The Wire

`abortPending()` SHALL drop every queued, not-yet-started payload job, report `Aborted` for
each ticket, leave registration jobs alone, and return how many were dropped. The job
already on the wire SHALL be untouched.

#### Scenario: Registration survives preemption

- **WHEN** `abortPending()` runs while registration probes are queued
- **THEN** the probes remain
- **AND** only payload jobs are discarded

#### Scenario: Abandoning mid-transfer happens at a frame boundary

- **WHEN** `abortAll()` is called during a multi-frame transfer
- **THEN** the transfer is abandoned at the next FRAME BOUNDARY, never mid-frame
- **AND** the continuation counter is reset

The panel is left holding a partial transfer, and whether it recovers cleanly is NOT
verified on hardware. Routine preemption is coalescing plus `abortPending()` plus
`Priority::Urgent`.

### Requirement: Losing Sync Cancels Queued Renders

When sync is lost or `begin()` is re-run, queued jobs SHALL be discarded and their tickets
SHALL complete `Cancelled`.

#### Scenario: begin is a reset

- **WHEN** `begin()` is called on a running display
- **THEN** the FSMs reset, the queue empties, every queued ticket reports `Cancelled`
- **AND** nothing is transmitted by `begin()` itself; the first frame leaves on the first
  `poll()`

### Requirement: A Caller Can Ask What Is Pending

The queue SHALL expose whether a not-yet-started job exists for a given slot, how many jobs
are waiting, and whether a job is in flight.

#### Scenario: Tests assert on pending rather than counting frames

- **WHEN** a test needs to know that a render was superseded
- **THEN** `pending(slot)` answers exactly and cheaply
- **AND** the test does not have to infer it from frame counts

### Requirement: One Render In Flight When Ordering Matters

An application whose steps depend on the panel's state SHALL drive them from completion
callbacks, not by enqueuing the steps together.

The queue is not an ordering mechanism for operations with physical preconditions. A
power-on followed immediately by a text render puts both on the wire microseconds apart,
and the text is drawn into a panel that is not lit yet.

#### Scenario: Power-on then text

- **WHEN** an application powers the display on and then writes text
- **THEN** it waits for the power render to COMPLETE
- **AND** waits a further settling period (0.5–1 s; 750 ms is the value the bring-up example
  uses) before enqueuing the text

#### Scenario: Two texts back to back would coalesce

- **WHEN** two `setText()` calls are made before the first has started
- **THEN** the second replaces the first, and the first never appears
- **AND** this is correct queue behaviour, not a defect — which is why sequencing belongs
  in the application
