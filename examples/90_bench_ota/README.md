# 90_bench_ota — the interactive web console

The harness that proves the library on real hardware, and a standing requirement in its own
right: **every public capability must be reachable and observable from a browser**, because
the rig has no serial cable and no physical buttons (they are in the car).

```
pio run -e ex90_bench_ota            build
pio run -e ex90_bench_ota -t upload  FIRST flash only, over USB at 115200
```
Thereafter the board is updated at **`http://<ip>/update`** and never over a cable again.

---

## IT WORKS WITH NO TRANSCEIVER AND NO PANEL ATTACHED

`GET /api/mode?panel=virtual` puts a `CarminatVirtualPanel` — the library's own hardware
faithful panel twin — on the other end of the wire and gates CAN TX off. The handshake, the
lazy `0x70` function registration, the ISO-TP fragmentation, the per-frame ACKs, the key
decode and the screen decode are all real; the only thing that is not real is the wire.

**If `Esp32CanLink::begin()` fails at boot the console switches itself to virtual mode and
says so.** You can develop the whole page, every endpoint and every render on a bare C3 with
nothing soldered to GPIO3/GPIO4.

`GET /api/mode?panel=real` switches back. In real mode the twin stays on, in **PASSIVE**
mode, fed from the same Layer-0 TX tap — so `/api/screen` still reports the decoded screen
with a live panel on the bus, and the page renders what the panel is showing next to the
frames that produced it. Passive means it transmits nothing: two ACKers on one bus is the
failure that switch exists to prevent.

The twin runs `AckMode::Declared`, not the `Done` default. Declared answers PARTIAL while
the declared FF_DL is unsatisfied and DONE at it, which is what the hardware does — it is
the only mode that reproduces `showMenu` at **13** frames (last PCI `0x2C`) rather than the
self-ACK emulator's 14.

---

## Where the boundary sits

**This example contains no menu logic and no key mapping of its own.** It translates HTTP
into `nav()`, `pressKey()` and render calls, and nothing else:

* `/api/nav?c=next` is one line: `display.nav(NavCommand::Next)`. The console does not know
  which row is selected, when a redraw is needed rather than a re-highlight, or what a Hold
  edge does to a field — all of that is reasoning about the wire and lives in the library.
* `/api/key?k=…` is `display.pressKey(k, edge, src)`. The console does not decode, mask or
  interpret a key code; it passes the wire value through.
* The rendered menu on the page is **the twin's decoded `ScreenModel`**, not a local model.
  The page maps the scroll byte to arrows and the row tag to a highlight, which is
  presentation of a value the twin already decoded.

If you find yourself writing navigation logic here, the library is missing an API — add it
there.

---

## Safety, and why each line of it exists

| | |
| --- | --- |
| **WiFi + OTA come up first** | `setup()` brings up the network and the HTTP server *before* CAN and before the library. Everything after that point may fail and still leave a way in. |
| **Credentials** | Read-only from NVS namespace `"megaopen"`, string keys `"ssid"` / `"pass"` — verified against `MegaOpen/src/cfg/Config.cpp`. Read-only matters twice: this example must never be the reason those credentials change, and an NVS **write** stops CAN reception outright. |
| **SoftAP fallback** | `AffaBench` / `affabench` if the station join fails within 15 s. Also `http://affabench.local`. |
| **Partitions** | `partitions_ota.csv`, copied verbatim from MegaOpen: two 1.4 MB OTA slots. The default table's 1.25 MB app slot does not hold this image, and a single-slot table makes a bad flash unrecoverable without a cable. |
| **`upload_speed = 115200`** | The native-USB C3 drops large images mid-write above this on a car rig. |
| **`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`** | Without them the C3's USB CDC never enumerates and the only diagnostic channel left is the network you are trying to bring up. |
| **`if (Serial)` around every mirror write** | With no USB host attached, `HWCDC::write` fills its ring and then blocks per write. At `AFFA_LOG_LEVEL=5` on a live bus that is a stall per frame, in the one loop that must not stall, on a board with no cable. `Serial.setTxTimeoutMs(0)` is the second belt. |
| **`delay(1)` at the end of `loop()`** | `loopTask` is FreeRTOS priority 1 and IDLE is 0, so a loop that never blocks starves IDLE and the single-core C3 panics with *Task watchdog got triggered (IDLE)* — a reboot loop on a board whose only way back in is the network the reboot loop never brings up. It is application policy; the **library** still never sleeps. It bounds L1 at ~1 ms, better than the 5 ms `docs/API.md` §3b assumes, and `/api/status` reports the measured worst period rather than asking you to take it on trust. |

