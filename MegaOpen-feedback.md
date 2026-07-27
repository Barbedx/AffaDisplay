# AffaDisplay — feedback from the MegaOpen integration

Findings from consuming AffaDisplay 0.1.0 in **MegaOpen** (`../MegaOpen`), an
ESP32-C3 bridge between a Renault Mégane II Carminat panel and an Android head
unit. Newest first. Each entry says what was observed, on what evidence, what it
costs the consumer, and what a fix might look like — the last part is a
suggestion, not a specification.

Hardware under test: ESP32-C3 SuperMini, SN65HVD230, **real Carminat panel**,
500 kbit/s, two nodes on the bus (panel + us), no radio present. The library
drives our own `ICanLink` over the IDF TWAI driver
(`AFFA_ENABLE_ESP32CAN_LINK=0`), not `Esp32CanLink`.

---

## 2026-07-27

### 1. BLOCKING — the panel never sends the `0x74` DONE ack, so every render ends in `Timeout`

**What happens.** Registration completes perfectly (`FUNCSREG` latches,
`registered() == true`, `synced() == true`). Every render then puts its complete
byte sequence on the wire, the panel evidently consumes it — and the call
completes with `Result::Timeout` after `AFFA_ACK_TIMEOUT_MS`. Every time,
regardless of which render it is.

**What the wire shows.** Captured with a 24-frame ring that records both
directions. `setText("claude top")`, read newest-last:

```
T 151  10 0E 77 55 55 FF 60 01      first frame, declared content length 0x0E
R 551  30 01 00 A3 A3 A3 A3 A3      panel replies
T 151  21 63 6C 61 75 64 65 20      "claude "
R 551  30 01 00 A3 A3 A3 A3 A3      panel replies
T 151  22 74 6F 70 00 00 00 00      "top" + padding
                                    <- nothing. ever.
```

`showFullscreenText()` — a 14-frame transfer — behaves identically: `30 01 00`
after every continuation frame `29`, `2A`, `2B`, `2C`, and **nothing at all**
after the last one, `2D`.

`setTime("1751")`, a single frame, gets **no reply of any kind**:

```
T 151  05 56 31 37 34 39 00 00      single frame
                                    <- nothing
```

**Why we think this is the panel being correct rather than broken.** `30 01 00`
is not an application ack, it is **ISO-TP flow control**: `0x30` = ContinueToSend,
`BS = 0x01` (one frame per block), `STmin = 0x00`. A conforming ISO-TP receiver
sends an FC to permit the *next* block. After the final frame there is no next
block to permit, so it sends nothing — and a single-frame message needs no flow
control at all, so it gets nothing either. Both silences are exactly what the
transport specifies.

`handleAckFrame()` (`core/AffaDisplayBase.cpp:280`) accepts `0x74` as DONE and
`30 01 00` as PARTIAL. The PARTIAL half matches the wire. The DONE half has no
counterpart on this panel.

**The twin disagrees with the hardware, which is why no host test catches this.**
`VirtualPanelBase::sendAck()` in `AckMode::Declared` — the mode the docs call
"the only mode that models hardware" — sends `30 01 00` while `declaredDone()`
is false and **`0x74` as soon as the declared length is satisfied**. The real
Carminat sends `30 01 00` on exactly the same frames and then **nothing** on the
one where the twin sends DONE. So the library's oracle and the panel differ at
precisely one frame, the last, and the whole native suite passes while every
render on real hardware times out. Reproducing this in `test_twin` should be a
one-line ack mode — something like `AckMode::FlowControlOnly`, which is also
what a conforming ISO-TP receiver does.

**This does not contradict the bench console working.** `examples/90_bench_ota`
does not call `setSelfAck()`, so against a real panel it hits the same 2 s
timeout — but its screens still appear, because the bytes do arrive. Judged by
looking at the panel, it works; judged by `onComplete`, every render failed. That
is the whole shape of this bug: the transfer is fine and the verdict is wrong.

Worth noting: `0x74` **does** appear on this bus — in the other direction.
`sendGenericAck()` is *us* answering the panel with `0x74` on `id | 0x400`, and
that is visible in our captures as `T 5C1 74 ...`. That suggests the DONE
expectation may have been generalised from the radio→panel direction, where it is
real, to the panel→radio direction, where (on this panel) it is not.

**What it costs us.** The data renders, so this is not a correctness failure of
the screen — it is a throughput and reporting failure:

* every render occupies the transmit queue for the full 2 s ack deadline, so
  renders serialise at 0.5 Hz;
* a repainting screen is impossible. Our now-playing screen wants a ~2.5 Hz
  marquee step and cannot have one;
* `onComplete` reports `Timeout` for work that visibly succeeded, so the one
  signal that would tell a real failure from a healthy render is unusable;
* `lastResult()` is permanently `timeout`, which makes the web UI's health panel
  lie.

**Possible shapes of a fix** (your call which, if any):

* treat a job as complete when its **last** frame has been handed to the link and
  the preceding FC was received — i.e. stop requiring a terminal ack;
* make the terminal ack a **panel policy** — a field on `SyncProfile`, or a
  virtual `bool expectsFinalAck() const`, since this may genuinely differ between
  Carminat and UpdateList;
