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

## Diagnostics — reach for these when something is wrong

| example | what it answers |
|---|---|
| `01_bringup` | Does the link come up at all, in the order it has to be proved? |
| `02_canspy` | Is the ESP32 seeing CAN frames *at all*? Strips the library out entirely (`build_src_filter = -<*>`), so a result here is a statement about the driver and the wire with AffaDisplay removed from the equation. |

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
