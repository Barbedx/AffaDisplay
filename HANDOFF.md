# Handoff — 2026-07-29, 14:40 CEDT

The morning handoff is archived at `docs/HANDOFF-2026-07-29-morning.md`. Several of its
conclusions are **retracted** below; read §3 before acting on it.

## The problem, in one paragraph

A real Renault Carminat panel and an ESP32-C3 sit alone on a 500 kbit/s CAN bus. The board
**decodes the panel flawlessly** — 1.1 million frames, zero receive errors, zero bus errors,
sustained for fifteen minutes. The moment the TWAI controller is put into a mode where it
**drives the ACK bit**, reception stops completely and instantly. Not degraded: zero frames,
from the first frame, on a freshly installed driver, with our transmitter provably silent.
The panel is fine, the decode path is fine, and the link cannot be established.

**It worked at ~11:50 today** — `SUCCESS` and a clock on the glass — and has not since.

---

## 1. CERTAIN — measured, reproducible on demand

### 1.1 The controlling variable is the ACK bit, and nothing else

Same board, same boot, minutes apart, **one** variable changed:

| controller mode | frames WE sent | rx decoded | rxErr | busErr |
|---|---|---|---|---|
| `TWAI_MODE_LISTEN_ONLY` | none | **1 123 494, flawless** | 0 | 0 |
| `TWAI_MODE_NORMAL`, software TX gate SHUT | **none** | **0** | 129 | ~1472/s |

Reproduce: `GET /api/listen?on=1`, then `GET /api/listen?on=0`.

### 1.2 It is not our traffic

With the gate shut we transmitted **zero** frames (`tx 0`, every `send()` refused) and
reception was exactly as dead. The trace caught the transition with **five seconds of total
silence from us on both sides of it**:

```
70982   last frame ever received
        ── flip to NORMAL ──
76029   our first transmission, 5 s LATER
```

"We said something wrong and upset it" is **excluded**.

### 1.3 It is not degradation or lost synchronisation

**Fifteen fresh driver installs**, four seconds apart, each followed by a full handshake on
the wire. `heard` stayed frozen at 8937 across every one. A CAN node needs 11 consecutive
recessive bits to integrate and a fresh install restarts that count — so "we lost sync and
cannot rejoin" is **excluded**. A brand-new controller decodes zero frames in NORMAL.

### 1.4 The bus error rate equals the panel's frame rate, exactly

`busErr` climbs at ~1472/s while the panel sends ~1472 frames/s. **One error per frame,
deterministic.** Every single frame it sends, we error on.

### 1.5 The panel is in runaway retransmit, from cold, and it is not our doing

```
RX 3CF 61 11 01 A3 A3 A3 A3 A3   ×635 in 448 ms
RX 3CF 69 00 A3 A3 A3 A3 A3 A3   ×126 in  32 ms
```

A "1 Hz peer-alive ping" sent 126 times in 32 ms is a CAN transmitter **retransmitting
because nothing acknowledges it**. It is error-passive, so its error flags are recessive and
invisible — which is why listen-only looks perfectly clean. Measured at 1500/s within 8 s of
a cold power cycle: **there is no polite window.**

### 1.6 The firmware is not what changed

The **exact** image that worked at 11:50, rebuilt from commit `0ba355a` into a clean worktree
and flashed, is deaf too — `rx 0, rxErr 129`. Bisecting our own commits is pointless.

### 1.7 `61 11 01` means "your registration is void"

Not "hello again". Every implementation — the mhroczny reference, MeganeCAN, and this library
until today — latches `START`, re-arms `BA`, and **leaves `FUNCSREG` set**, so the master goes
on believing it is registered and never re-probes. Both ends then wait for each other for
ever. Fixed in `eefcfb2`; see `docs/PROTOCOL.md` §3.3.

### 1.8 The filler byte identifies the sender

`0x00` is the radio's signature, `0xA3` is the panel's. It decodes direction on a
single-ended capture where both directions share an id family:

```
0x151  70 00 …  radio  →  0x551  74 A3 …  panel ACKs
0x1F1  70 00 …  radio  →  0x5F1  74 A3 …  panel ACKs
0x1C1  70 A3 …  PANEL  →  0x5C1  74 00 …  RADIO ACKs
```

