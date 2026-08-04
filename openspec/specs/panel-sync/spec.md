# panel-sync Specification

## Purpose

The AFFA handshake: how a node in the radio role establishes and keeps a conversation with
a panel, and how it registers the function ids it intends to draw on.

One state machine in `AffaDisplayBase` serves every panel family. Families differ only by
data — a `SyncProfile` of ids and payload bytes.

## Requirements

### Requirement: Sync Behaviour Is Data, Not Code

Each panel family SHALL supply a `SyncProfile` giving the sync id, the reply id, the reply
flag, the alive byte, the request byte, the request argument, the filler, and the hello
frames in transmit order. The state machine SHALL be shared.

#### Scenario: Carminat profile

- **WHEN** the Carminat family is selected
- **THEN** the profile is sync `0x3AF`, reply `0x3CF`, reply flag `0x0400`, alive `0xB9`,
  request `0xBA`, request argument `0x00`, filler `0x00`, three hello frames

#### Scenario: Filler bytes are not harmonised

- **WHEN** a Carminat request frame is built
- **THEN** it is `BA 00` followed by six literal `0x00` bytes
- **AND** the request argument stays `0x00` even though the panel's own filler is `0xA3`

### Requirement: The Heartbeat Is Unconditional And Frequency-Independent

While begun and not passive, the library SHALL transmit the alive frame once per
`AFFA_SYNC_INTERVAL_MS`, driven by a clock deadline and never by counting `poll()` calls.

#### Scenario: Heartbeat at 1 Hz regardless of poll rate

- **WHEN** `poll()` is called at 2 ms intervals, or at 200 ms intervals
- **THEN** the alive frame `B9 00 00 00 00 00 00 00` goes out once per second in both cases

### Requirement: The Sync Request Is Sent Only While Unsynced

The request frame SHALL be transmitted only while the sync state carries `Failed` or
`Start`.

#### Scenario: Request repeats while the panel is not heard

- **WHEN** no frame has been received from the panel
- **THEN** the state stays `Failed` and `BA 00 …` accompanies every heartbeat
- **AND** a trace showing only `TX 3AF B9 …` and `TX 3AF BA …` at 1 Hz means precisely
  "we are transmitting correctly and hearing nothing back"

#### Scenario: Request stops once synced

- **WHEN** the panel's frames are being received and `Failed` is cleared
- **THEN** the heartbeat continues and the request frame is no longer transmitted

### Requirement: The Radio Announces, Then Answers The Panel's Second Request

> **Rewritten 2026-08-04** against four OEM captures of a real radio and a real Carminat
> (`docs/captures/*.csv`), and proven on glass. The previous version of this requirement —
> *"The Panel Opens The Conversation"*, with a `70 1A 11 …` hello answering the **first**
> `61 11` — described a handshake that does not open a session: without our `BA` first, the
> panel never sends its own `1C1` and registration never begins.

The library SHALL announce itself into silence, and SHALL answer the panel's **next** request
after that announce — not the first one it hears.

Measured opening, from `aknowledge offed display cONNECT OT POWER.csv`. Filler identifies the
sender: `0xA3` is the panel, `0x00` is us.

```
RX  3CF  61 11 01 A3 …               panel, repeating every ~104 ms into an empty bus
TX  3AF  BA 00 00 00 00 00 00 00     us: the announce. THIS FIRST.
RX  3CF  61 11 01 A3 …               panel asks again, +81 ms, on its own timer
TX  3AF  B0 14 11 00 1F 00 00 00     us: hello #1, +30.75 ms after THAT request
RX  1C1  70 A3 …                     panel opens ITS channel, +0.81..1.55 ms
TX  5C1  74 00 …                     us: mandatory reflex, +0.25..0.48 ms
TX  3AF  B0 14 11 00 1F 00 00 00     hello #2 and #3, 31 ms apart. Exactly three.
TX  151  70 00 …                     us: only now, ~61 ms after the panel's 1C1
TX  1F1  70 00 …                     pipelined, before either is acknowledged
RX  551  74 A3 …   RX  5F1  74 A3 …  panel: registration complete
TX  151  03 52 09 00 …               +400 ms exactly. Display ON, always first.
```

#### Scenario: The announce precedes the hello burst

- **WHEN** a complete `61 11 xx` arrives and no `BA` has yet been transmitted
- **THEN** the bounded announce is armed and transmitted
- **AND** no hello frame is sent for that request
- **AND** the panel's next `61 11 xx` transmits the hello burst after the profile's delay

#### Scenario: The hello burst is three identical frames paced from the request

