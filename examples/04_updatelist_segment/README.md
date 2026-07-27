# 04_updatelist_segment — marquee and clock on the 8-segment panel

```
pio run -e ex04_updatelist_segment -t upload -t monitor
```

AFFA2 family: sync on `0x3DF`, text on `0x121`, display control on `0x1B1`, keys in on
`0x0A9`, filler `0x81`. The example alternates a 12 s scrolling title with a 6 s clock.

## The marquee is in the library; the title is not

The eight-cell window and the 400 ms step are panel geometry, so they belong here. What a
track is *called* is application territory, so the seam is a string plus a bit:

```cpp
display.setScrollText("AFFADISPLAY - NON BLOCKING MARQUEE");
display.setScrollActive(true);
```

Position is derived, not accumulated:

```
window(now) = (base + (now - epoch) / 400) mod len
```

so it cannot drift, cannot catch up in a burst after a stalled loop, and produces the same
frames at 5 Hz and at 5 kHz. A frame is emitted only when the window actually moves.

Four behaviours worth knowing, all deliberate:

* Re-publishing the **same** text is a no-op — the position is not reset. An application
  that re-sends the current track every second would otherwise freeze the scroll on
  character 0 forever.
* `setScrollActive(false)` draws the frozen window **once** and then transmits nothing at
  all. Resuming continues from where it froze.
* An **empty** title switches the marquee off and transmits nothing, rather than blanking
  the panel — blanking would be a decision the application did not make. This example uses
  exactly that to hand the screen to the clock; call `setText("")` if you do want it blank.
* An inbound AUX frame from the radio re-asserts our window once (`setReassertOnAux`,
  default on — a reaction to a *radio*, therefore a default that can be turned off).

## Why the clock is `setText`, not `setTime`

```
supports Text=1 Time=0 Menu=0 Power=1 KeyTx=1
```

**There is no clock command on this wire.** `setTime()` returns `Result::NotSupported`.
The extracted code's `setTime()` returned `NoError` and put nothing on the bus — the exact
silent no-op that `Feature` and `NotSupported` exist to stop. `"HH:MM:SS"` is eight
characters, which is the whole display, so the application formats it and renders it as
text.

`Menu=0` for the same reason: eight characters have no two-row window. Menu rendering for
this family is whatever the application draws through `setText()` — see `05`.

## Keys and the AMS banner

Hold-Load toggles AMS key forwarding and draws `AMS  ON ` / `AMS OFF ` three times, 100 ms
apart. That was `for (i=0;i<3;i++){ setText(msg); delay(100); }` in the extracted code; it
is now a repeat count and a deadline advanced from `onPoll()`, plus a hold window that
stops the marquee redrawing over the banner in the same pass that emitted the last repeat
— which is what freezing the whole loop used to achieve.

**While forwarding is off, keys do not reach `KeyCb` or `EventKind::Key`.** That is the
extracted semantic and the point of the switch. The raw `0x0A9` frames are still visible
at Layer 0 (`onFrame` tap) and Layer 1 (`subscribe`). If you want the gesture for
yourself, `clearAmsHotkey()` — `05` does.
