# Handoff — 2026-08-11, v2.0.0

For whoever takes this into implementation. Everything here is checkable; where it is not,
it says so.

---

## What this is

An ESP32 CAN driver for Renault **AFFA2 / AFFA3** dash panels. It speaks the protocol —
handshake, ISO-TP, registration, heartbeat, key decode and ACK, and the frame builders for
every screen the panel has. **It is not a UI.** Widgets were deleted in 2.0; what a gesture
means is yours.

```
pio test -e native                          31 cases, ~3 s, no hardware
pio run -e ex17_mediascreen -t upload       the console
node tools/gen_wire_doc.js --check          fails if docs/WIRE.md drifted
node tools/check_web_ui.js                  fails on broken console JS
```

Seven envs: `native`, `ex03_hello`, `ex17_mediascreen`, `ex17_mediascreen_c3`, `ex18_aiscreen`,
`ex19_cantest_c3`, `ex19_cantest_devkit`.

## The bench rig

**THE IP MOVES. Find the board by MAC, never by a remembered address.** On 2026-08-09 it
was `192.168.100.97`, STA MAC `ec:e3:34:b3:3c:54`. Sweep `arp -a` for the Espressif OUI.

| | |
|---|---|
| CAN | `rx = GPIO_NUM_5`, `tx = GPIO_NUM_4`, 500 kbit/s. **RX FIRST** in `begin()` — the usual trap |
| WiFi | SSID from NVS namespace `megaopen`, keys `ssid` / `pass`. 2.4 GHz only |
| no credentials | comes up as its own AP, `AffaMedia` / `affa1234`, `http://192.168.4.1/`, **OTA intact**. This is the recovery path, not a fault |
| change network | `/api/cmd?op=wifi&ssid=…&pass=…` — before that existed, moving networks meant a cable |
| OTA | `GET /ota/start?mode=fr` then `POST /ota/upload` multipart, or `/update` in a browser |

```
curl -s "http://<ip>/ota/start?mode=fr"
curl.exe -F "file=@.pio/build/ex17_mediascreen/firmware.bin" "http://<ip>/ota/upload"
```

**A reflash wipes `.pio/build` if `platformio.ini` changed.** I uploaded an absent binary
once and spent ten minutes wondering why the new op did not exist. Check the `.bin`
timestamp before blaming the board.

## Start here when something is wrong

**IF THE BOARD HEARS NOTHING, LEAVE THE PANEL OUT OF IT.** Flash `19_cantest` — no panel, no
protocol, no library unless you ask for it. In this order, and the order is the lesson:

1. **`mode=loop`** — the controller's RX input comes from its own TX pad through the GPIO
   matrix, so the frame never leaves the die. `matched` climbing clears the silicon, the bit
   timing and every line of the driver install at once; `matched` at zero means the fault is
   in the firmware and no amount of probing wires will find it. This is the only test here
   that can fail for exactly one reason.
2. **`mode=selftest`** — the same frame, but out through the transceiver and back. **Read its
   failures with care.** A zero here is NOT proof of a fault inside the board: on 2026-08-11
   both boards failed it on an idle pair, and the DevKit then worked perfectly the moment a
   real panel was on the bus. **An idle bus with no other powered node is not a valid
   environment for this test.**
3. **`mode=pingpong`** with the second board, then `op=layer&v=link` and repeat — that last
   switch is what clears `CanCommonLink` by comparison rather than by argument.

Only then bring the panel back. Every conclusion that had to be retracted on this bench was
measured across three moving variables at once — panel, bus and library — and the ones that
had to be retracted *this* session were measured on a bus with nothing alive on it.

Once the wire is not in question: `17_mediascreen`, **Health tab**. Two controls and four
groups of counters, in the order you read them when a panel is dark.

**RUN SELF-CHECK** — link → power → (1.2 s warm) → text → time, each waiting on **its own
ticket's completion**. A tick means the *panel answered*, not that a render was queued. That
distinction is the whole point: `Ok` from `setText()` means "queued", and every counter on
that page has been zero on a board whose glass never moved.

**gate TX off** — the thirty-second diagnostic. Gate our transmitter and watch the link
counters. If errors keep climbing with nothing of ours on the wire, the fault is not ours.
It has settled more arguments here than any other single control.

**Override** (Wire tab) — `load last TX` reassembles the last payload the builder produced,
including a 304-byte nav screen. Edit a byte, send it back, watch the glass. Step one byte at
a time; the diff line shouts when more than one moved, because **co-varying samples are not a
field** — three of them once produced a "one icon field" misreading that took a day to undo.

## The 2026-08-11 bench session — what `19_cantest` settled, and what it got wrong

A whole evening went into "the C3 never receives", the way an earlier one had. This time the
tool was built first, and the most useful thing it produced is the list of things it
**disproved** — including two of its own readings.

**Proven, and safe to build on:**

