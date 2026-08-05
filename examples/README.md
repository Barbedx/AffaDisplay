# Examples — which one to read, and which ones are history

**If you are starting a project: copy [`09_golden`](09_golden/main.cpp). Read nothing else first.**

Several examples here predate the protocol being settled (2026-08-04, see
[`../docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md`](../docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md)).
They are kept because they are **evidence** — each one is a measurement that was taken on real
hardware, and several are the reason the protocol is now known. But some of them implement the
handshake as we *believed* it worked, and copying those would reproduce beliefs the captures
have since disproven. That is what this file exists to prevent.

---

## Start here

| example | board | stack | what it is |
|---|---|---|---|
| **`09_golden`** | ESP32 DevKit V1 | **library** over `can_common` | **THE ONE TO COPY.** Three scrolling rows, web console (pause, per-row text/speed, clock), downloadable wire log, WiFi manager, OTA. Contains **no protocol at all** — the library owns it. |
| `03_hello` | ESP32-C3 | library over ESP-IDF TWAI | The smallest correct program: power on, `SUCCESS`, clock. ~170 lines, no WiFi, cable-flashed. Read this to understand the API shape. |
| `04_rows` | ESP32-C3 | library over ESP-IDF TWAI | Live three-row screen with a console. The TWAI-stack counterpart to `09_golden`. |

## The other panel family

| example | board | what it is |
|---|---|---|
| **`10_updatelist`** | DevKit V1 | **The AFFA2 / UpdateList example, and the only one.** A first-contact diagnostic: serial only, every frame in both directions, every phase transition, queue depths twice a second, and a 15-second verdict that names the frame that never arrived. After the proof frame the panel cycles random phrases, which turns it into a continuous soak of the ISO-TP path. **This is the file that proved the family on hardware** — 2026-08-04, first attempt, opening to `SUCCESS` in 220 ms of wire time. Its header lists the failure shapes and which knob each implicates; that table was written *before* the run and kept. |

## Fun, and harder soaks than they look

Each of these is a continuous full-rate load as well as something to look at. A marquee only
repaints when the visible text moves; these change **every frame**, so nothing is skipped by
repaint-on-change and the bus is exercised at a steady ~6–8 fullscreens a second — the rate
the 1 h 36 m soak ran at. All three take live settings from a web form and persist them.

| example | board | what it is |
|---|---|---|
| `17_mediascreen` | DevKit V1 | **THE PRESENTATION — run this to see what the library does.** A live 48×48 in the `0x1F1` nav pane while `setText` marquees a track title on the main line, plus *every* render call behind buttons, sixteen scenes (seven drawn on the device from a frame counter — spectrum, VU needle, waveform, analogue clock, starfield, bouncing trail, rings — and nine from the generated set), a marquee that drives the main line or the info rows, and the panel family selectable at boot. It needs all three of the 2026-08-05 bench results: the pane renders, it is an *independent layer* rather than a screen mode, and the OEM animates the channel itself. Measured: **64 ms an image, 24% bus duty at 4 fps, 158/158 frames with zero errors.** The console reports duty and dropped frames so the frame-rate trade is visible rather than guessed. |
| `14_demoreel` | DevKit V1 | **The one to show people.** Six effects, eight seconds each: a Knight Rider scanner, fire, a sine-bobbed scroller, shockwave rings, a bouncing ball with a trail, falling rain. Every effect has a jump link on the console and the console mirrors the panel. |
| `11_boom` | DevKit V1 | A T-10 countdown with a burning fuse, then a nine-frame three-row ASCII explosion, then again. The fuse is *scaled* to the row, so the bar and the number cannot disagree whatever the countdown is set to. |
| `13_starfield` | DevKit V1 | Perspective 3D, procedural rather than a frame table: each star carries a direction and a depth, and every frame is an integer perspective division. The depth cue is the **rate**, not the shape — which is why it works at 20×3 where a wireframe cube would resolve to a blob. Deliberately subtle; `14_demoreel` is the loud one. |

## Protocol experiments

