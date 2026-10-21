# Changelog

## 1.3.0 — 2026-10-21

### `showInfoRow`: one info row on its own

`showInfoRow(text, offset = kInfoOffset0, prefix = kInfoPrefix, cells = kInfoCells)` sends one row
of the info screen as one message, with the same cells and padding rules as `showInfoMenu`.
`showInfoMenu` now builds its three rows with it; its bytes are unchanged.

- For redrawing a single row, or a glass sweep that must put exactly one row on the glass.
- A new host test: the three OEM rows sent one at a time match the OEM list's frames byte for
  byte, and 0 or 17 cells are refused. 79 host tests in all.

## 1.2.0 — 2026-10-21

### Info rows: the cell count is a parameter

An API addition, ready for a glass sweep of the info box's width. Nothing on the wire changes
unless you pass the new argument.

- `showInfoMenu(..., uint8_t cells = 8)`: text cells per row, 1 to `carminat::kInfoCellsMax`
  (16). The declared length is 3 + cells, so the default 8 sends today's `10 0B 76 ...` bytes
  exactly. Text is space-padded or cut to `cells`. Out of range: `BadArgument`. Only 8 has been
  seen on glass.
- `PanelGeometry::infoCells`: the cells `showInfoMenu()` sends by default (8 on Carminat, 0
  elsewhere). `infoRowChars` stays the width the glass shows.
- Four new host tests:
  - the default and an explicit 8 match the OEM frames byte for byte;
  - 9 to 16 cells declare 3 + n and send nothing but the filler past it;
  - 0 and 17 are refused;
  - the geometry says 8.
  78 host tests in all.

## 1.1.2 — 2026-10-21

### Fixed: `Priority::Urgent` jobs keep their order

docs/API.md §3.4 says urgent jobs are FIFO among themselves, but a new urgent job was put
ahead of the urgent jobs already queued, so two of them went out in reverse order. A new
urgent job now goes after the job in flight, the queued registrations and the queued urgent
jobs, and still ahead of every `Normal` job.

- `setPower(true)` then `setPower(false)` before either starts still coalesce: one frame, the
  OFF, and the first ticket completes `Aborted`. Now pinned by a test.
- Three new host tests (urgent order, registration still first, the two `setPower` calls).
  74 host tests in all.

### Docs and comments now match the code

- API.md:
  - `TooLong` is only for payload sizes; text longer than its field is cut, not refused.
  - A held render whose hold runs out completes `NoSync` or `LinkDown`.
  - The `poll()` order includes `pumpLink()` and the Ready edge.
  - The `Highlight` slot also covers `selectMenuItem` and `selectBoxButton`.
  - `setPower` is queued `Urgent` and replayed after a session reset.
  - `AffaTask::stop()` ends unstarted renders `Aborted`.
  - `AFFA_UNSAFE_LONG_PAYLOAD` is listed.
  - `pressKey`: `Wire` only transmits, `Both` does both, and `Local` with no `onKey` is
    `NotSupported`.
  - The cluster has `KeyTx` on `0x1C1`.
- README: the cluster's data ids are `0x121` and `0x1B1`; its keys are `0x1C1`.
- NOTES.md: three `queueHello()` call sites, not two; no example sweeps the nav header yet.
- WIRE.md: the confirm box's last PCI 0x2F is not a counter ceiling.
- Source comments: `SendFailed` is not retried, an expired hold is `NoSync`/`LinkDown`,
  `stop()` gives `Aborted`, `ICanLink::trySend()`. Names of examples that are not in this
  repository are gone.

## 1.1.1 — 2026-10-21

Documentation and examples only. The library code is unchanged.

- **README:** the minimal example compiles as written (`#include <Arduino.h>`, and it uses the
  `[[nodiscard]]` result of `setText`). CI now builds it as a user's project on
  `espressif32@6.13.0` and `8.0.0`, and fails on any warning in it.
- **README:** a Wiring section (transceiver, 500 kbit/s, termination, listen-only before a car
  bus), the tested platforms, and a plain note that the library is not in the PlatformIO
  registry yet.
- **`03_hello`:** pins come from `AFFA_CAN_RX` / `AFFA_CAN_TX` (default 5 / 4, as in the other
  examples; the `ex03_hello` env sets the C3's 3 / 4). A failed render restarts the sequence
  instead of stopping it.
- **Example dependencies pinned:** `hoeken/PsychicHttp@1.2.1`, ElegantOTA at
  `e2c2aee15e9f71ed35415cc02f0b10b53b2c195c` (the `psychic-http` branch).
- **Build noise:** `-Wundef` is on for the host build only. On ESP32 the framework headers
  printed about 1200 `-Wundef` warnings that were not ours. Warnings in `src/` and `examples/`
  still fail CI.
- Every file in `src/` and `examples/` has an `SPDX-License-Identifier: MIT` line.
- Stale comments in `platformio.ini`, `examples/README.md` and `03_hello` no longer point at
  examples and tools that are not in this repository.

## 1.1.0 — 2026-10-21

### `showFullscreenText` uses the OEM geometry

The fullscreen now matches the bytes MegaOpen sends to the car's panel (its `sendFullscreen`,
camera-measured):
- declared length 0x69, header `21 05 FF 00 00 49` (byte 5 = declared - 0x20);
- rows start at content byte 32 with no leading spaces, each ended by 0x0D;
- the 73-byte body is space-filled.

