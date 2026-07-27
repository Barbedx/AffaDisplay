# 02_carminat_text — sync, then text

```
pio run -e ex02_carminat_text -t upload -t monitor
```

The example that demonstrates the goal: a `CarminatDisplay`, the CAN link, a clock, and a
counter re-rendered once a second. Built with `-D AFFA_ENABLE_MENU=0`, so this is the
rendering path alone.

## What you should see

```
supports Text=1 Time=1 Power=1
[sync] 0x02  PEERALIVE   synced=1 registered=0
[sync] 0x06  PEERALIVE START   synced=1 registered=0
[app ] setText("CNT 1") -> Ok, ticket 3
[sync] 0x0E  PEERALIVE START FUNCSREG  synced=1 registered=1
[tx  ] ticket 3 -> Ok
[app ] setText("CNT 2") -> Ok, ticket 4
```

`FUNCSREG` latches on the **first payload send**, not at handshake time: registration is
lazy, so `0x151` and `0x1F1` each get their one-byte `70` probe immediately before the
first `setText` payload that follows a resync. That is why `registered` is still 0 when
the first `setText` is accepted and 1 by the time it completes.

## The two verdicts

`setText()` **enqueues and returns**. Its `Result` answers *"was this accepted into the
queue?"* — `NoSync` before the handshake, `QueueFull` if six messages are already
outstanding, `Ok` otherwise. Whether the panel drew it arrives later through
`onComplete()` with the same `TxTicket`.

Two `Result`s are worth recognising in the log and are not faults:

* `Aborted` in `onComplete` — a queued render of the same `RenderSlot::Text` was
  superseded by a newer one before a single byte of it reached the wire. Latest value
  wins; at 1 Hz you will rarely see it, at 10 Hz constantly (see `06_counter_preempt`).
* `NoSync` from `setText` — the panel has not come up yet. Nothing is lost and nothing is
  queued; call it again.

`lastEnqueued()` must be read **immediately** after the call that issued it. It holds the
ticket from the most recent successful enqueue, including one made on your behalf inside a
render, and the very next enqueue overwrites it.

## Text length

`setText` transmits 20 bytes but declares 14 (`0x0E`), and the panel consumes the header
plus the first **eight** text bytes. Roughly 7–8 characters are what actually appear.
Do not "fix" the declared length: it is what has been rendering correctly for months, and
a length byte is glass, not style.

Strings are transliterated to the panel charset by `affa::toAscii` inside the builder,
always. UTF-8 that reaches the wire is garbage on the screen, not a compile error.