| example | what it asks |
|---|---|
| `12_ulclock` | **Which frame sets the clock on an AFFA2 panel?** It fires numbered candidates with the panel showing which one is live, so a human watching the glass can name the winner. The answer so far is *none of them* — 23 candidates, 162 probes, no effect (`../docs/BENCH-VERIFIED.md`). Kept as the shape to copy for the next protocol question, and for the finding that came out of it: **this panel ACKs everything**, so an ACK proves nothing and only the glass counts. |
| `16_navlab` | **What are the fourteen header bytes in front of the `0x1F1` nav bitmap?** The OEM's 302-byte nav screen decodes as a 14-byte header plus a 48×48 monochrome bitmap, and the bitmap *is* the idle globe (`../docs/PROTOCOL-NOTES.md` §18). This replays it byte for byte, makes every byte editable from a browser canvas, ships alternative images (Renault losange, tryzub, tryzub+clock, thermometer, battery, and an orientation checker), renders arbitrary text through the browser's own font, and **sweeps one header byte across a range**, one send per step, so an unknown field is characterised in a single pass instead of a rebuild per guess. First consumer of `enqueueExternal()`. **The bitmap renders** (bench 2026-08-05) and the pane turns out to be an *independent layer* — the info menu draws with it, and the image can be replaced under an open popup and is seen to change. Still open: the caption slot at `[3..9]`, and whether bytes 12/13 are honoured as real geometry. `12_ulclock`'s lesson still applies to everything unconfirmed: this panel ACKs everything, so only the glass counts. |

## Diagnostics — reach for these when something is wrong

| example | what it answers |
|---|---|
| `01_bringup` | Does the link come up at all, in the order it has to be proved? |
| `02_canspy` | Is the ESP32 seeing CAN frames *at all*? Strips the library out entirely (`build_src_filter = -<*>`), so a result here is a statement about the driver and the wire with AffaDisplay removed from the equation. |
| `10_updatelist` | Is this panel an AFFA2 panel, and if the opening stalls, **where**? It prints the phase on every change, and a phase that will not advance names the missing frame. |

## The protocol by hand — correct, but not the way to build an application

These implement the wire directly, without the library. They are **current** and were proven on
glass, so they are trustworthy references for the protocol itself. They are also ~700 lines
each where `09_golden` is ~685 including a whole web console — which is the argument for the
library, not against these.

| example | board | what it proved |
|---|---|---|
| `07_cantime` | DevKit V1 | Handshake + clock on `can_common`. **This is the file that first put 10:00 on the glass from a cold bus**, and where the BA-before-hello rule was found. |
| `08_rows3` | DevKit V1 | Adds ISO-TP multi-frame transmit with BS=1 flow control. Where the `0x21`-not-`0x20` consecutive-frame bug was found. |

## History — DO NOT COPY

These encode the protocol as it was understood **before** the OEM captures settled it. They are
kept as the record of how it was worked out, and because each carries measurements that are
still cited. Read them for the reasoning; do not build on them.

| example | what it still gets wrong |
|---|---|
| `05_pingpong` | *"Answers ANY 69 ping with a paced B9 pong"* — **B9 is not a pong.** Its period is 500.08 ms (σ 0.33) while the display's `69` is 507.83 ms (σ 4.60); the gap between them slides monotonically and wraps past zero. A reply cannot be 14× more stable than its trigger. |
| `06_authclock` | Built around `61 11 00` being the only authorizing request, with `01` behind a compatibility flag. **`61 11 00` and `61 11 01` are the same request** — one capture completes an entire session on `01` with zero `00` frames. It also answers the *first* request with the hello burst, where the OEM answers with `BA` and lets the *next* request draw the burst. |

Both also predate three rules that are now load-bearing: the display's `1C1 70` must be
acknowledged and must precede our own registration; `03 52 09` (display ON) must be sent before
anything is drawn, or the panel ACKs a screen it never lights; and any `61 11` arriving while
registered means the panel has voided us and application traffic must stop.

---

## Building

```
pio run -e ex09_golden -t upload --upload-port COM5     # DevKit V1, CRX=GPIO5 CTX=GPIO4
pio run -e ex03_hello  -t upload                        # ESP32-C3,  CRX=GPIO3 CTX=GPIO4
```

Every example after the first flash is reachable over OTA at `http://<ip>/update`, except
`03_hello`, which deliberately has no WiFi so that it stays the smallest correct program.

**Pin names are the transceiver's.** CRX is the transceiver's `R` output and must reach the
MCU's CAN **RX**. Swapping them produces a link that never errors and never receives — which
reads as a dead display rather than as miswiring, and cost a full bench session.