* a knob, e.g. `AFFA_REQUIRE_FINAL_ACK` (default 1 to preserve today's behaviour);
* at minimum, distinguish "no ack at all" from "acked every frame but the last"
  in the `Result`, so a consumer can tell this apart from a dead panel.

We have a live rig and can test any of these the same day.

---

### 2. `CarminatDisplay::setText` declares 14 bytes but transmits 22, and the trailing frame is not acked

`setText` writes `d[1] = 0x0E` (declared content length 14 = 6 header bytes + 8
text bytes) and then always emits `kTextCells` = 14 text cells, so the payload is
always 22 bytes and always three frames.

The source comment (`carminat/CarminatDisplay.cpp:159`) says the surplus bytes
"go on the wire, **are ACKed**, and are ignored". On this panel the surplus frame
is **not** acked — it is precisely the unacked last frame in finding 1. So the
two facts are related: the transfer runs one frame past what the panel was told
to expect.

Two consequences for a consumer:

* **visible text is capped at 8 characters.** `"claude top"` renders as
  `"claude t"`. That is not obvious from the API — `setText` takes an arbitrary
  `const char*` and returns `Ok`;
* a caller cannot choose the trade-off, because both the declared length and the
  cell count are compile-time constants inside the builder.

The comment already flags "shortening the payload to match is an untested
change" — we now have the rig to test it, and would be glad to.

Suggestion: make the declared length follow the actual content, or expose the
cell count / declared length so a consumer can experiment; and either way state
the 8-character limit in `docs/API.md` next to `setText`.

---

### 3. `library.json` declares `esp32_can` + `can_common` as hard dependencies

The README says, correctly: *"With `-D AFFA_ENABLE_ESP32CAN_LINK=0` you need
neither `can_common` nor `esp32_can`"*. The manifest disagrees — they are listed
under `dependencies`, and PlatformIO **installs and builds a declared dependency
whether or not anything includes it**.

For us that is not cosmetic. MegaOpen deliberately removed `collin80/esp32_can`
(its runtime mode setters are `disable()` + `enable()` underneath — a driver
reinstall on a live bus, which repeatedly left the controller stopped), so having
it linked back in is exactly what we were avoiding. The consumer-side workaround
is:

```ini
lib_ignore = ESP32_CAN, esp32_can, can_common
```

— and note the manifest name is `ESP32_CAN` (uppercase, from its
`library.properties`), not the folder name, which cost a debugging round.

Suggestion: drop the two from `dependencies` and document them as required only
when `AFFA_ENABLE_ESP32CAN_LINK=1`. The library's own `platformio.ini` already
declares them for its examples, so its own builds are unaffected.

---

### 4. `library.json` `frameworks`/`platforms` blocks host builds in a consumer

`"frameworks": "arduino", "platforms": "espressif32"` makes PlatformIO's
compatibility check exclude the whole library from a `platform = native`
environment. A consumer with host tests over its own code that includes
`<AffaDisplay.h>` — ours does, for the key-mapping layer — gets a bare
`fatal error: AffaDisplay.h: No such file or directory`, with no hint that a
compatibility rule caused it.

Workaround, in the native env:

```ini
lib_compat_mode = off
lib_ldf_mode = chain+          ; so the gated <esp32_can.h> include is not followed
```

This works and the library's `core/`, `util/` and `proto/` compile on the host
exactly as its contract promises. It is only the discovery that is painful.

Suggestion: a sentence in the README's installation section. Relaxing the
manifest would be nicer but has other consequences, so the note may be enough.

---

### 5. Minor — `chain` LDF mode follows the gated `<esp32_can.h>` include

With the default `lib_ldf_mode = chain`, the finder sees the `#include
<esp32_can.h>` inside `link/Esp32CanLink.cpp` even though
`AFFA_ENABLE_ESP32CAN_LINK=0` removes that whole translation unit, and pulls the
driver in. `chain+` evaluates the conditional and behaves correctly.

Suggestion: mention `lib_ldf_mode = chain+` in the README alongside the
`AFFA_ENABLE_ESP32CAN_LINK=0` note.

---

## What is working, for balance

Worth recording, because the list above is only the problems:

* **Registration is solid.** `PEER_ALIVE` and `FUNCSREG` latch within seconds of
  the panel coming up, and stay latched. The wall-clock sync watchdog does what
  it was designed to do — the previous hand-rolled implementation in MegaOpen
  never got this far without flapping.
* **The heartbeat is exactly 1 Hz** and does not flood `0x3AF`. The old code's
  per-call watchdog produced hundreds of frames a second; that entire class of
  bug is gone by construction.
* **The pull-port CAN seam is right.** `recv(Frame&)` let us put the library
  behind our own TWAI driver in about eighty lines, with no callback threading
  question to answer.
* **`poll()` really is frequency-independent.** We call it from a loop with a
  2 ms delay and it needs no pacing at all — the removal of `tick()` is the
  single biggest simplification in our `main.cpp`.
* **Zero TX drops, zero controller errors** over the whole session: `txErr=0`,
  `rxErr=0`, `busErr=0`, `txFailed=0`, with 747 frames received and 453 sent.
* **`supports()` is honest.** With `AFFA_ENABLE_MENU=0` the menu widget
  disappears while `showMenu()` remains available as a panel primitive, exactly
  as `AffaConfig.h` documents — verified on hardware.