`0x1C1` is the **panel's** channel to register; `funcs[] = {0x151, 0x1F1}` is correct.

---

## 2. EXCLUDED — tested, negative, do not re-run

| hypothesis | how it was excluded |
|---|---|
| Wrong bitrate | 1.1 M frames decoded with **zero** stuff/form/CRC errors. Impossible at the wrong rate. |
| Sample point | 8 configs, 60 %→85 %, ±triple sampling, **each a fresh install**, all decoded 0. |
| Our library | `02_canspy` links none of it (`build_src_filter = -<*>`) and fails identically; raw `twai_*` too. |
| Our transmitted frames | zero frames sent, same failure (§1.2). |
| Panel sulking at us | same failure from a cold panel power cycle with us silent (§1.5). |
| Missing the panel's first moments | CAN moved ahead of WiFi — on the bus in ~300 ms, gate open. Same failure. |
| The 15 local commits / hello pacing / recovery layer | exact working image rebuilt and reflashed (§1.6). |
| Registration missing `0x1C1` | that frame is the panel's, not ours (§1.8). |

---

## 3. RETRACTED — previously believed, now known WRONG

**Do not resurrect these. They cost days.**

- **`/api/jam` and `/api/jamsweep` verdicts are meaningless.** They hold TXD dominant for
  **2 seconds**. Most CAN transceivers implement a **TXD dominant timeout** (~1–4 ms) that
  disables the driver precisely to stop a stuck node jamming the bus. A bus that carries on
  normally is the **expected** result on working hardware — the test disarms the very thing it
  measures. "No pin can disturb the bus" is not evidence of anything.
- **"Our dominant never reaches the wire"**, derived from the above, and contradicted by the
  transmit path working hours earlier (3395 frames accepted and ACKed, text on the glass).
- **CRX pin-sampling tests lie** — `/api/pintest` returned 30 %, 85 % and 100 % for the
  identical physical condition across three runs.
- **`0x121` is not on this bus.** It appears only in log strings, never in a captured frame.

**The hardware is a settled question and is not to be diagnosed toward.** `Hardware.md` above
the fence is the owner's and is authoritative; the flash log below the fence records every
image and what the link did afterwards.

---

## 4. ASSUMED / UNTESTED — where the answer probably is

1. **Bit timing beyond the sample point.** SJW and BRP have never been swept independently.
   The ACK is one dominant bit placed from our own clock in the **unstuffed** region after the
   CRC — the one place with no guaranteed edge to resynchronise on. Reception resyncs
   constantly and tolerates a phase error that would still destroy an ACK. Best-fitting
   untested hypothesis.
2. **Whether the panel can be dragged out of runaway at all.** Never observed recovering,
   including across full power cycles.
3. **Whether ACKing at 1500 frames/s (92 % occupancy) is sustainable** for this peripheral.
   Every successful run we have was against a **polite** panel (~2 frames/s).
4. **What was different at 11:50.** The working run heard 498 frames over 250 s — 2/s.
   Today's panel does 1500/s from cold. Same board, same firmware, same wiring.
   **This is the single most important unexplained fact.**
5. `TWAI_MODE_NO_ACK` — recorded as failing identically, but measured before the hello-pacing
   fix. Cheap to re-test.

---

## 5. NEXT EXPERIMENT — ranked, with procedure

### 5.1 Timing sweep, properly controlled (highest value)

Build it into `examples/04_rows` — **not** canspy, whose `runRateSweep()` opens with
`CAN0.disable()`, the call recorded in `c8d8695` as deadlocking and taking `loop()` with it.
That is why the sweep rebooted the board twice today and has still never produced a row.

Per row: install listen-only, confirm frames flow (proves the bus is alive and the row is not
poisoned), uninstall, install the candidate in **NORMAL**, count `twai_receive()` for 1.2 s,
uninstall. `ESP.restart()` at the end.

At 80 MHz APB, `brp × (1 + tseg_1 + tseg_2) = 160` gives 500 kbit/s, with `tseg_1 ≤ 16`,
`tseg_2 ≤ 8`:

