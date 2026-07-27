# 01_link_check — is the link actually up?

```
pio run -e ex01_link_check -t upload -t monitor
```

No panel driver, no rendering. This example instantiates the shared core with the
Carminat sync profile, taps every frame (Layer 0) and prints a summary every 2 s.

Wiring: ESP32-C3 SuperMini, `CanPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 }`, 500 kbit/s,
120 Ω at both ends of a two-node bus (board + panel).

## What a working bench looks like

```
TX 3AF B9 00 00 00 00 00 00 00   <3AF ...>       our 1 Hz heartbeat
TX 3AF BA 00 00 00 00 00 00 00                   sync request, only while FAILED
RX 3CF 61 11 A3 A3 A3 A3 A3 A3   <3CF 61 11  sync request>
TX 3AF 70 1A 11 00 00 00 00 01                   hello, then B0 14 11 .. twice
RX 3CF 69 A3 A3 A3 A3 A3 A3 A3   <3CF 69     peer ping>
RX 1C1 70 A3 A3 A3 A3 A3 A3 A3   <1C1 70     registration>
TX 5C1 74 00 00 00 00 00 00 00   <|400 74    ACK DONE>
---- sync=0x07 synced=1 registered=0 peerAcked=0 | ourTx=9 txFrames=9 txDropped=0 txErr=0 ...
```

`registered=1` appears only after the first payload send, because function registration is
lazy — this example never sends a payload, so `registered=0` here is correct, not a fault.

## The acceptance criterion

> **On this two-node bus, `txErr` staying at 0 IS the proof that the panel acknowledges
> us.**

CAN acknowledgement is a link-layer bit: a transmitter that nobody ACKs re-arbitrates and
increments the controller's transmit error counter. With exactly two nodes there is no one
else who could have supplied that bit. So:

| Symptom | Reading |
| --- | --- |
| `txErr` climbing, `txFailed` climbing | nothing is acknowledging us — bad wiring, wrong bit rate, missing termination, or the panel is not powered |
| `txErr = 0`, no `RX` lines at all | we are on the bus and the panel is silent or on a different id set |
| `txErr = 0`, `peer ping` ticking, `peerAcked=0` | the panel is alive but has not answered anything **we** sent |
| `ovf` non-zero | `poll()` is not called often enough — frames were LOST |

`peerAcked` is a stronger statement than a clean trace: it means a frame arrived on
`id | 0x400` carrying `74` (DONE) or `30 01 00` (PARTIAL), i.e. the panel answered *our*
transmission. Note that the `age` column matters as much as the count — a counter that
stopped 40 s ago and a counter that is still moving print the same number.

Nothing here matches a filler byte, ever. It is per-node: our bench panel pads `A3`, an
OEM cluster `84`, the OEM radio `FF`. Only `data[0]` (DONE) and `data[0..2]` (PARTIAL)
carry meaning on the ACK channel.
