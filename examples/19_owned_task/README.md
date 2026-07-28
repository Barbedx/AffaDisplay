# 19_owned_task — the library owns the poll task

The reference for the 0.3.0 threading model, and the firmware the bench rig runs.

```
pio run -e ex19_owned_task            build
pio run -e ex19_owned_task -t upload  first flash, over USB
thereafter: http://<ip>/update        ElegantOTA
```

## What is on the glass

```
10:04:33                        the clock, drawn by hand, 1 Hz
AFFA 0.3.0 - THE LIBRARY …      marquee, 300 ms per symbol
THREE ROWS, THREE CLOCKS …      marquee, 700 ms per symbol
```

Three rows on three different clocks, redrawn only when something actually moved, through
`showFullscreenText` (`0x21` mode `0x05` — three equal lines, no row tags). Once every
20 s a popup covers it for 3 s with a counter that ticks each second, and **the rows keep
moving underneath** — visible at the line ends the overlay does not cover.

Two panel facts are being demonstrated there, both measured on a real Carminat and both the
opposite of what `WIRE-SPEC.md` §8.6 said before 2026-07-28:

* **A fullscreen needs no teardown.** Any other full-screen render replaces it, so this
  example never calls `hideFullscreenText()` between screens.
* **A popup is the true overlay, and its lifetime is yours.** No auto-revert has been
  observed; `hidePopup()` is what clears it, which is why the popup here has a deadline.

`setTime("1000")` also goes out on every fresh sync. That is a different mechanism from the
clock row — it writes the panel's *own* clock widget, the one thing a Carminat draws with
the display powered off — so 10:00 appearing on the panel means the handshake completed and
registration latched, with no console and no serial cable.

## What is being proved

**No `poll()` anywhere in the application.** `loop()` renders and is allowed to be slow;
the library polls itself every 2 ms at priority 2. The HTTP handlers call
`task.setText(...)` **directly, from the web server's task**, with no mailbox, no mutex and
no hand-off — the ~250 lines of mailbox that every consumer used to need are what this
release deletes.

Read it off `/api/status`:

| Field | Healthy | Meaning |
| --- | --- | --- |
| `task.foreignPolls` | `0` | nobody but the owned task calls `poll()`. If it climbs, someone left a `poll()` in `loop()`; those calls did nothing. |
| `task.pollLateMaxUs` | ~300–500 | worst iteration, µs, against a 2000 µs period. A blocking callback shows up here first. |
| `task.queueDropped` | `0` | renders posted faster than the wire drains. This example gates on `status().busy`, which is why it stays 0. |
| `task.stackFree` | ~2000 | bytes still unused of `AFFA_TASK_STACK`. Half the stack spare with a marquee and a rendering `KeyCb`. |
| `screen.failed` / `refused` | `0` | delivery verdicts, not acceptance verdicts. |
| `keys.lastKeyToCbUs` | — | wire-to-`KeyCb`, measured. Press a panel button; the answer must stay bounded by the task period, not by what `loop()` is doing. |

## Endpoints

| | |
| --- | --- |
| `GET /` | one self-contained page: buttons, live status, live log |
| `GET /api/status` | everything above, one snapshot, taken under a seqlock |
| `GET /api/log?n=` | the last `n` log lines |
| `GET /api/text?t=` | `setText` — **replaces** the three rows; the demo puts them back |
| `GET /api/popup?t=` / `/api/popup/hide` | drive the overlay by hand |
| `GET /api/state?on=0\|1` | display power. With the panel off a render still completes `Ok` and shows nothing (WIRE-SPEC §8.3) — the cheapest way to produce a delivered-but-invisible render on purpose |
| `GET /api/demo?run=0\|1` | stop/start the three-row repaint |
| `GET /api/resync` | `begin()` on the owned task: tears the link down and rebuilds it |
| `GET /api/reboot` | restart in 400 ms |
| `GET /update` | ElegantOTA |

`/api/resync` is the interesting one. Measured on the rig: `0x01` (Failed) → `0x00` →
`0x08` (`FUNCSREG`) in **110 ms**, the `0x70` registration burst re-sent with the next
render, the link clock re-armed and re-delivered, and `screen.failed` still `0` afterwards.
That is the whole recovery path, on demand.

## The order in `setup()` is the contract

```
log sink and callbacks   ->   display.begin()   ->   task.start(display)
```

`start()` refuses a display that was never begun (`begun() == false`) and refuses a second
call, both with a log line rather than a silent no-op. Callbacks installed *after* the task
is running would miss whatever it has already delivered.

## What an application still owes the library

One thing, and it is the price of the mode: **callbacks now run on the library's task, so
they must not block.** `onKey`, `onSync` and `onComplete` here append to a ring and return.
The log ring takes a `portMUX` spinlock because it is now written from more than one task —
that is three lines, and it is the only new obligation.