- **WHEN** the announce has been transmitted and a `61 11 xx` arrives
- **THEN** `B0 14 11 00 1F 00 00 00` is transmitted three times, 31 ms apart
- **AND** the first is timed from the request, not from the announce — anchored on the
  request the spread across four captures is 0.79 ms; anchored on the announce it is 80 ms
- **AND** exactly three are sent, never two and never four

#### Scenario: Byte 2 does not gate anything

- **WHEN** the sync request carries `0x00` or `0x01` in `data[2]`
- **THEN** both are treated as the same request
- **AND** a frame shorter than three bytes is ignored entirely, because reading past the
  length would act on a byte the panel never transmitted

> `0x01` was long believed to mean "discovery only, wait for `00`". It does not:
> `cONNECT OT POWER.csv` completes an entire session — registration, display-on, ISO-TP
> text — on sixteen `01` frames and **zero** `00` frames. The low bit reports the panel's own
> state; it is not an authorization grade.

#### Scenario: A registered panel that asks again has voided the session

- **WHEN** a complete `61 11 xx` arrives while function registration is held
- **THEN** the session is torn down and the opening re-runs
- **AND** application traffic stops until registration is regained
- **AND** the value of `data[2]` plays no part in this

> A registered display never sends `61 11` at all in any capture, so one arriving is real
> loss. Keying this on `0x01` alone let a panel send `61 11 00` forty-one times while the
> library kept pushing fullscreen renders at it.

#### Scenario: Peer-alive clears the failure

- **WHEN** `69 00 …` arrives
- **THEN** `PeerAlive` is recorded as a transient that the next heartbeat consumes
- **AND** `Failed` is cleared

### Requirement: A Silent Peer Tears The Session Down

When no peer frame has arrived for `AFFA_PEER_TIMEOUT_MS`, the library SHALL set `Failed`,
emit `PeerLost`, drop `FuncsReg`, and re-arm the handshake for the next poll.

The effective silence window is up to `AFFA_PEER_TIMEOUT_MS + AFFA_SYNC_INTERVAL_MS`, so a
test that starves the link by the timeout alone will not observe a teardown.

#### Scenario: Registration does not survive a resync

- **WHEN** the peer times out
- **THEN** `FuncsReg` is cleared along with every other flag except `Failed`
- **AND** the panel is assumed to have forgotten us too

#### Scenario: The timeout must exceed the longest flash write

- **WHEN** an OTA update writes flash, stalling CAN reception because the TWAI ISR is not
  in IRAM
- **THEN** `AFFA_PEER_TIMEOUT_MS` MUST be larger than that stall, or the library reports a
  lost peer for a panel that never went away

### Requirement: Function Registration Is Lazy And Latched

Registration SHALL happen on the first render call, sending payload `0x70` to each function
id in table order, and SHALL latch once each id is acknowledged on `id | replyFlag`.

The order of `funcIds` is on the wire; the array must outlive the display object.

#### Scenario: Registration precedes the payload it was triggered by

- **WHEN** the first render is enqueued and no function is registered
- **THEN** the registration probes are queued ahead of it
- **AND** a payload that reached the panel before its function was registered would be
  rejected, producing a `SendFailed` that looks exactly like a wire-format bug

#### Scenario: Registration jobs are invisible to the application

- **WHEN** a registration probe completes
- **THEN** no `onComplete` callback fires for it, because it carries no application ticket

### Requirement: Passive Mode Injects Without Arbitrating

In passive mode the library SHALL send no sync frames, no hello, and no generic
acknowledgment, and SHALL never latch `FuncsReg`. It SHALL only inject data.

#### Scenario: A real radio owns the handshake

- **WHEN** passive mode is set on a vehicle bus where a genuine head unit is present
- **THEN** the library transmits only the renders it is asked for
- **AND** it does not compete with the real radio for the handshake

### Requirement: Self-Sent Frames Are Never Treated As Received

Frames the library transmitted SHALL be marked and dropped before the auto-acknowledgment,
before the acknowledgment matcher, and before the key decoder.

A real controller does not receive its own transmissions, but `LoopbackLink` does — without
the flag, host tests would not describe the target.

#### Scenario: All three paths reject a self-sent frame

- **WHEN** a transmitted frame is observed on the receive path under loopback
- **THEN** it does not produce an auto-acknowledgment, does not complete a transfer, and
  does not decode as a key press

Missing the acknowledgment matcher makes a loopback transfer complete after one frame with
a bogus success; missing the auto-acknowledgment makes the library acknowledge itself into
a storm. Both have been observed on real hardware.
