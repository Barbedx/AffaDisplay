# PORTING.md — moving on, and moving off

Two audiences, in this order:

1. **[Part A](#part-a--porting-an-application-off-the-extracted-classes)** — you have an
   application built on the classes this library was extracted from (`CarminatDisplay`,
   `ICanBus`, `AffaCommon::AffaKey`, `CanUtils`, …) and you want it running on AffaDisplay.
2. **[Part B](#part-b--dropping-affadisplay-entirely)** — you want to leave: replace the OEM
   panel with an OLED, move to another MCU, or reimplement the protocol yourself. This part
   says plainly which files are panel-specific, which are the reusable transport core, and
   what you must reimplement if you keep none of it.

`docs/API.md` §7 holds the **complete old-symbol → new-symbol table** (about forty rows).
This document does not duplicate it; it gives the procedure, the traps, and the parts that
are not a rename.

---

## Part A — porting an application off the extracted classes

### A.0 The three changes that are not renames

Everything else is mechanical. These three change how your code is *shaped*:

| Was | Is | Why it cannot be a rename |
| --- | --- | --- |
| `ICanBus` with an `onReceive` **push** callback | `ICanLink::recv(Frame&)`, a **pull** port | The library now decides *when* it looks at the bus. That is what lets `poll()` order RX-before-TX and guarantee key latency, and what lets a host test drive a whole session with no driver at all. |
| `setText()` **blocked** until the panel ACKed the whole transfer (up to 2000 ms), and returned the delivery verdict | `setText()` **enqueues and returns immediately**; the delivery verdict arrives on `onComplete(cb, ctx)` | The old blocking wait sat inside the only code path that could have delivered the ACK it was waiting for. The fix is not a faster wait, it is no wait. |
| Unsupported calls returned `NoError` and did nothing | Unsupported calls return `Result::NotSupported` | A silent no-op is indistinguishable from success. If your code ignored return values, it will now see non-`Ok` where it saw `NoError`. |

### A.1 Step by step

**1. One include, one umbrella.**

```cpp
- #include "display/Carminat/CarminatDisplay.h"
- #include "can/HwCanBus.h"
+ #include <AffaDisplay.h>
```

**2. Provide a clock. Delete the delay.**

```cpp
struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};
```

`IClock` has `millis()` and nothing else. If you had an `IClock::delayMs()` implementation,
delete it — and delete every call site. Nothing in the library sleeps, and anything of
yours that slept inside a callback was stalling the pump that feeds it.

**3. Construct with the link and the clock; neither may change after `begin()`.**

```cpp
- CarminatDisplay display;  display.setBus(bus); display.setClock(clock);
+ affa::Esp32CanLink    link;
+ ArduinoClock          clock;
+ affa::CarminatDisplay display(link, clock);
```

**4. Collapse the pump.** `tick()`, `recv(frame)` and `processEvents()` become one call, and
**you no longer feed frames in** — the library drains the link itself:

```cpp
  void loop() {
-   CAN_FRAME f;
-   while (CAN0.read(f)) display.recv(f);
-   display.tick();
-   display.processEvents();
+   display.poll();
  }
```

If your old `loop()` had a `delay()` in it to "give the bus time", delete that too: `poll()`
is frequency-independent and there is nothing to give time to.

**5. Callbacks take a context pointer and return `void`.**

```cpp
- display.setKeyHandler([](AffaCommon::AffaKey k, bool hold) -> bool { ... });
+ static void onKey(affa::Key k, affa::KeyEdge e, void* ctx) { ... }
+ display.onKey(&onKey, this);
```

Consumption is no longer your return value's job: routing into a page or an open menu is
decided inside the library, and your callback sees what was not consumed. There is no
`std::function` and no capture — put your state behind `ctx`.

**6. Split the two verdicts.** This is the port's real work:

```cpp
- if (display.setText("HELLO", 0) != AffaCommon::AffaError::NoError) retry();
+ const affa::TxTicket t = display.lastEnqueued();          // read AFTER the call
+ const affa::Result   r = display.setText("HELLO", 0);     // acceptance only
+ if (r != affa::Result::Ok) { /* rejected outright: NoSync, TooLong, QueueFull ... */ }
+ // delivery arrives later:
+ static void onDone(affa::TxTicket t, affa::Result r, void*) { ... }
+ display.onComplete(&onDone, nullptr);
```

There is no synchronous shape to fall back to, not even for setup. A `sendBlocking(ticket,
timeoutMs)` that pumped `poll()` itself was offered in earlier revisions and has been
removed: nothing ever called it, and the one blocking call in a library that promises never
to block is a trap, not a convenience. If setup must not proceed until a message lands, run
`poll()` in your own loop and watch `onComplete()` — that is the same three lines, and it is
visibly yours.

**7. Expect coalescing.** `setText()` carries `RenderSlot::Text`, so a second call
supersedes a queued, not-yet-started first one and the superseded ticket completes
`Result::Aborted`. That is the fix for "the panel keeps counting after Pause". If you have a
sequence where **every** message must reach the panel, pass
`TxOptions{ .coalesce = false }` on those specific calls — do not turn `AFFA_TX_COALESCE` off
globally.

**8. Move what was never the library's.** Four things left the library on purpose, and each
is reimplementable with public API alone:

| Was in the display class | Now |
| --- | --- |
| `sendPasswordSequence()` (with `delay(1000)`) | `examples/08_radio_mitm` — `subscribe()` arms a step machine, `pressKey(..., KeySource::Wire)` presses. Non-blocking. |
| `_aux.onCanMessage()` wired into `recv()` | `AuxModeTracker` is **deleted**, gate and all. Its seven reverse-engineered patterns are tabulated in `docs/PROTOCOL-NOTES.md` §8; `subscribe()` to `0x151` and write the ~20-line classifier yourself, as `examples/08_radio_mitm` does. It is a heuristic about your radio, so it is your policy. |
| `ISettings` / NVS writes for menu items | Your `MenuItem::onChange` callback. The library persists nothing. |
| media / ANCS / ELM routing | Your application. Drive the library with `setText()` / `showMenu()`. |

**9. Re-read every return value you used to ignore.** `setTime()` and `showMenu()` on an
UpdateList panel used to return `NoError` while putting nothing on the wire; they now return
`NotSupported`. Ask `supports(Feature::X)` first.

### A.2 Traps specific to this port

* **`getMenu()` is on `CarminatDisplay`, not on `AffaDisplayBase`.** Keep a typed handle. A
  base-typed handle gives you `nav()`, which is the panel-agnostic half.
* **The closing keystroke is now consumed.** Hold-`Load` that closes the menu no longer falls
  through to your `KeyCb`. The old `MenuController::routeKey` returned `isActive()` *after*
  handling, so it did.
* **`Menu::clear()` closes the menu without firing `CloseCb`**, so replacing menu content
  does not transmit a `setText("RENAULT")`.
* **A one- or two-item menu now reports scroll indicator `0x00`.** The extracted code
  returned `0x0B` there and read `items[top+1]` out of bounds on a one-item menu.
* **`pressKey(RollUp/RollDown, Hold, Wire)` is refused** with `NotSupported` and transmits
  nothing. A hold edge on the wheel has no wire representation: `0x0101|0xC0` and
  `0x0141|0xC0` are both `0x01C1`. Use `KeySource::Local` for `NavCommand::Increase` /
  `Decrease`.
* **`AFFA_PEER_TIMEOUT_MS` and flash writes.** The TWAI ISR is not in IRAM. If your
  application writes NVS or takes an OTA image, reception stops for the duration and looks
  exactly like a panel that went quiet. Expect a `PeerLost` and a resync; that is correct.

### A.3 A porting checklist you can tick off

- [ ] no `delay()`, `vTaskDelay()` or busy-wait anywhere in code reachable from `loop()`
- [ ] exactly one task calls `poll()`; every other context posts to a mailbox
- [ ] every render call's `Result` is inspected, or deliberately discarded with a comment.
      The compiler now checks this half for you: every `Result`-returning call is
      `[[nodiscard]]`, so an ignored one is a `-Wunused-result` warning. Write
      `(void)display.setText(...)` when you mean it.
- [ ] at least one `-D AFFA_PANEL_*=1` is in `build_flags`. Naming none is an `#error`, and
      that `#error` is also what a mis-typed panel flag looks like
- [ ] `onComplete()` is registered if delivery matters
- [ ] `supports()` is consulted before any optional render
- [ ] no capture-lambdas passed as callbacks; state lives behind `ctx`
- [ ] nothing calls a CAN driver mode setter, a second `watchFor()`, or a per-mailbox
      callback on mailbox 0 or 1 (see README, "Three ways to break the link from outside")
- [ ] `pio test -e native` still passes if you vendored the library and changed it

---

## Part B — dropping AffaDisplay entirely

You might be replacing the OEM panel with an OLED, moving to an MCU with a different CAN
driver, or simply reimplementing the protocol. Here is what is worth keeping, what is not,
and what you cannot avoid rewriting.

### B.1 Which files are panel-specific and which are reusable

| Directory | Lines | Panel-specific? | Keep it if… |
| --- | ---: | --- | --- |
| `src/core/` | ~1950 | **No.** Transport, sync FSM, transmit FSM, queue, key decode, event seam. The only panel knowledge is *parameterised* — `SyncProfile` supplies the ids and the handshake bytes. | you are still speaking AFFA to *something*, on any transport |
| `src/util/` | ~430 | **No.** `AffaText` (UTF-8 → panel charset transliteration) and `AffaLog`. | you need transliteration; the OEM charset table is genuinely reusable for any 7-bit display |
| `src/link/` | ~350 | **No**, but ESP32-specific. `ICanLink` is three methods; `Esp32CanLink` is the only file in the repository that includes `<esp32_can.h>`; `LoopbackLink` is a header-only test double. | you keep CAN. Replacing the MCU means writing one new `ICanLink` and nothing else. |
| `src/proto/IsoTp.*` | ~200 of 544 | **No.** Plain ISO-TP-style fragmentation and reassembly over 8-byte frames. | you keep any multi-frame CAN protocol |
| `src/proto/ScreenDecode.*`, `ScreenModel.h` | ~340 of 544 | **Yes** — every offset is a Carminat/UpdateList payload layout. `ScreenModel` itself (header + two rows + a mode) is generic enough to survive. | you decode panel traffic |
| `src/widget/` | ~940 | **No.** `MenuModel` + `IMenuRenderer` + `MenuGeometry` are a list UI with no wire knowledge; `Marquee` is a scrolling window with none either. Both take their geometry as data and reach a display through an interface. | you want either widget on your own glass (see B.3) |
| `src/carminat/` | ~1250 | **Yes**, except the menu adapter, which is just the seam between `src/widget/` and this panel's frames. | you keep a Carminat panel |
| `src/updatelist/` | ~805 | **Yes** — the 8-segment and LCD encodings. The marquee they drive is **not** here; it moved to `src/widget/Marquee`. | you keep an UpdateList panel |
| `src/rtos/` | ~480 | **No**, but it is the **one directory in this library that requires FreeRTOS** and therefore the one a port to anything else omits entirely. `AffaTask` owns the poll task; `AffaCommand.h` (the command POD, the dispatch table and the ticket→request map) needs nothing but C++17 and is host-tested. Compiled only under `AFFA_ENABLE_TASK=1`, which is an `#error` off ESP-IDF / Arduino-ESP32. | your target has FreeRTOS and you want the library to own its own task (docs/API.md §4b) |

`src/vpanel/` (~825 lines) was here: panel twins used as a test oracle and a no-hardware
dev loop. **Deleted** — they were application-shaped code shipped as library surface.
`setSelfAck()` covers the dev loop, and a decoder over `isotp::Reassembler` +
`affa::screen` covers the oracle in about thirty lines; `test_bench_surface` and
`examples/90_bench_ota` each carry one.

**The short version:** `core/` + `util/` + `link/` + `proto/IsoTp.*` + `widget/` is the
reusable transport, protocol and UI machinery — roughly 3 900 lines with no panel knowledge
that is not supplied as data. Everything under `carminat/`, `updatelist/` and
`proto/ScreenDecode.*` is one specific panel family's wire format, roughly 2 400 lines.

### B.2 Moving to an OLED (or any display that is not an AFFA panel)

Two ways, and they are genuinely different projects:

**(a) Keep the library, add a display.** `IDisplay` is the render interface, and its method
set is honestly panel-shaped: `setText`, `setTime`, `setPower`, `showMenu(header, row0,
row1, scrollIndicator)`, `highlightItem(row)`, popup, fullscreen, confirm box, info popup.
Implement that against your OLED and everything the library renders reaches it — including
`CarminatDisplay`'s own menu, which draws through `affa::CarminatMenuRenderer` and therefore
through exactly `showMenu()` + `highlightItem()`. What you inherit along with it is a
**two-row window** — the geometry of the OEM screen — which on a 128×64 OLED is a
constraint you did not need. Worth it if you are driving both a panel and an OLED from one
menu; not worth it otherwise. If the geometry is the part you object to, you want (b): the
model takes the shape of the display as a constructor argument.

**(b) Keep only the menu.** `widget/MenuGeometry.h`, `widget/IMenuRenderer.h` and
`widget/MenuModel.{h,cpp}` contain no wire bytes, no CAN, no ISO-TP and **no panel header**:
fixed-capacity items, three field kinds (integer / list / read-only), function-pointer
callbacks with a `ctx`, a `MenuGeometry` you choose, and a `render()` that emits
`beginFrame(header, mask)` / one `row(index, text, selected)` per visible row / `endFrame()`.
Lift those three files, write ~20 lines of `IMenuRenderer` against your own draw calls, and
you have kept the part that took the longest to get right while dropping every byte of AFFA.
`docs/MENU-WIDGET.md` §5 is a worked example for a display the library has never seen.
`carminat/MenuController` and `IPage` are the optional fourth file: a page stack and a
`(Key, KeyEdge)` → intent map, which is navigation policy and probably yours to write.

If you are dropping the panel entirely, **stop including `core/`.** It exists to speak to a
panel; carrying it for `AffaRing` (a 40-line power-of-two ring) is not a trade worth making.

### B.3 Reimplementing the protocol yourself

If you keep none of this, `docs/WIRE-SPEC.md` is the file you want; it is written to be
sufficient on its own. The pieces you cannot skip, in the order they bite:

1. **The handshake — and on Carminat *you* open it.** Announce one bounded `B9` + `BA` pair
   on `0x3AF` into silence; the panel then requests `61 11 xx` on `0x3CF` (`00` and `01` are
   **the same request**), and its **next** request +30.75 ms draws your hello burst on
   `0x3AF`/`0x3DF` — Carminat sends **three byte-identical** frames 31 ms apart, and that is
   not a typo in the spec. **Answer the panel's `1C1` with `5C1 74` within ~0.5 ms,
   unconditionally**, including during the burst and before any registration. Then heartbeat
   `B9` **free-running at 500 ms** and answer the panel's `0x69` ping with **nothing** — the
   two are independent timers, not a request/response pair. Get any of this wrong and nothing
   else ever runs.
   *(Corrected 2026-08-04: this item used to say the panel opens the conversation, that only
   the second and third hello frames are identical, and that you "heartbeat at 1 Hz and answer
   the panel's `0x69` ping". All three are disproven by `docs/captures/*.csv`; see
   `docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md`.)*
2. **Function registration is part of the opening, not of the first render.** The `70` probe
   walks the **whole** function table in order (`0x151` then `0x1F1`; `0x121` then `0x1B1`),
   0.1–0.3 ms after the last hello frame, pipelined 0.29 ms apart, with no payload involved.
   Registering only the id you are about to use stalls; registering lazily means an idle
   build never finishes a session. Then wait **400 ms** from the final ACK and send
   `151 03 52 09 00 00 00 00 00` — display ON is **always** the first application payload,
   never a screen and never a clock.
3. **ISO-TP framing with BS = 1 flow control.** 8 bytes in frame 0, 7 per continuation, PCI
   `0x20 | (n & 0x0F)` with `n` starting at **1** — so the first continuation is `0x21`, and
   the counter **wraps**, `0x2F → 0x20 → 0x21`. Ceiling: 113 bytes. The peer answers each
   frame on `id | 0x400` with `30 01 00` (ISO-TP flow control: CTS, BlockSize 1, STmin 0 —
   send exactly one more) or `0x74` (the declared length is satisfied). BS = 1 makes it
   *behave* like a per-frame ACK, which is why it was long mislabelled as one; parse the FS
   nibble rather than constant-matching all three bytes, but keep the one-frame-per-reply
   pacing — bursting desynchronises the panel.
4. **Treat "DONE while bytes remain" as SUCCESS.** A real panel stops at the declared FF_DL,
   not at the length you built. This is why `showMenu` is 13 frames on hardware and 14
   through a self-ACK emulator. A sender that calls that a short write breaks the menu on
   every single render.
5. **Never match on a received filler byte.** Padding is per-node: the bench panel pads
   `0xA3`, an OEM cluster `0x84`, the OEM radio `0xFF`. Only `data[0]` (done) and
   `data[0..2]` (partial) carry meaning.
6. **Guard short DLCs.** The sync channel really does carry DLC 1 and DLC 2 frames. Require
   `len >= 3` before reading `data[2]`, or your start flag latches off uninitialised stack.
7. **Key decode needs the `03 89` guard.** The key id also carries `70 A3..`,
   `02 64 0F A3..` and `05 63 "0037"`; without the guard you will invent keys `0x640F` and
   `0x3030` out of ordinary traffic.
8. **Compute the ACK id, never tabulate it.** `0x0A9 | 0x400` is `0x4A9`, not `0x5A9`.

The golden vectors in `docs/WIRE-SPEC.md` are ready to paste into a test, and each is tagged
with the strongest witness that attests it (`[CAP]` capture, `[REF]` third-party source,
`[TWIN]` decoder, `[CODE]` source only). Anything tagged `[CODE]` is a claim, not a fact —
those are the ones to verify on your own bench first.

### B.4 What you would lose

Not a sales pitch — a list of things that are easy to not notice you were getting:

* key-to-callback latency bounded at **one `poll()`** regardless of transmit backlog;
* latest-value-wins coalescing per render slot, which is what stops a fast render loop from
  queueing stale screens;
* a wall-clock sync watchdog rather than a call counter (the original defect);
* self-frame suppression at all three points that need it — auto-ACK, ACK matching, key
  decode — which is what makes a loopback test behave like hardware;
* 206 host tests that run in about fourteen seconds with no hardware.