### AFFA_PEER_TIMEOUT_MS and OTA

`AFFA_PEER_TIMEOUT_MS` (default 5000) **must stay above the longest flash write the
application performs.** The TWAI ISR is not in IRAM, so a flash write stops reception
outright and looks exactly like a panel that went quiet. During an OTA upload nothing is
received at all, so expect `PeerLost`, a torn-down sync and a resync afterwards — this is
correct behaviour, not a defect.

`ElegantOTA.onStart` shuts the software TX gate for the duration, so the board is not
shouting at a bus it cannot hear while it flashes itself.

---

## Threading — read this before adding an endpoint

The library is per-instance and unlocked: **exactly one task may drive it**, and that task
is `loop()`. HTTP handlers run in the `esp_http_server` task and never touch the display,
the twin, the rings or the JSON buffer. They fill a `Cmd`, post it into a one-slot
mutex-protected mailbox, and block until `loop()` has executed it — typically well under a
millisecond. The HTTP task is allowed to sleep; the loop task is not.

That is also why every response is exact rather than optimistic: the ticket in
`{"ticket":N,"enqueued":"Ok"}` is the ticket `enqueue()` actually issued, not a guess.

Adding an endpoint means: a new `Op`, a case in `execCmd()` (runs on the loop task), and a
route that parses parameters and calls `run()`. Never call into `affa::` from a handler.

---

## The HTTP API

Everything is `GET` and everything answers JSON, so every function is testable from a URL
bar with no tooling.

Every **render** endpoint answers `{"ticket":N,"enqueued":"Ok"}` **immediately**. The
delivered outcome appears later in `/api/status` (`lastDelivered`) and in the log ring —
that IS the non-blocking contract made visible. A capability that is gated off answers
`{"error":"NotSupported"}` and **not 404**, so the console doubles as a capability probe:
every endpoint exists on every build.

### Observation
| | |
| --- | --- |
| `/` | the console page |
| `/api/status` | sync state, registered, busy, queue depth, last enqueue Result + ticket, last delivered Result + ticket, every link counter, menu state, uptime, heap, WiFi mode/IP/RSSI, twin counters, measured latencies |
| `/api/frames?n=` | the last N of 128 frames: `[ms, dir(1=Rx,2=Tx), id, "hex"]` |
| `/api/log?n=` | the log ring, fed by an `ILogSink` installed into the library and mirrored to Serial |
| `/api/keys` | the last 32 key events with timestamp, code, name, hold and **source** |
| `/api/screen` | the twin's decoded `ScreenModel` — mode, header, row0, row1, row tags, `sel`, scroll, info rows |
| `/api/menu/state` | every item, every field, its kind, range, step, multiplier and list |
| `/api/selftest` | starts the run and returns the report (see below) |

### Render
| | |
| --- | --- |
| `/api/text?t=&d=` | `setText(t, digit)` |
| `/api/time?hhmm=` | `setTime` |
| `/api/state?on=0\|1` | `setPower` |
| `/api/menu?h=&a=&b=&s=` | `showMenu(header, row0, row1, scroll)` — `s` accepts `0`, `7`, `11`, `12` or `0x0B` |
| `/api/highlight?row=` | `highlightItem` |
| `/api/popup?t=&icon=&src=&fmt=` | `showPopupText`, all three header bytes editable |
| `/api/popup/hide` | `hidePopup` |
| `/api/fullscreen?l1=&l2=&l3=` , `/api/fullscreen/hide` | |
| `/api/confirm?cap=&r1=&r2=` | `showConfirmBox`. Sits at exactly the 113-byte ceiling; a caption longer than 6 characters writes into row 1. |
| `/api/info?l1=&l2=&l3=` , `/api/info/hide` | three separate messages, one per row |
| `/api/menu/show` | opens the menu if closed, otherwise re-renders it |

### Input
| | |
| --- | --- |
| `/api/nav?c=open\|next\|prev\|select\|back\|inc\|dec` | `nav(NavCommand)` |
| `/api/key?k=<code>&hold=0\|1&src=local\|wire\|both` | `pressKey(...)`. `k` is the wire code, decimal or `0x…` |

