# remote-serviceability Specification

## Purpose

The bench board has no serial cable and no buttons. Over-the-air update is the only way to
change its firmware, so anything that can take the web server down can take the board with
it.

This capability is about EXAMPLES and applications, not the library — but it is specified
here because losing it costs a session, and it has.

## Requirements

### Requirement: Network And Update Come Up Before Anything Else

An example intended for a remote board SHALL start WiFi and the update endpoint before it
touches the CAN controller, and a CAN failure SHALL be reported rather than fatal.

#### Scenario: The controller does not come up

- **WHEN** `Esp32CanLink::begin()` returns false at boot
- **THEN** the board still serves its console and update endpoint
- **AND** the failure is readable over HTTP instead of being guessed at from a board that
  does nothing

#### Scenario: A SoftAP fallback always exists

- **WHEN** the configured network cannot be joined within the join timeout
- **THEN** the board brings up its own access point
- **AND** remains reachable

### Requirement: The Socket Table Must Never Latch Shut

The HTTP server SHALL be configured with LRU purge enabled before it starts listening.

`PsychicHttpServer` sets `lru_purge_enable` only inside `#ifdef ENABLE_ASYNC`, which these
builds do not define, so it otherwise runs on `HTTPD_DEFAULT_CONFIG()`: seven sockets, LRU
purge OFF. In `esp_http_server` that combination is a one-way door — once every slot is held
by a lingering connection, `httpd` stops calling `accept()` and never resumes.

#### Scenario: The failure is invisible from every angle but the useful one

- **WHEN** the socket table has latched shut
- **THEN** ICMP still replies, mDNS still resolves AND answers, and the board looks alive
- **AND** every TCP connection attempt hangs until timeout with connect time 0

Observed on the bench 2026-07-28: the owner's browser could load the console while `curl`
from the same LAN got nothing for forty consecutive seconds.

#### Scenario: A new connection can always displace an old one

- **WHEN** LRU purge is enabled and all sockets are occupied
- **THEN** a new connection evicts the least recently used one
- **AND** the worst a misbehaving client can do is get itself dropped

#### Scenario: Half-open sockets are reclaimed promptly

- **WHEN** receive and send wait timeouts are set to 3 s rather than the default
- **THEN** a stalled peer releases its slot in seconds

### Requirement: A Diagnostic Page Does Not Poll On A Timer

A console page SHALL NOT refresh on an unconditional interval. Refresh SHALL be explicit,
and any automatic refresh SHALL default to off and run no faster than every 2 s.

The previous console ran `setInterval(tick, 1000)` firing two or three fetches every second
forever. A tab left open is what filled the socket table.

#### Scenario: A forgotten tab is harmless

- **WHEN** the console is left open overnight
- **THEN** it issues no traffic unless auto-refresh was deliberately enabled

### Requirement: The Board Watches Its Own Server

A remote example SHALL periodically verify that its own HTTP server still accepts
connections, and SHALL restart the board when it does not.

#### Scenario: The probe is a real connection

- **WHEN** the self-probe runs
- **THEN** the board opens a TCP connection to its own address and requests a trivial
  endpoint
- **AND** anything short of a real connection would not detect a server that has stopped
  accepting

#### Scenario: It cannot become a boot loop

- **WHEN** the probe fails
- **THEN** a restart is only scheduled after several consecutive failures, never during an
  update, and never within the first two minutes of uptime

### Requirement: Handler Stacks Are Sized For What Handlers Build

A response body assembled in a handler SHALL live in static storage or on the heap, not on
the server task's stack, and the server task stack SHALL be raised above the 4 kB default.

#### Scenario: A large snapshot overflows silently

- **WHEN** a handler copies a 3.2 kB ring snapshot onto a 4 kB task stack
- **THEN** the handler dies and returns an empty body with no error anywhere
- **AND** the endpoint appears to exist and answer, which is worse than a clean failure

Observed while building `examples/01_bringup`: `/api/log` returned an empty body until the
snapshot was made static.

#### Scenario: One shared buffer is safe

- **WHEN** a single JSON buffer is shared between handlers
- **THEN** it is safe because `esp_http_server` runs every handler on one task
- **AND** it costs one buffer instead of kilobytes of per-task stack

### Requirement: An Update Gates The Transmitter, It Does Not Stop The Task

Before firmware is written, an example SHALL shut the CAN transmit gate, and SHALL NOT join
the library's poll task.

A flash write stalls CAN reception outright because the TWAI ISR is not in IRAM, so the
panel looks dead for the duration. Stopping the task joins, and joining waits for a message
already on the wire — up to two acknowledgment timeouts of a stalled upload handler on the
only route back into the board.

#### Scenario: Loss of sync during an update is expected

- **WHEN** an update completes and the board reboots
- **THEN** a peer-lost and a resync are the expected sequence, not a fault

### Requirement: Two Update Slots, Always

A remote example SHALL use a partition table with two application slots.

#### Scenario: A bad image is recoverable

- **WHEN** an update is interrupted or produces an unbootable image
- **THEN** the previous slot still holds a working firmware
- **AND** a single-slot table would require the cable the board does not have

### Requirement: Firmware Must Be Distinguishable Over HTTP

Each example SHALL expose an endpoint that identifies which firmware is running.

#### Scenario: Telling two builds apart without a serial cable

- **WHEN** an operator does not know which firmware was last flashed
- **THEN** a trivial endpoint present in only one of them settles it immediately
