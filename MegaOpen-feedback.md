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

> ### How to read this file
>
> **Two entries here were written as library defects and both were wrong** —
> findings 1 and 2b, retracted the same day, kept visible rather than deleted so
> the reasoning can be checked. Both failed the same way: I concluded *"the
> library does X"* from **not observing something in a sampled window** — no ack
> in a 24-frame ring snapshot, no log line in a ten-second capture. A ring that
> holds eight seconds of traffic and a capture that starts after the event are
> not evidence of absence.
>
> So: entries that quote **bytes off the wire in both directions**, or a line the
> hardware actually printed, are measurements. Entries that say "never" or
> "always" about the panel, without that, are not — check them before spending
> implementation time. The integration itself is working; the remaining entries
> below are ergonomics, not blockers.

---

## 2026-07-27

### 1. OBSERVATION, NOT A BUG — a sustained window where the panel flow-controlled but never completed a transfer

> **RETRACTED AND REWRITTEN THE SAME DAY.** The first version of this entry
> claimed, as a blocking bug, that this panel *never* sends the `0x74` DONE ack
> and that the library's expectation was therefore wrong. **That conclusion was
> wrong.** The panel does send `0x74`, and once it started doing so every render
> has completed `Result::Ok`. The library behaved correctly throughout: no DONE
> arrived, so it reported `Timeout`, which is exactly what it should do. What
> follows is what was actually observed, kept because the window was real and
> lasted about fifteen minutes — but it is an open question about the panel, not
> a defect in this library, and nothing here should be implemented against.

**What happened.** For roughly fifteen minutes after the panel was first
connected — with registration fully established (`FUNCSREG` latched,
`registered() == true`) — every render put its complete byte sequence on the
wire, the panel flow-controlled each frame, and no terminal ack ever arrived.
Every call completed `Result::Timeout`. It was reproducible on demand across
`setText`, `setTime` and `showFullscreenText`.

It then stopped, and has not recurred. Renders now complete `Ok`, including
`setTime`, which is a single frame. We do not know what changed. Candidates we
could not separate: the panel needing `setPower(true)` to have taken effect
first; a settling period after registration; or the first transfers racing the
two registration probes the transport prepends before `FUNCSREG`. We did send
power-on during the window, so power alone does not explain it.

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

**And what it looks like now**, same panel, same firmware, a `setTime` and a
`showPopupText` a few minutes later:

```
T 151  05 56 31 38 30 31 00 00      single frame
R 551  74 A3 A3 A3 A3 A3 A3 A3      <- DONE. It does send it.
```

`handleAckFrame()` (`core/AffaDisplayBase.cpp:280`) accepts `0x74` as DONE and
`30 01 00` as PARTIAL. Both halves match the wire. Nothing to change.

**The one actionable thing left in this entry.** During the window there was no
way to tell *"the panel is not answering at all"* from *"the panel acknowledged
every frame and then stopped one short"*. Both are `Result::Timeout`. The second
is a far more specific symptom and would have pointed straight at the tail of the
transfer instead of sending us to look at the transceiver and the termination. A
distinct `Result`, or simply the frame index reached reported in the
`TxComplete` event, would have saved most of an afternoon.

**Two notes for the record, since both were part of the wrong conclusion:**

* `VirtualPanelBase::sendAck()` in `AckMode::Declared` sends `30 01 00` while
  `declaredDone()` is false and `0x74` once the declared length is satisfied.
  That is a faithful model of what the panel does now. It did not model the
  window above — but that window may not be a panel state worth modelling.
* `examples/90_bench_ota` does not call `setSelfAck()`, so it drives a real panel
  by exactly the path we do. Its working flawlessly is consistent with all of
  the above, and was the observation that prompted the re-check.

---

### 2. `CarminatDisplay::setText` caps visible text at 8 characters, and the API gives no sign of it

`setText` writes `d[1] = 0x0E` (declared content length 14 = 6 header bytes + 8
text bytes) and then always emits `kTextCells` = 14 text cells, so the payload is
always 22 bytes and always three frames. The source comment
(`carminat/CarminatDisplay.cpp:159`) explains this and says the surplus bytes go
on the wire, are acked and are ignored — which matches what we see now.

Confirmed on the panel: `setText("ONTEST")` renders. We have not yet put a
string longer than eight characters up and read the glass, so the 8-character
figure is the library's own arithmetic rather than our measurement.

Two consequences for a consumer:

* **visible text is capped at 8 characters.** `"claude top"` would render as
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

### 2b. WITHDRAWN — `setAuxMode()` does return `NotSupported`, exactly as documented

This entry claimed `setAuxMode()` returns `Ok` while emitting nothing, against
the README. **Wrong.** The owner's console shows the completion line plainly:

```
L [disp] aux -> not-supported
```

The README is correct, the base default reaches this path, and there is nothing
to fix. The "evidence" was that no such line appeared in a ten-second WebSocket
capture I took — which was a sampling window, not a measurement.

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