* **Both ESP32 controllers and all of `19_cantest`'s firmware are good.** `mode=loop` routes
  the controller's RX input to its own TX pad through the GPIO matrix, so the frame never
  leaves the die: C3 **246/246** matched, DevKit **176/176**, `seqGaps 0`, every error counter
  0, running continuously. Nothing outside the chip is in that path. Run it first, always.
* **Both transmit pads drive correctly** — `op=scan` reads every pin at once through
  `GPIO_IN_REG` and reports `gpio4=600` on each board (300 toggles, both edges).
* **The C3's transmit physically reaches the other node** — with the C3 toggling its D at
  10 kHz the DevKit counted **38 234 edges at 53% duty**. The bus pair carries dominants.
* **The DevKit works on the real panel.** With the panel back on the bus: a clean alternating
  `TX 3AF B9 …` / `RX 3CF 69 00 A3 …` every ~500 ms, `rx 125`, `tx 153` with only
  `txFail 11`. **A CAN frame only completes if something acknowledges it**, and the panel is
  the only other node — so its transmit reaches the wire and is ACKed.

**RETRACTED — do not repeat these:**

* **"The DevKit's transmit path is dead."** `xloop` reported `edges 0` for it repeatedly, and
  that was measured with the C3 off the bus and no panel. **An idle pair with no other powered
  node is not a valid environment for the D→R loopback test**, and a whole diagnosis was built
  on it before the panel went back on and the board simply worked. If a transmit test fails
  with nothing else alive on the bus, put something on the bus before concluding anything.
* **"An RTC pad is latched through the reboot."** A good theory — the latch does survive a
  software reset (an OTA) and not a power cycle, which fits an intermittent perfectly, and the
  failing halves lined up with the RTC-capable pins. `/api/state` now reports `rtcPad`: the C3
  says `false` for both, because that part has the hold facility but no RTC IO mux over those
  pads. `releasePads()` clears the latch anyway; it changed nothing on either board.
* **"The pins are swapped."** `op=swap` exchanges them at boot; tested both ways on both
  boards. They are not.

**Still open, and it is one thing:** the C3's receive line reaches the chip on **no pin at
all**. `boot edges 0`, `xloop rec 0`, `op=scan` → `NONE`, measured while the bus is
demonstrably carrying that same board's own traffic. Its transmit is fine. Everything
firmware-side is exhausted.

## What is proven, and what is not

Full record with evidence in `docs/BENCH-VERIFIED.md`.

**Seen on a real Carminat** — handshake and registration (ACK mean 1 287 µs), `setText`,
`setTime`, `setPower`, the two-row menu with selection, `showMenuN` with a pictogram and a
positioned scrollbar, popup, fullscreen animating at ~190 ms/screen, the 48 × 48 nav bitmap,
and an 8.3-hour soak: 257 k frames, zero ring overflows, flat heap. A further 4 h 15 m on
2026-08-09 at 326 k frames, unattended, after three deliberate wedges and recoveries.

**Seen on a real UpdateList panel** — handshake, `setPower`, normal and menu modes, selected
row inversion, scrolling text.

**Host-tested, never on glass** — Carminat's info popup and confirm box; UpdateList's icon
bytes and any row wider than 12 cells.

**Known not to work** — `3EF A6 hh mm` does not set an UpdateList clock: the bus takes the
frame, the clock does not move. UpdateList fullscreen concatenates into one ~19-character
line rather than stacking rows.

**Never run at all** — the whole cluster family, and **its opening cannot complete**: there
is no `61 11` in the one capture, so the hello is never triggered. Whether the trigger is the
panel's `69` or our own request is not decidable from one sample. `docs/NOTES.md` §1.1.

## Where silence is dangerous

2.0 deleted 165 behavioural tests, keeping only those that assert a golden wire vector. This
was a deliberate trade and it is the biggest risk in the repository.

**Nothing catches a regression in:** the transmit queue, the owned task, link recovery, the
dispatch ring, key decoding, or ISO-TP edge lengths. The §3b guarantees in `docs/API.md` —
one-poll key delivery, latest-value coalescing, `abortPending` reporting each dropped ticket,
`Priority::Urgent` never splitting a transfer — are implemented and documented but **not
enforced**. §3b.9 says so in those words.

**Never tested at any point:** `ILogSink`, `shouldAutoAck()`, `PanelGeometry` for the two
working families, `stats()`, and **`CanCommonLink` in its entirety** — the only code that
actually talks to hardware. `LoopbackLink` is exercised; the real link is not. If something
inexplicable happens on the bus, look there first, and expect no test to help.

## Traps that have each cost hours

* **`Ok` is not a rendered screen.** It happened three times with three different causes:
  rendering to a powered-off panel, rendering under a fullscreen, and a query parameter
  silently defaulting. Transport succeeded, counters looked healthy, glass did not move.
* **`rx 0` with zero errors fits three states** — a silent bus, a bus we cannot decode, and
  a controller that never started. Telling them apart needs `msgs_to_tx`. Without it a dead
  ESP32-C3 receive path looked exactly like a sleeping display for hours.
* **The panel acknowledges `setPower` before the glass is lit.** Text drawn inside that
  window goes into a display still coming up, and the usual conclusion is "setText does not
  work". The self-check waits 1.2 s for this reason.
