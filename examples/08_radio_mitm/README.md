# 08_radio_mitm — the seam test

```
pio run -e ex08_radio_mitm -t upload -t monitor
```

**The point of this example is that none of it is in the library, none of it needs to be,
and none of it was awkward to write.** Three things that used to be hard-wired inside
`CarminatDisplay` are reimplemented here entirely in application code, against the public
API, with no library internal touched, no `friend`, and no blocking:

| Was | Is now |
| --- | --- |
| `sendPasswordSequence()` — `delay(1000)` + four `delay(200)` inside the receive path | `subscribe(FrameMatch{...})` to detect, `pressKey(k, e, KeySource::Wire)` to answer, two deadlines on the application's own clock |
| `AuxModeTracker` fed unconditionally from `recv()` | a `subscribe()` on `0x151` and twenty lines of pattern matching |
| "hold Load opens the menu", welded into `Menu::handleKey` | `clearMenuHotkey()` + `nav(NavCommand::Open)` on a double-click |

## (a) The password sequence

The trigger is the second ISO-TP frame of the radio's PIN screen, observed on the bench as
exactly `21 20 20 B0 30 30 30 20` on `0x151`. The answer is the remote's presses:
`RollUp ×5, Load, RollUp ×3, Load, RollUp ×2, Load, RollUp ×1, Load(hold)` — 1000 ms to arm
(the radio is still drawing when we match), then 200 ms between presses, exactly as the
extracted code paced them.

Four things it demonstrates, and one it deliberately does not:

* **`KeySource::Wire`, not `Local` and not `Both`.** We are impersonating the panel at a
  real radio. A `Local` press would drive *our own* menu and tell the radio nothing — and
  with `Both` these fourteen phantom presses would also walk our menu while they ran.
* **The `dataMask` is doing real work.** No decoded event the library publishes identifies
  this screen. The application needed the bytes and got them without re-deriving the
  protocol.
* **The echo rule earns its keep.** On a link that echoes, the transmitted key frames come
  straight back; because they carry `Frame::fromSelf` the library's key decoder ignores
  them, so a host run of this example observes exactly what a bench run observes.
* **The blocking is gone, not moved.** The subscription callback *arms* and returns —
  waiting there would be the old `delay(1000)` with extra steps, and it would stall the
  very `poll()` that has to deliver the ACKs for what it sends. Meanwhile the 1 Hz sync
  heartbeat keeps its cadence and a key arriving mid-sequence is still delivered within one
  poll.
* **What the library did not supply: the timer.** Pacing presses at 200 ms is radio policy,
  so the pacing lives in the application, driven by the same `IClock` the library uses.
  That is the seam behaving correctly, not a gap in it.

Why `Wire` is in the library at all, rather than the application building a frame and
calling `link.send()`: the key **encoding** is panel-defined — the `0xC0` hold mask, the
exemption of the two encoder codes `0x0101`/`0x0141` from that mask, and the per-family key
id (`0x1C1` here, `0x0A9` on UpdateList). `pressKey(Load, Hold, Wire)` transmits
`03 89 00 C0 00 00 00 00`, byte-identical to the legacy `emulateKey(Load, true)`.

## (b) AUX detection

A second `subscribe()`, this one id-only on `0x151` — every frame the radio sends on the
text channel. The callback pairs a `0x10` header frame with the `0x21` continuation that
follows it within 200 ms and reads the text cells:

```cpp
void onRadioFrame(const affa::Frame& f, void*) {
  if (f.data[0] == 0x10) { /* keep the header and its arrival time */ return; }
  if (f.data[0] != 0x21 || !g_haveHead) return;
  if (affa::expired(g_clock.millis(), g_headMs + 200)) return;
  if (f.data[1] == 'A' && f.data[2] == 'U' && f.data[3] == 'X') ...
}
```

Three things this shows, and they are the reasons it is written this way:

* **It works on raw frames, not on a decoded string, on purpose.** The full discriminator
  includes *header* byte 6 — the `setText` format byte, which separates the radio-digit
  style from plain ASCII — and no reassembled string carries it. The library publishes no
  decoded-text event and nothing in it reassembles inbound ISO-TP, so there was nothing to
  give up.
* **`data[0]` of the `0x21` frame is the last byte of the header region, not text.** Every
  index starts at 1. Off by one here and the classifier silently never matches.
* **No pattern matched means retain the previous verdict**, never flip to "not AUX". A
  classifier that flipped would toggle the application's source state on every screen the
  table has not seen.

Two patterns are implemented, which is enough to show the shape. **The full seven —
`"AUX"`, `"RENAULT"`, `"TR n CD"`, `"M nnnn"`, `"L nnnn"`, the leading `"> "`, and the
digits-only frequency case — are tabulated in `docs/PROTOCOL-NOTES.md` §8**, together with
why each index and the `0x59` threshold are what they are. They describe *one Renault radio
family*, not the panel and not your car, which is exactly why they are a table in a
document and not a class in the library.

> This used to be `AuxModeTracker` behind `AFFA_ENABLE_AUX_TRACKER`, and before that it was
> wired unconditionally into `recv()`. The class is gone — nothing depended on it, no test
> covered it and no example used it — and the knowledge it held is in §8.

## (c) The custom hotkey

`clearMenuHotkey()` is called **first**, and it is not optional here: with the default
hotkey live, hold-Load would still open the menu behind our back — and hold-Load is
precisely what the PIN sequence ends with. After clearing it, `nav(NavCommand::Open)` is
the only way in, and a closed menu consumes nothing, so every key reaches the application's
`KeyCb` and a double-click gesture becomes possible.

`nav()` must be called from the task that owns `poll()`. A web console or a BLE handler on
another task must post to a queue that task drains: the library is not internally locked.

## Safety

`KeySource::Wire` puts **phantom button presses on the bus**. On a bench with one panel
that is harmless and is exactly the point. On a **vehicle** bus it is injecting input other
modules may act on. Do not ship this in a car unless you know precisely which module is
listening and what it will do.
