# 06_counter_preempt — the responsiveness demo

```
pio run -e ex06_counter_preempt -t upload -t monitor
```

A counter runs 1..1000, re-rendered at 10 Hz — deliberately faster than a full render
round-trip, since each `setText` is 3 frames and every frame waits for the panel's ACK.

| Gesture | Effect |
| --- | --- |
| Pause | stop / resume **instantly**: `abortPending()` then render `PAUSED` |
| wheel up | halve the period (faster), down to 20 ms |
| wheel down | double the period (slower), up to 1000 ms |

## What it prints, and why those three numbers

```
[key ] 0x0005 click  key->cb 214 us  period 100 ms  PAUSED
       cb->wire 138 us   (dropped 1 stale renders)
```

* **`key->cb`** — from the key *frame* being seen at Layer 0 to the application callback
  running. This is the number people think they are debugging.
* **`cb->wire`** — from the callback to the first byte of *our* reply on the link. It is
  matched on `0x151`, not on "the next TX frame", because the 1 Hz sync heartbeat on
  `0x3AF` would otherwise be timed instead and flatter the result.
* **`dropped`** — how many stale counter renders `abortPending()` threw away. **This is
  the number that actually explains the symptom.**

## The failure mode

The key is received promptly in *any* design: `poll()` drains RX and delivers keys
**strictly before** it pumps the transmit FSM, so key delivery costs exactly one `poll()`
regardless of queue depth or of any message in flight. A `WaitAck` with 1900 ms left on its
2000 ms deadline delays nothing on the receive side — the TX FSM checks a deadline and
returns; it never waits.

What goes wrong is everything **already queued**. At 10 Hz in front of a ~300 ms transfer
there are several stale counter values waiting their turn, so the panel keeps counting for
about a second after the user pressed Pause and after the library correctly delivered the
key. It reads as key latency; it is a queueing bug. Two mechanisms remove it, and this
example uses both:

1. **Coalescing** (`AFFA_TX_COALESCE=1`, default). A render supersedes a queued,
   not-yet-started render of the same `RenderSlot` instead of stacking behind it. The
   superseded ticket completes `Result::Aborted`. A repeated render therefore occupies
   exactly **one** queue slot no matter how fast you render, and it always holds the
   newest value.
2. **Preemption** (`abortPending()`). Drops every job of which not one byte has been handed
   to `ICanLink::send()`, reports each dropped ticket as `Aborted`, and returns the count.

Rebuild with `-D AFFA_TX_COALESCE=0` and watch `dropped` climb: without coalescing the
queue fills with stale values (and then returns `QueueFull` for the rest), which is the
"panel keeps counting after Pause" symptom in its pure form.

## What `abortPending()` does *not* do

* **It never splits the message on the wire.** The job that has started keeps going. This
  is not politeness: a half-transferred ISO-TP message leaves the panel holding a partial
  screen, and whether it recovers cleanly has never been verified on hardware.
* **It never drops a pending function-registration job.** A payload that reaches the panel
  before its function is registered is rejected, and the resulting `SendFailed` looks
  exactly like a wire-format bug — an hour of debugging the wrong layer.

`abortAll()` exists and additionally abandons the in-flight message at the next frame
boundary. It is a bench and shutdown tool. Routine preemption is coalescing +
`abortPending()` + `Priority::Urgent`.