Before this, the header was 0x60 / 0x40 with two leading spaces: row 1 was indented and row 3
ran out of room. The API is unchanged.

- `panelGeometry().fullRowChars` is 22 on Carminat (was 26), the measured width in wide
  characters.
- Three new host tests pin the bytes against MegaOpen's builder (two rows, three rows, a full
  body), and one pins the geometry. 71 host tests in all.
- UpdateList and the cluster have no fullscreen; nothing changes there.

### Not changed

- Info rows (`showInfoMenu`) stay 8 cells (`kInfoCells`, declared 0x0B). The glass box looks
  wider, but no wider width has been measured; that needs a bench sweep of the declared length
  and offsets first.

## 1.0.0 — 2026-10-21

First public release.

### What is in it

- **Panels:** Carminat / AFFA3 (`CarminatDisplay`) and UpdateList / AFFA2 (`UpdateListDisplay`).
  An instrument-cluster family (`ClusterDisplay`) is also included, built only with
  `AFFA_PANEL_CLUSTER=1`; it has never run on hardware.
- **Session:** the opening handshake, function registration, heartbeat and peer watchdog. All
  timers are wall-clock deadlines inside `poll()`, and none of them blocks.
  - A panel that never opens its `1C1` channel still registers, after `AFFA_AWAIT_PEER_MS`
    (400 ms).
  - A repeated `61 11` before registration draws a fresh hello burst.
- **Transmit:** ISO-TP segmentation with flow control, and retries with backoff. Renders are
  held while there is no session and re-registered after a resync. Repeated renders of one slot
  coalesce, `Priority::Urgent` goes first, and `abortPending()` / `abortAll()` cancel queued
  work. Every render returns `Submitted{ticket, result}`.
- **Threading:** with `AFFA_ENABLE_TASK=1` (the ESP32 default), `affa::rtos::AffaTask` owns
  `poll()`, and renders, `pressKey()`, the aborts and `begin()` are safe from any task through a
  bounded dispatch ring. Long payloads (the 48 × 48 image, lists of up to 10 items) are copied
  into a pool of `AFFA_BULK_SLOTS` × `AFFA_MAX_EXTERNAL_PAYLOAD` (2 × 320 B).
- **Link:** `affa::TwaiLink` on the ESP-IDF TWAI driver, with no third-party CAN library.
  - Configured through `TwaiConfig`: pins, bitrate, listen-only, queue depths (RX 64).
  - A full TX queue is reported as `Busy`, a controller that cannot send as `Rejected`.
  - Recovers from bus-off.
  - `TxGate` stops transmitting to a bus nobody ACKs within 2 s and reopens on the first frame
    heard.
  - `waitRx()` lets the owner task sleep until a frame arrives, paced by `WakePacer`.
  - `poll()` reads at most `AFFA_RX_BUDGET` (64) frames, so a retransmit storm cannot starve
    the sync pump.
- **Link only:** `<AffaTwai.h>` gives `TwaiLink` and `affa::setLogSink()` with no panel compiled
  in (`examples/04_sniffer`).
- **Your own driver:** implement `affa::ICanLink` (`trySend`, `recv`, and optionally `isLive`,
  `service`, `waitRx`).
- **Tests:** 68 host tests (`pio test -e native`), no hardware needed. `docs/WIRE.md` is generated
  from their golden vectors.
- **CI:** GitHub Actions runs the host tests, builds every example and fails on a warning from
  `src/` or `examples/`. A `vX.Y.Z` tag that matches `library.json` and `library.properties`
  publishes a release with this file's section as its notes.

### Known gaps

- This release has been built and host-tested, but this tree has not yet driven a panel.
  `TwaiLink` comes from MegaOpen, where it ran in the car.
- UpdateList still uses its older opening (lazy registration, no auth gate). Setting its clock
  does not work.
- The cluster family cannot complete its opening: the one capture it comes from has no `61 11`.
- Carminat's info popup and confirm box have been built but not yet seen on glass.
- `abortAll()` leaves the panel holding a partial transfer. Whether the panel recovers from that
  has not been verified.
- `AffaTask` has no after-poll hook. An owner that must run code after every `poll()` keeps its
  own loop with `waitRx()` and `WakePacer`.
- The ordering, coalescing and preemption rules in `docs/API.md` §3 are implemented, but they do
  not have tests of their own.
- The library and its examples are built against `espressif32@6.13.0` (Arduino-ESP32 2.0.17).
  Newer platforms (7.x, 8.x with Arduino 3) are untested.