* **PsychicHttp `lru_purge_enable` is a one-way door.** Without it a full socket table is
  permanent: httpd stops accepting and never resumes — ping answers, mDNS answers, `/update`
  is gone. It is set in `examples/shared/net.h` with the story attached.
* **`max_uri_handlers` overflow is silent.** One extra route once unregistered `/ota/upload`
  and the board needed a cable. OTA is registered *first* for that reason.
* **A held-low TXD measures nothing.** The transceiver disarms its own driver after ~1–4 ms
  so one stuck node cannot jam a bus, so a probe that drives D low and then samples reads a
  healthy part as dead. It invalidated a whole session's verdict once (`docs/NOTES.md`) and
  then invalidated `19_cantest`'s first probe too. **Toggle at ~10 kHz and count edges**;
  keep the recessive half static, since no driver is requested there.
* **A busy-spin in an HTTP handler takes the board off the network.** `delayMicroseconds()`
  never yields; `delay()` does. A 4 s spin inside the web server's task starved the network
  stack, stale sockets filled the seven slots against a 3 s timeout, and the DevKit stopped
  answering HTTP **while still replying to ping and still completing TCP handshakes** — lwIP
  does those itself and the connection sits in an accept queue nobody drains. A power cycle
  did not clear it; an EN reset over the serial cable did. `17_mediascreen`'s `op=wiggle`
  spins a pin for a full minute and has never wedged anything, because it uses `delay(500)`.
* **Flash pin numbers are not the same part to part.** GPIO6–11 on the classic ESP32,
  GPIO11–17 on the C3 (plus 18/19 for USB). A pin list written for one and run on the other
  reconfigured six live SPI-flash pins on a running board.
* **An upload after a failed build flashes the previous binary and reports success.** Two
  boards were flashed with stale images and answered with the old firmware's fields. Check
  that the build succeeded, not just that the upload said `OK`.
* **The DevKit wedges on OTA repeatedly** (upload returns an empty body, then no HTTP). USB on
  `COM5` worked first time, every time: `pio run -e <env> -t upload --upload-port COM5`.
  Unexplained; the serial route is the reliable one on that board.
* **PowerShell `Set-Content -Encoding utf8` writes a BOM** and corrupts `platformio.ini`; the
  error blames the wrong line. Use the editor tools.
* **`perl -0pi -e` multi-line patterns silently do nothing on CRLF files.** Half this
  session's edits failed that way before I switched to line-anchored patterns or the editor.
  If a substitution reports success and changes nothing, that is why.

## Open questions

0. **The C3's receive line does not reach the chip.** Its transmit is fine and the other node
   receives it; nothing on the C3 hears anything, on any pin, ever. Every firmware avenue is
   closed (see the session section above). What has NOT been done is the one physical split:
   measure the transceiver's own R pin **at the module**, powered, bus idle — ~3.3 V means the
   part is fine and the wire to the chip is open, 0 V or floating means the receiver output is
   dead. Swapping the two transceiver modules between the boards answers the same question
   without a meter.
1. **Registration does not complete on the DevKit + panel.** The panel pings `69 00 A3 …`
   every ~500 ms and gets its `B9`, but never sends `61 11`, so the session never opens and
   `registered` stays false; after ~228 s the panel went quiet. `rxErr` sits at 128–129
   (error-passive) throughout. Not diagnosed. Note the panel sleeps after a board reset, so
   power-cycle it before reading anything into this.
2. **The cluster's opening.** Needs a cluster on a bench, or an owner's ruling on the
   trigger. Nothing guards it now — the marker test went with the behavioural suites.
3. **Ten of the fourteen nav-header bytes** are unmeasured. `[4..10]` held `"ABCDEF\0"` in
   the capture and are confirmed **not** to be text. The override editor sweeps them; the
   oracle is a person looking at glass, because the panel ACKs a screen it never lights.
4. **The `0x7F` text-plus-icons flavour** is documented and not emitted. If icons are
   implemented, use the captured bytes — our reconstruction disagreed with the only real
   capture in two places.

## The map

```
src/            8 437   the library. core/ knows no panel; carminat/ updatelist/ cluster/ are gated
test/           1 934   three suites, all of them wire vectors
docs/WIRE.md      GEN   from those vectors, by tools/gen_wire_doc.js  (--check guards it)
docs/API.md            contracts: threading, Result, latency, capabilities. Copies no declarations
docs/NOTES.md          what we do not know; how this project has got things wrong eight times
docs/BENCH-VERIFIED.md what a human saw on glass
docs/captures/         the OEM corpus — third-party recordings we cannot reproduce. Keep.
examples/shared/       net.h (WiFi/OTA/HTTP, once), media_render.h, nav_images.h
```

**Read `docs/NOTES.md` §2 before changing the protocol.** Eight entries, and seven of them
are the same error: a special case standing in for a general rule. The eighth is a guard
whose two terms did not cover the middle. They are recorded because this project keeps
making them.
