# observation Specification

## Purpose

Three layers of visibility into what the library is doing, so an application can build a
console, a sniffer or a man-in-the-middle without reaching into library internals.

Layer 0 is every frame, Layer 1 is a filtered subscription, Layer 2 is decoded events.

## Requirements

### Requirement: Layer 0 Sees Every Frame In Wire Order

A single frame tap SHALL be offered, receiving every frame in BOTH directions, unfiltered,
in wire order. Installing a second tap SHALL replace the first.

#### Scenario: Both directions pass through one choke point

- **WHEN** the library transmits and receives
- **THEN** the tap observes both, interleaved in the order they hit the wire
- **AND** a sniffer built on it sees the whole conversation

#### Scenario: The tap is on the hot path

- **WHEN** an application installs a tap
- **THEN** it MUST do no more than push into a ring
- **AND** rendering or blocking from it stalls the poll task

### Requirement: Layer 1 Subscriptions Are Fixed And Checked

`subscribe()` SHALL use a fixed table with no allocation, SHALL return an invalid handle
when the table is full or the match is unsatisfiable, and SHALL be observational only —
never consuming.

#### Scenario: A full table is reported, not silently ignored

- **WHEN** the subscription table is full
- **THEN** `subscribe()` returns `kNoSub`
- **AND** an ignored return value is a subscription that silently never fires, which is why
  the result must be checked

#### Scenario: Relative ordering is not guaranteed

- **WHEN** two subscriptions match the same frame
- **THEN** callers MUST NOT depend on which fires first

#### Scenario: A stale handle cannot unsubscribe the next owner

- **WHEN** a slot is unsubscribed and then reused by a new subscription
- **THEN** the old handle is rejected, because each slot carries a generation counter
- **AND** a bare index would silently cancel somebody else's subscription

### Requirement: Layer 2 Events Are Additive

The decoded event sink SHALL fire IN ADDITION TO the dedicated callbacks, never instead of
them.

#### Scenario: A key produces both

- **WHEN** a key frame is decoded
- **THEN** the key callback fires AND a key event is emitted
- **AND** an application may use either without losing the other

### Requirement: Sync Callbacks Fire Only On Real Transitions

The sync callback SHALL fire only when the state actually changes.

#### Scenario: A repeated heartbeat is not a transition

- **WHEN** peer-alive frames keep arriving and the state is already synced
- **THEN** no sync callback fires

### Requirement: Inbound Text Is Available Behind A Gate

Text another node drew SHALL be reassembled from its ISO-TP frames, decoded by the panel,
delivered once per complete message, and gated on `AFFA_ENABLE_ISOTP_RX`.

In the radio role nothing else produces it — we are the node that normally writes that
channel — so this is the sniff and man-in-the-middle seam.

#### Scenario: The buffer belongs to the library

- **WHEN** the text callback fires
- **THEN** the pointer is valid only for the duration of the callback
- **AND** an application that needs it afterwards must copy it

#### Scenario: A payload that is not text delivers nothing

- **WHEN** a reassembled payload is a screen or an info row rather than text
- **THEN** the panel decoder returns false and no callback fires

### Requirement: Counters Distinguish Heard From Decoded

Diagnostics SHALL make it possible to tell "frames reached the library" apart from "the
driver moved a counter".

A driver's receive counter can move while nothing ever decodes, and the question an operator
actually asks is whether the panel is being heard.

#### Scenario: The bench spy separates them

- **WHEN** a diagnostic firmware reports what it heard
- **THEN** it counts frames delivered to the library alongside the raw controller counters
- **AND** the two together distinguish "busy but undecodable" from "silent"

### Requirement: Diagnostics Do Not Filter What They Are Diagnosing

A diagnostic tool SHALL record extended and remote-transmission frames as well as standard
data frames.

The library's own callback discards extended and RTR frames because AFFA is 11-bit data
frames only. A bus carrying them therefore looks silent from inside the library and busy
from a tool that keeps them — and that difference is a diagnosis.

#### Scenario: A spy keeps what the library drops

- **WHEN** an extended-id frame appears on the bus
- **THEN** the diagnostic firmware records it with its id, length, bytes and flags
- **AND** the protocol library still ignores it
