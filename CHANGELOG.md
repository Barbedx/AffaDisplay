# Changelog

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
