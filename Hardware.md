Esp32 connected to can bus driver PROPERLY.
pin4 CRX pin3 CTX
power connected and diplay works. 
ITS 100% and hardwaree 100% if that text is here. 

---

# Flash log

Everything above this line is the OWNER'S. Claude does not edit it. Everything below is
Claude's record of what was put on the board and when, so that "it worked twenty minutes
ago" is a fact with a firmware next to it rather than a memory.

Newest last. Times are local (CEDT).

## 2026-07-29

| time | image | why | what the link did afterwards |
|---|---|---|---|
| ~11:5x | `ex01_bringup` @ b317b4f | previous session | **worked — SUCCESS and the clock on the glass.** Found later at `rx 498, tx 3395, sinceRx 26 min`: it heard 498 frames early and then stopped hearing |
| 12:26 | `ex04_rows` build 1 (874416 B) | first cut of the new example | `rx 0`, `txErr 128`, bus-off cycling |
| 12:33 | `ex04_rows` build 2 (876516 B) | added Layer-0 frame ring, `/api/frames`, `/api/txgate` | `rx 0`, `rxErr 129`, `busErr` ~20/s |
| 12:41 | `ex04_rows` build 3 (876956 B) | added driver state + `busErr` to `/api/status` | `rx 0`; full cycle traced: `rxErr 129` → `busErr` 0→808 → `txErr 128` → BUS-OFF → reinstall, ~45 s |
| 12:47 | `ex02_canspy` @ f04cea8+ | needed true listen-only, which `Esp32CanLink` refuses | **listen-only: 157 000+ frames, `rxErr 0`, `busErr 0`, `sinceRx 5 ms`.** The panel transmits continuously and we decode it perfectly while we drive nothing |
| 12:52 | *(no flash)* | `/api/jam` — held GPIO3 dominant for 2 s | 2964 frames/2 s vs 3001 baseline; CTX read back LOW on 100% of pulses |
| 12:54 | *(no flash)* | `/api/jamsweep`, 11 pins | no pin disturbed the bus |
| 12:56 | `ex04_rows` build 3 | restored the soak image | `rx 0`, `rxErr 129`, `busErr 515`, flaps 733 in 735 s |
| 13:02 | `ex04_rows` build 4 (878726 B) | **boots SILENT** (TX gate shut until 20 frames heard) + `/api/trace`, a frame ring that FREEZES itself the moment reception stops | **`tx 0` — not one frame sent — and still `rx 0` with `busErr` 2400/s.** `flaps 0`, controller stays RUNNING |

## What this log establishes

| run | our TX | do we ACK? | rx | busErr |
|---|---|---|---|---|
| listen-only, 12:47 | none | **no** | 157 000+ | 0 |
| NORMAL gate shut, 13:03 | **none** | **yes** | 0 | ~2400/s |
| NORMAL talking, ~11:5x | 3395 frames, **panel ACKed them** | yes | 498, then deaf | — |

1. **Transmitting nothing does not help.** Build 4 sent zero frames and reception was still
   dead. So "we sent a wrong message and upset it" is not the mechanism.
2. **The hardware TX path is not dead.** 3395 frames went out and were acknowledged at
   ~11:5x; that is what put SUCCESS on the glass. A transmitter that cannot reach the bus
   cannot do that. The 12:52 jam verdict must therefore be read as "not driving *at that
   moment*", not "never able to drive". **Do not conclude hardware from it.**
3. **The one variable that tracks the failure is whether our controller ACKS.** Listen-only
   (no ACK) is flawless; NORMAL with zero transmissions (ACK on) is a 2400/s error storm.
4. **The panel is in runaway retransmit** — one frame repeated at line rate is a CAN
   transmitter that has never been acknowledged. Our ACK into that state errors on every
   frame, so we decode none, so it is never acknowledged. Both ends are locked, and a
   reboot re-enters the lock because the panel is already in it.
5. **`ex01_bringup` heard 498 frames over 250 s — about 2/s — and then went deaf.** That is
   the panel being POLITE. The working state and the broken state differ by the panel's own
   mode, not by our wiring.

### Next measurement
Power-cycle the panel while the board runs build 4 (which boots silent). A freshly powered
panel is polite, which is the state our ACK is known to work in. If it dies again,
`/api/trace` freezes on the transition and holds the last frames before it.

## 2026-07-29 (afternoon session)

| time | image | why | what the link did afterwards |
|---|---|---|---|
| ~15:1x | `ex04_rows` build 5 (unlogged by previous session; has `/api/listen`, setListenOnly from 1220f78) | previous session | found running in LISTEN-ONLY, panel in runaway (1500 f/s of `61 11 01` + `69 00`), decoding flawlessly |
| ~16:0x | *(no flash)* | `/api/kick?ms=1500` — the panel-reaction oracle | panel INDIFFERENT: same ~500 ms macro-cycle (126× `69 00` + ~500-630× `61 11 01`) before and after; our decode dead during the NORMAL window |
| ~16:2x | `ex04_rows` +ECC probe (884 kB) | read the error-code-capture register — never done before | ECC always EMPTY across 7618 polls while busErr counted hundreds: the driver ISR consumes the capture to re-arm the bus-error interrupt |
| ~16:3x | `ex04_rows` +ECC, BEI masked | let the capture survive until our poll | **VERDICT: `BIT RX @ ACK-SLOT` ×1996, plus `BIT TX @ SOF` (tec 144-152). Every dominant we drive samples back recessive.** |
| ~16:4x | `ex04_rows` +`/api/pad` | is the dominant leaving the chip? | GPIO matrix `out_sel[3]=74`, OE on, IO_MUX correct — and the PAD PULSES: ~1770 one-bit-wide dominant pulses/s (the ACKs) |
| ~16:5x | `ex04_rows` +pad correlation | same-instant CTX/CRX correlation, one register read | **while CTX low: CRX low 0, CRX HIGH 627 (of 627). Our dominant leaves GPIO3 and never appears on GPIO4. Control: CRX shows panel traffic at 10-23% duty in the same window.** |

### What this session establishes

Every firmware-observable point is now measured and correct: matrix routing, output
enable, pad waveform (real per-frame ACK pulses, correct width, correct rate), and the
controller's own error capture agreeing with all of it. The segment GPIO3-pad →
[return path] → GPIO4-pad does not carry our dominant, while carrying the panel's traffic
perfectly. Firmware has no lever on that segment and no remaining unexplored lever behind
it. Historically the loop worked after the rig repower on the morning of 07-29 (3395
ACKed frames, SUCCESS on the glass) and died mid-run 26 minutes later; nothing in any
firmware image correlates with either edge.