`src` is the whole point of the Keys panel. **`local` drives our menu and puts nothing on
the bus; `wire` puts `03 89 <hi> <lo> 00 00 00 00` on `0x1C1` and has no local effect.**
Press one of each and watch the frame ring: that is the clearest demonstration of the seam
in the library.

### Control
| | |
| --- | --- |
| `/api/counter?run=1&hz=10&to=1000` | the `docs/API.md` §3b.6 scenario from the browser. `hz` = render rate, `to` = run for that many ms (`0` = until stopped). `run=0` stops it. |
| `/api/abort` | `abortPending()`, and it returns **how many were dropped** |
| `/api/txgate?on=0\|1` | the **software** TX gate. Never a driver mode change: it makes `send()` return false, and the controller keeps ACKing other nodes at the link layer — which on a two-node bus is required. |
| `/api/mode?panel=real\|virtual` | see above. Re-runs `begin()`, so queued tickets complete `Cancelled`. |
| `/api/selftest` | wait for sync → registration → `setText("AFFA OK")` → read back the **delivered** Result |
| `/api/reboot` | replies first, reboots 400 ms later |
| `/update`, `/ota/start`, `/ota/upload` | ElegantOTA |

### On boot

Once sync is established the console attempts `setText("AFFA OK")` **exactly once** and
records the outcome, so the goal is verifiable with a single GET:

```
curl http://192.168.100.85/api/selftest
```

---

## What is measured, and what is not

`/api/status` → `lat`:

| field | what it actually is |
| --- | --- |
| `keyToCbUs` | the `0x1C1` frame reaching the Layer-0 tap (or the injection being issued) → `KeyCb`. It does **not** include the RX ring residency, which no observer inside the firmware can see. |
| `keyToWireUs` | `KeyCb` → the first data frame it caused. This is L2 minus L1. |
| `pollMaxUs` | the worst `loop()` period observed. `docs/API.md` §3b.3 states L1 is **exactly one poll period**, so this is the measured L1 bound. |
| `staleDropped` | completions with `Result::Aborted` — superseded renders that never reached the wire. Start the counter at 20 Hz and watch it climb: that is coalescing working. |
| `ackMinUs` / `ackMeanUs` / `ackMaxUs` / `ackN` | **the panel's ACK turnaround.** `docs/API.md` §3b.8 names this as the one term the library cannot compute and asks for the measured Carminat figure to be recorded before v0.1.0 is tagged. This is where that number comes from — run a `showMenu` and read it off. A figure measured against the twin is not that number; take it in `real` mode with a panel. |

The counters reset on a `/api/mode` switch, so a virtual-mode figure cannot silently be
mistaken for a hardware one.

---

## The demo menu

Three items, exercising all three field kinds, one of them with three fields:

| item | fields |
| --- | --- |
| `Bright` | integer 0–100, step 5, ×4 on a Hold edge (`inc`/`dec`), unit `%` |
| `Mode` | list `Off` / `Auto` / `On` |
| `Time` | `h` integer 0–23, `m` integer 0–59, and a **read-only** integer — still an integer field, and `Select` skips over it |

Every `onChange` logs to the ring, so a change made from the D-pad, from a real wheel
detent, or from `/api/nav?c=inc` is visible in `/api/log` within a second. The content is
the application's; nothing about *how* a field is edited lives in this file.

A held rotary detent has no wire representation (`0x0101|0xC0` and `0x0141|0xC0` are both
`0x01C1`), so `inc`/`dec` — the coarse `stepMultiplier` step — is reachable only through
`nav()`/`pressKey()` with a source that includes `Local`. Pressing `inc` on the D-pad works;
holding the real wheel does not, and that is by design, not a gap.

## Footprint

`ex90_bench_ota`, ESP32-C3, `partitions_ota.csv` (1.4 MB app slot):

```
RAM:   21.7%  (71 124 of 327 680 bytes)
Flash: 62.3%  (898 242 of 1 441 792 bytes)   firmware.bin on disk: 910 512 bytes
```

Most of that is WiFi, esp_http_server and ElegantOTA. Compare `size_carminat`, which is the
same library with an example that has no network stack.