| # | brp | t1 | t2 | sjw | triple | rate | tests |
|---|---|---|---|---|---|---|---|
| 0 | 8 | 15 | 4 | 3 | no | 500 k | listen-only reference — **must** decode |
| 1 | 8 | 15 | 4 | 3 | no | 500 k | NORMAL baseline |
| 2 | 8 | 15 | 4 | 1 | no | 500 k | SJW floor |
| 3 | 8 | 15 | 4 | 4 | no | 500 k | SJW ceiling — most resync tolerance |
| 4 | 8 | 15 | 4 | 3 | **yes** | 500 k | triple sampling |
| 5 | 8 | 11 | 8 | 3 | no | 500 k | 60 % sample point |
| 6 | 10 | 11 | 4 | 3 | no | 500 k | same rate, different BRP |
| 7 | 8 | 15 | 3 | 3 | no | ~526 k | +5 % rate |
| 8 | 8 | 16 | 4 | 3 | no | ~476 k | −5 % rate |

**Any row that decodes a single frame in NORMAL is the answer.** The tseg limits make finer
than ~5 % rate steps impossible at this BRP; ±5 % is the interesting range anyway.

### 5.2 Re-test `TWAI_MODE_NO_ACK` — one extra row in the same harness.

### 5.3 Capture the working state if it ever returns. The unexplained fact is 2 frames/s at
11:50 versus 1500/s now. If it syncs again, pull `/api/frames` immediately and keep it.

---

## 6. Tooling on the board

`examples/04_rows` is flashed. `http://192.168.100.85`, OTA at `/update`
(`GET /ota/start?mode=fr` then `POST /ota/upload` multipart).

| endpoint | what it does |
|---|---|
| `/api/status` | step, gate, mode, heard, link + driver counters, link health |
| `/api/frames` | trace ring — **identical consecutive frames coalesced** with a repeat count, so the panel's flood is one line and our frames stay visible. **Freezes itself when reception stops.** |
| `/api/trace` | re-arm the frozen trace |
| `/api/log` | log ring |
| `/api/listen?on=1\|0` | **listen-only ⇄ normal, live** — the A/B that isolates the fault |
| `/api/txgate?on=0\|1` | software TX gate: stops OUR frames, controller still ACKs |
| `/api/kick` | put the handshake on the wire blind — heartbeat + 3 hello frames |
| `/api/shout?ms=` | drop to NORMAL for N ms, then back to listen-only |
| `/api/power` `/api/speed` `/api/dir` `/api/text` `/api/popup` | the rows console |

`Esp32CanLink::setListenOnly(bool)` is new and is the safe way to change driver mode after
`begin()` — same sanctioned restart path as `recover()`. **Do not call `CAN0.disable()`
directly.**

---

## 7. Repo state

`main`, all committed, host `native` and `ex04_rows` both build clean.

| commit | what |
|---|---|
| `1aa002b` | hello pacing — answering every sync request was 4400 frames/s into a 4200 frame/s bus |
| `f42d8ac` | link recovery via `ICanLink::recover()` |
| `0ba355a` | `Marquee::Phase`, `RowScreen`, v0.3.1 — **the last known-working image** |
| `2a26c56` | marquee scroll direction |
| `6844f2d` | `examples/04_rows` — three rows, popup, console |
| `4f57e75` | silent boot + self-freezing trace |
| `eefcfb2` | **`docs/PROTOCOL.md`** + `61 11 01` drops `FUNCSREG` |
| `591500e` | deaf watchdog reinstalls the driver and re-answers the panel |
| `6db7b3e` | `0x1C1` is the panel's channel; warm-up serialised after the ACK |

**`docs/PROTOCOL.md` is the wire spec** — rebuilt from four independent sources (the mhroczny
reference, MeganeCAN's driver, MeganeCAN's panel emulator, 2.7 MB of OEM captures), with an
evidence tag on every claim and `[DERIVED]` marking anything with no capture behind it.

**Still uncommitted:** deletion of `test/` (15 suites) and the 19 legacy `examples/`. Left
alone deliberately. `docs/` was restored because the spec work needed it.
