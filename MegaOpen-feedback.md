# MegaOpen → AffaDisplay: what a consumer wanted and did not find

Written 2026-08-05, against **v0.5.0**, while moving MegaOpen from v0.2.1 and
from its own CAN stack onto `CanCommonLink`.

**The headline is that the migration was almost free.** v0.5.0 is source
compatible with v0.2.1 for this consumer: the pin moved, both environments built
untouched, and 131 host tests passed with no edit. Every item below is a request,
not a defect. Nothing here blocked anything; each one is a place where MegaOpen
still writes code that looks like it should belong to the library.

Ordered by how much consumer code each would delete.

---

## 1. A popup with a lifetime — `showPopupText(..., ttlMs)`

**The biggest one, and the only one that is a protocol fact rather than a
convenience.**

The Carminat popup has no lifetime of its own: it stays on the glass until
`hidePopup()` removes it. So every consumer that shows one has to own a deadline,
and owning it correctly is not obvious. MegaOpen's version, arrived at by getting
it wrong first:

* the deadline is a wall-clock `millis()` value and not a countdown, because it
  is written from the NimBLE host task and read on the loop task;
* it is armed **only on a successful post**, because arming it regardless
  schedules a hide for a popup that was never shown — and an unpaired
  `hidePopup()` is a change to a screen we did not put there;
* the retry is **backed off**, because a refused `hidePopup()` at loop rate is a
  spin, and the popup being briefly late is a far smaller problem than the
  traffic that clearing it aggressively generates;
* the deadline is **pushed rather than cleared** on failure, so a failure
  schedules the next attempt instead of abandoning the popup on the glass;
* a second popup inside the window **moves the deadline out** rather than
  inheriting the old one, so the newer message is readable for its full time.

That is five rules, all of them library-shaped, and MegaOpen now has two callers
of them (an iPhone notification and a key popup) which is exactly when a
consumer starts getting one of the five wrong in one of the two.

Suggested shape — `ttlMs = 0` keeps today's behaviour exactly:

```cpp
[[nodiscard]] Result showPopupText(const char* text, uint8_t icon, uint8_t srcIcon,
                                   uint8_t fmt, uint32_t ttlMs = 0);
```

with `poll()` owning the hide, its retry and its backoff. `RenderSlot::Popup`
already coalesces, so a second popup inside the window naturally supersedes the
first; the only new state is one deadline and one ticket.

## 2. Listen-only at `begin()` on `CanCommonLink`

**This is the one that cost MegaOpen an architectural compromise.**

MegaOpen serves two roles on one controller: the display bus, and the car's
multimedia bus where it must observe and never speak. The second needs
**hardware** listen-only — not a software transmit gate — because the property
that matters is that a wrong bitrate cannot put error frames into a vehicle
network. A gated controller still ACKs.

`Esp32CanLink` has `setListenOnly()`. `CanCommonLink` has `setTxGate()` and no
mode. So MegaOpen cannot use one link for both roles, and now keeps its own
`TwaiBus` alive purely for the vehicle role, with the two chosen at boot and
never switchable — two CAN stacks in one firmware, which is precisely the shape
that has hurt this project before.

The request is deliberately narrow: **listen-only as a `begin()` argument, set
once, never changed at runtime.** Runtime mode changes are exactly the misuse
that got `esp32_can` removed from MegaOpen in the first place — each
`setListenOnlyMode`/`setNoACKMode` is `disable()` + `enable()` underneath, a
driver reinstall on a live bus — so this asks for the safe half only:

```cpp
bool begin(gpio_num_t rx, gpio_num_t tx, uint32_t bitrate = 500000,
           bool listenOnly = false);
```

If `esp32_can` genuinely cannot do this before `begin()`, saying so in
`CanCommonLink`'s header would be worth as much as the feature: right now the
absence reads as an omission rather than as a limit, and a consumer has to read
two libraries to find out which.

## 3. What the panel can actually sustain

Every consumer that animates has to guess a repaint rate, and the library is the
only party that knows the answer. MegaOpen currently hard-codes a 200 ms floor
from the library's own measured "~190 ms per screen, 14 frames, every one
acknowledged" — a number copied out of a comment, which will be wrong the moment
it is remeasured or the moment a UpdateList panel is attached.

`RowScreen`'s header already teaches the caller to harmonise cadences onto one
grid because "the wire carries roughly five per second". That figure deserves to
be readable rather than taught:

```cpp
uint32_t estimatedRenderMs(RenderSlot slot) const;   // per family, per screen
```

A consumer's repaint gate then becomes `estimatedRenderMs(RenderSlot::Menu)`
instead of a constant that is a fact about somebody else's panel.

## 4. `queued()` alongside `busy()`

`busy()` is binary, so a repaint gate can only be on or off. What a caller
actually wants to ask is "how far behind am I" — enough to back off
proportionally rather than at a threshold. `uint8_t queued() const` (jobs in the
queue, in-flight included) would cost nothing and is already known internally.

Related: MegaOpen counts `Result::Aborted` completions separately to detect
"enqueueing faster than the panel drains", which is a real and otherwise
invisible failure — the glass looks frozen while every error counter reads zero.
A `supersededCount()` on the base would make that diagnosis available without
every consumer keeping its own tally.

## 5. `keyName(Key)`

Small, and it removes a table that has now been written twice. The eight
`[REF]`-attested codes are named in the enum and there is no way to get a string
out of one, so MegaOpen carries `kKeyNames[]` beside its own map. A
`const char* keyName(Key)` returning `nullptr` (not `"?"`) for an unnamed code
would let a caller distinguish "a key we have a name for" from "a raw wire code
worth reporting" — which is the distinction the open enum exists to preserve, and
consumers currently re-derive it with `indexOf() < KEYS`.

## 6. A ready-made frame ring for the display role

Not needed — `onFrame()` Layer 0 is exactly right and MegaOpen will feed its own
ring from it. Noting it only because the obvious next step, a small
`affa::FrameRing<N>` with a JSON-shaped dump, is something every consumer of the
tap will write, and the last-24-frames-with-direction view has been the deciding
evidence in this project more than once. Low priority; the seam is already there.

---

## Things that were exactly right, and why they are worth keeping

Stated because a request list with no counterweight is a misleading document.

* **`Phase`.** Adopted immediately and logged on change. It answers the question
  `SyncState` cannot: not "are we registered" but "what is being waited for".
  Stuck at `Announced` and stuck at `AwaitPeerChannel` are two different faults
  that every counter MegaOpen had reported identically.
* **`LossReason`.** Same reasoning, for the four ways a session ends.
* **`TxDisposition::Busy`.** MegaOpen's own link was blocking `twai_transmit` for
  20 ms per frame on the task that owns the display's deadlines, because a
  boolean seam could not say "the queue is full, come back". That was thirteen
  frames deep into a screen. The three-valued verdict removed it.
* **`healthy()` / `recover()`.** The split from `isLive()` is the right one and
  the header explains it better than a consumer would have guessed: recovery
  must ignore a deliberately shut gate.
* **`setAutoPower()`.** MegaOpen deleted a flag, a ticket, a backoff and four
  re-arming sites for this. The library puts the power-on between registration
  and the first permitted payload, which is where it has to be and where a
  consumer cannot put it — ours raced the held renders and lost.
* **The pin being a tag.** Reproducible on a machine with no sibling checkout,
  which was the entire point of the exercise that produced it.
