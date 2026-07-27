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
| `AuxModeTracker` fed unconditionally from `recv()` | twelve lines against `EventKind::RadioText` |
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

```cpp
void onEvent(const affa::Event& ev, void*) {
  if (ev.kind != affa::EventKind::RadioText) return;
  const bool aux = (std::strstr(ev.text.text, "AUX") != nullptr);
  ...
}
```

`ev.text.text` and `ev.text.raw` point at library-internal storage and **die when the
callback returns** — copy what you keep.

> **Known gap, stated rather than hidden.** `EventKind::RadioText` requires
> `AFFA_ENABLE_ISOTP_RX` (reassembly is what turns a pair of frames into a string), and
> as of this writing **no panel emits it**: `supports(Feature::RadioText)` reports the
> compile gate, but the reassembler in `proto/` is not yet wired into the RX path for
> either family. On target the gate is 0 by default, so this example prints
> `RadioText supported=0` and part (b) stays quiet. Parts (a) and (c) do not depend on it.
> The pattern is what is being demonstrated; when the wiring lands, this file needs no
> change. If you want the heuristics today, `AFFA_ENABLE_AUX_TRACKER=1` gives you
> `AuxModeTracker`, fed by a `subscribe()` on `0x151` — see `docs/API.md` §7b.7b.

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
