# Bench handoff — 2026-07-29

Panel powered off by the owner. Board at **192.168.100.85** runs `examples/01_bringup`,
OTA at `/update`. Goal not reached: **SUCCESS never appeared on the glass.**

## Read this first

Our node in **NORMAL** mode emits CAN error frames whenever it detects an error — so while
it was in normal mode **we were actively corrupting the bus**. Any normal-mode number below
describes a bus we were disturbing. **Only the listen-only observations are clean.** If you
re-run anything, start in listen-only.

## Proven working (measured, listen-only)

| | evidence |
|---|---|
| Panel alive, correct protocol | `3CF 69 00 A3 A3 A3 A3 A3` peer-alive, and `3CF 61 11 01 A3 A3 A3 A3 A3` sync-request-with-Start. Filler `0xA3`, exactly per spec. |
| We decode it perfectly | ~1500 frames/s, **rxErr 0, busErr 0**, sustained. 14 960 frames in 10 s. |
| Bitrate | `beginAutoSpeed()` rejects 1M, locks **500000**. |
| ESP32 internals | Internal loopback (TX+RX on one pad, NO_ACK + self-reception) **passes first try** on GPIO3, GPIO4 and GPIO6. |
| Pins | `setCANPins(rx=GPIO4, tx=GPIO3)`. GPIO4→CRX proven by 14 960 decoded frames. GPIO3→CTX proven connected (busErr 0 in listen-only requires it held recessive). |
| OTA | Solid after the `lru_purge_enable` fix — ~15 flashes this session, no lockout. |

## Does not work

The moment the controller is allowed to drive the bus — **NORMAL or NO_ACK, identically**:

```
rxFrames  0          nothing ever decodes
rxErr     129        pinned at error-passive
busErr    ~1500/s
txErr     0 -> 128 -> bus-off (state 2) -> reinstall -> repeat, ~every 4 s
```

Our transmissions are never acknowledged (`txErr += 8` per attempt — the CAN penalty for a
missing ACK). Handshake never completes.

## Eliminated — do not re-test these

- **The 15 local commits.** `origin/main` = 0.2.1. Built its `examples/90_bench_ota` — the
  exact firmware that put text on the glass on 2026-07-27 — flashed it, and it fails
  **identically**: `rxFrames 0`, `txErr 128`, bus-off, never synced. So 0.3.0, the owned
  task, the recovery layer and `periph_module_reset` are all innocent.
- **Bit timing.** 8 configs swept directly through `twai_*`: sample points 60%→85%, 8→20
  time quanta, ± triple sampling. Every one identical, `rx=0`. Rules out sample point and
  transceiver loop delay together.
- **Our library.** `examples/02_canspy` links none of it (`build_src_filter = -<*>`) and
  fails the same way.
- **Bitrate, pins, TWAI peripheral, driver install, GPIO matrix** — see table above.

## Unknown — needs instruments, not more firmware

1. **Does our dominant actually reach the bus at correct differential levels?** Everything
   above proves the ESP32 drives GPIO3 correctly; nothing proves what CANH/CANL do when it
   does. Scope on CANH/CANL while transmitting is the only way to see this.
2. **Termination.** Ohmmeter across CANH/CANL with everything powered down: expect ~60 Ω
   (two 120 Ω in parallel). 120 Ω means one terminator; anything else is informative.
3. **The anomaly worth understanding.** In listen-only we see ~1500 frames/s with
   **busErr 0** — a clean bus. But a CAN transmitter that gets no ACK *must* signal an ACK
   error, and an error frame is 6 dominant bits we would count. Nobody is ACKing (listen-only
   cannot), yet there are zero errors. Either the panel does not require an ACK, or something
   else on the bus is providing one. This was not resolved.

## Tools left behind

`examples/02_canspy` — bare `esp32_can`, **no AffaDisplay linked**. Flash it and use:

| endpoint | what it does |
|---|---|
| `/api/listen?on=1\|0` | listen-only ⇄ normal, live. **Start here.** |
| `/api/frames?n=48` | raw ring, RX+TX, ext and RTR included |
| `/api/status` | verdict banner + full driver counters |
| `/api/looptest?pin=N` | internal loopback, transceiver not in the path |
| `/api/pintest` | drive CTX by hand, watch CRX |
| `/api/timingsweep` | the 8 bit-timing configs |
| `/api/noack?on=1` | NO_ACK mode |
| `/api/autospeed` | bitrate sweep |

`examples/01_bringup` — the SUCCESS sequence with OTA. `examples/03_hello` — same sequence,
minimal, no WiFi (needs a cable).

## Two traps that produced false results here

- **Arbitration.** The first TX self-test used id `0x7AB`, which loses every bit fight to the
  panel's `0x3CF`. A test that can never win the bus proves nothing. Use `0x001`.
- **TXD dominant timeout.** Holding TXD low for ~10 ms makes the transceiver disable its own
  driver; the bus goes idle and CRX reads *higher*, inverting the verdict. Use pulses under
  ~300 µs. Also: `twai_driver_uninstall()` does **not** return the pads to GPIO control —
  without `gpio_reset_pin()` first, `digitalWrite()` does nothing and the test lies. This is
  why MegaOpen's old `/api/can/selftest` reported `txPath: broken` on a good board.
