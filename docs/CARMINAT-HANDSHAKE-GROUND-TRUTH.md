# Carminat radio↔display registration handshake — ground truth

**Status:** authoritative. Derived from 579 frames across four passive-sniffer CSVs in `C:\Users\Andru\source\repos\AffaDisplay\docs\captures\` (real OEM Renault radio + real Carminat "AFFA 3 NAV" display, no ESP32 on the bus), cross-checked against our own bench capture `C:\Users\Andru\source\repos\AffaDisplay\captures\authclock-2026-08-03-ctx4-crx3-success-01.txt`.

Bus: 500 kbit/s, standard 11-bit identifiers, **every frame is DLC 8, always**. Every claim below tagged **[CAP]** is measured in the OEM captures; **[BENCH]** is measured on our rig; **[INF]** is inference and is listed again in §9.

> **Measurement floor.** The sniffer emits deltas as small as 6 µs. An 8-byte standard frame is ≥ 130 µs on the wire at 500 kbit/s. **Any delta below ~130 µs is a logger artifact and must never be quoted as a protocol timing.** Two 1C1→5C1 latencies (0.017 ms, 0.020 ms) are discarded on this basis.

---

## 1. Node roles and channel map

Direction is determined by the **padding/filler byte**, and this is proven rather than assumed: semantically identical messages carry different filler depending on the ID group — `70` open appears as `1C1 70 A3 A3…` and as `151 70 00 00…`; `74` ack appears as `551 74 A3…` and `5C1 74 00…`. Filler is therefore a property of the transmitting node, not of the message. **0/579 exceptions.** [CAP]

| CAN id | Direction | Filler | Purpose | Responder id |
|---|---|---|---|---|
| `0x3AF` | RADIO → display | `0x00` | Sync/announce channel: `B9` alive, `BA` request, `B0 14 11 00 1F 00 00 00` capability announce | *(none — no ACK semantics)* |
| `0x3CF` | DISPLAY → radio | `0xA3` | Sync channel: `61 11 00` / `61 11 01` hello-request, `69 00` / `69 01` liveness ping | *(none)* |
| `0x151` | RADIO → display | `0x00` | Function 1 — text / clock / power / control. ISO-TP requester | `0x551` |
| `0x551` | DISPLAY → radio | `0xA3` | Responder for `0x151`: `74` done, `30 01 00` flow control | — |
| `0x1F1` | RADIO → display | `0x00` | Function 2 — graphics / nav pictograms. ISO-TP requester | `0x5F1` |
| `0x5F1` | DISPLAY → radio | `0xA3` | Responder for `0x1F1`: `74` done, `30 01 00` flow control | — |
| `0x1C1` | DISPLAY → radio | `0xA3` | Display's own control channel: channel open `70`, capability `02 64 0F`, version `05 63 "0040"`, key events `03 89 …` | `0x5C1` |
| `0x5C1` | RADIO → display | `0x00` | **We must transmit here.** `74` ack for every `0x1C1` frame | — |

**Universal rule: responder id = requester id + 0x400.** Verified for all three pairs. Implementations test `id & 0x400` as a bit test, not equality.

**Opcode alphabet (channel-independent):**

| Byte (position 0 of payload, after any PCI) | Meaning |
|---|---|
| `0x70` on a requester id | open/register this function |
| `0x74` on a responder id | acknowledged / done |
| `0x30 01 00` on a responder id | ISO-TP flow control, CTS, BS=1, STmin=0 |

**Caveat on filler.** `0xA3` is a *positive* fingerprint of the display. `0x00` is only "not the display" — it is the CAN default and is indistinguishable from genuine zero data. A third `0x00`-padding node would be silently folded into "radio". The role assignment is nevertheless independently corroborated: the `0x00` group sends the content (48×48 pictogram on `0x1F1`, ASCII text on `0x151`) and the `0xA3` group issues the ISO-TP flow control — content flows *toward* a display, and the receiver of a segmented transfer is the one issuing FC. [CAP]

---

## 2. The registration state machine

Terminology below: **we** = the radio role (the ESP32 emulating the head unit), **panel** = the Carminat display.

### 2.0 The invariant that governs both entry paths

> **The B0 announce burst is triggered by a `3CF 61 11 xx` frame that arrives after we have transmitted `3AF BA`, and it starts 31 ms after that frame. The value of `xx` (00 or 01) is irrelevant to the trigger.**

This is a measured discriminating test, not a preference. Δ from the triggering `61 11 xx` to `B0 #1`, all four captures:

| capture | trigger | Δ from `61 11 xx` | Δ from our last `BA` |
|---|---|---|---|
| `aknowledge on on display.csv` | `61 11 **00**` | **30.740 ms** | 37.982 ms |
| `aknowledge offed display.csv` | `61 11 **00**` | **31.527 ms** | 31.634 ms |
| `aknowledge offed display 2.csv` | `61 11 **00**` | **30.817 ms** | 61.914 ms |
| `…cONNECT OT POWER.csv` | `61 11 **01**` | **30.751 ms** | 111.783 ms |

Spread anchored on the hello: **0.79 ms**. Spread anchored on `BA`: **80.1 ms**. The hello is the trigger; `BA` is only the precondition. Any implementation that times the burst off `BA` is wrong. [CAP]

`aknowledge offed display cONNECT OT POWER.csv` contains **sixteen `3CF 61 11 01` frames and zero `61 11 00`**, and completes a full session — registration, power command, ISO-TP text — off `01` alone. **`61 11 01` is not "bad auth". It is the same request; the low bit is a state indication from the panel, not an authorization grade.** [CAP]

### 2.1 Entry path A — display already powered, spamming `61 11 01`; radio cold-starts into it

This is the common bench case (panel on 12 V, ESP32 rebooted).

| # | Actor | Frame | Trigger / timing | Notes |
|---|---|---|---|---|
| **A1** | Panel | `3CF 61 11 01 A3 A3 A3 A3 A3` | free-running, **103.985 ms mean** (min 103.838, max 104.230, n=15) | Repeats indefinitely while no radio is present. **15 of 16 went unanswered** before the radio woke. [CAP] |
| **A2** | Panel | `3CF 69 00 A3 A3 A3 A3 A3 A3` | interleaved, ~503.95 ms | Liveness ping, unrelated timer. [CAP] |
| **A3** | **Us** | `3AF B9 00 00 00 00 00 00 00` | on power-up, unsolicited | The radio announces itself. First observed radio frame at 147328246. [CAP] |
| **A4** | **Us** | `3AF BA 00 00 00 00 00 00 00` | **B9 + 0.29 ms** | The pair is one transaction. Send `BA` immediately after `B9`, once. **Never periodically.** |
| **A5** | Panel | `3CF 61 11 01` | the panel's *next* scheduled hello, +81 ms in the capture | Nothing changes on the panel side — it is still on its own 104 ms timer. We simply latch onto the next one. |
| **A6** | **Us** | `3AF B0 14 11 00 1F 00 00 00` | **A5 + 31 ms** (tolerance **30.7–31.6 ms**) | Announce #1. |
| **A7** | Panel | `1C1 70 A3 A3 A3 A3 A3 A3 A3` | **B0#1 + 0.8…1.6 ms** | Panel opens its own control channel. Arrives *between* B0#1 and B0#2 — this is why the 31 ms staging exists. |
| **A8** | **Us** | `5C1 74 00 00 00 00 00 00 00` | **A7 + 0.25…0.48 ms** | **Mandatory.** See §4. Must interleave with the B0 burst. |
| **A9** | **Us** | `3AF B0 14 11 00 1F 00 00 00` | **B0#1 + 31 ms** (30.887–31.207) | Announce #2, byte-identical. |
| **A10** | **Us** | `3AF B0 14 11 00 1F 00 00 00` | **B0#2 + 31 ms** | Announce #3, byte-identical. Exactly three, never two, never four. [CAP 4/4] |
| **A11** | **Us** | `151 70 00 00 00 00 00 00 00` | **B0#3 + 0.10…0.30 ms** | Register function 1. Unconditional — no application involvement. |
| **A12** | **Us** | `1F1 70 00 00 00 00 00 00 00` | **B0#3 + 0.31…0.59 ms**, i.e. ~0.29 ms after A11 | Register function 2. **Pipelined: sent before A11 is acknowledged.** |
| **A13** | Panel | `551 74 A3 A3 A3 A3 A3 A3 A3` | A12 + ~0.5 ms | Function 1 registered. |
| **A14** | Panel | `5F1 74 A3 A3 A3 A3 A3 A3 A3` | A13 + ~0.47 ms | Function 2 registered. **This frame is the registration-complete anchor.** |
| **A15** | — | *(quiet)* | **400 ms ± 0.5 ms measured from A14** | See §6. Nothing is transmitted on `0x151`/`0x1F1` in this window. |
| **A16** | **Us** | `151 03 52 09 00 00 00 00 00` | end of A15 | Display ON. Application sequence begins, §6. |

**After A6, `61 11 xx` is never seen again for the life of the session** — in all four OEM captures and in our own successful bench run. Its disappearance is the definitive signal that registration took. [CAP][BENCH]

### 2.2 Entry path B — display and radio powering together (`61 11 00` path)

Identical from step 5 onward; only the opening differs.

| # | Actor | Frame | Trigger / timing |
|---|---|---|---|
| **B1** | **Us** | `3AF B9 **01** 00 00 00 00 00 00` | first radio frame after power-up, unsolicited |
| **B2** | **Us** | `3AF BA 00 00 00 00 00 00 00` | **B1 + 8.17 ms** |
| **B3** | Panel | `3CF 61 11 00 A3 A3 A3 A3 A3` | **B2 + 7.24 ms** — the panel *answers* here rather than free-running |
| **B4…** | — | **identical to A6…A16** | B0#1 at **B3 + 30.74 ms**, then exactly as above |

Reference (`aknowledge on on display.csv`, µs):

```
4685844  3AF  B9 01 00 00 00 00 00 00     radio alive (note byte1 = 01, once, ever)
4694018  3AF  BA 00 00 00 00 00 00 00     +8.174 ms
4701260  3CF  61 11 00 A3 A3 A3 A3 A3     +7.242 ms
4732000  3AF  B0 14 11 00 1F 00 00 00     +30.740 ms   B0 #1
4732831  1C1  70 A3 A3 A3 A3 A3 A3 A3     +0.831 ms
4733301  5C1  74 00 00 00 00 00 00 00     +0.470 ms    MANDATORY
4763018  3AF  B0 14 11 00 1F 00 00 00     +31.018 ms   B0 #2
4794010  3AF  B0 14 11 00 1F 00 00 00     +30.992 ms   B0 #3
4794106  151  70 00 00 00 00 00 00 00     +0.096 ms
4794393  1F1  70 00 00 00 00 00 00 00     +0.287 ms
4794874  551  74 A3 A3 A3 A3 A3 A3 A3     +0.481 ms
4795343  5F1  74 A3 A3 A3 A3 A3 A3 A3     +0.469 ms    <-- 400 ms anchor
5195152  151  03 52 09 00 00 00 00 00     +399.809 ms
```

`B9 **01**` appears exactly **once in 579 frames**, as the very first radio frame of a cold co-boot. Every other `B9` in every capture is `B9 00`. It is a boot marker, not a state. [CAP]

### 2.3 Transition table, retry and timeout behaviour

| State | Transition trigger | Action | On failure |
|---|---|---|---|
| `IDLE` | our power-up | TX `B9`, then `BA` +0.3…8 ms | If either frame is refused locally, retry **the complete pair** after a bounded delay. Never retry `BA` alone as a periodic stream. |
| `AWAIT_HELLO` | RX `3CF 61 11 xx`, DLC ≥ 3, **any** `xx` | arm B0 burst at `now + 31 ms` | No timeout of our own. The panel free-runs at 104 ms; the next one will come. A short `61 11` (DLC < 3) is ignored, not answered. |
| `ANNOUNCE` | 31 ms elapsed | TX one `B0`; re-arm at `+31 ms`; repeat until 3 sent | If the controller refuses a frame, retry **that frame**, not a fresh burst. `1C1 → 5C1` has priority and must be allowed to interleave. |
| `ANNOUNCE` (during) | RX `3CF 61 11 xx` again | **ignore** — do not restart the burst | A duplicate hello while a burst is in flight must not re-arm anything and must not tear down state. |
| `REGISTER` | B0 #3 accepted | TX `151 70` then `1F1 70` back-to-back, **without waiting for ACKs** | Each probe is ACK-gated with a 2 s deadline and retried up to 3 times. A probe that exhausts retries fails the session; return to `AWAIT_HELLO`. |
| `SETTLE` | RX `5F1 74` (last registration ACK) | arm application gate at `+400 ms` | — |
| `RUN` | 400 ms elapsed | release application queue, `03 52 xx` first | — |
| any | RX `3CF 61 11 xx` **after** `RUN` was reached | session lost — tear down registration, return to `AWAIT_HELLO` | Not observed in any capture (the panel goes silent on `3CF 61 11` once registered), so this is a defensive rule. **[INF]** |

**Panel-side retry behaviour, for reference:** the panel re-issues `61 11 xx` at ~104 ms until it gets the B0 burst, and re-issues `1C1 70` at **~610 ms** until it gets `5C1 74`. In our bench capture 22 of 23 `1C1 70` frames went unanswered and the panel stalled on `61 11 01` for 15 seconds as a direct consequence. [BENCH]

---

## 3. Steady state

Two **independent free-running timers**. This is not a request/response relationship, and treating it as one is the single most damaging modelling error available here.

| Stream | Sender | Period | Jitter | Must be answered? |
|---|---|---|---|---|
| `3AF B9 00 00 00 00 00 00 00` | radio (us) | **500.08–500.14 ms** | σ = 0.11–0.50 ms | No. Free-runs. |
| `3CF 69 00 A3 A3 A3 A3 A3 A3` | display | **503.93–503.96 ms** when the display is OFF; **bimodal ~504 / ~512 ms, mean 507.83, σ 4.60** when the display is ON | see left | **No. Answer nothing.** |
| `1C1 …` (any payload) | display | event-driven | — | **YES — always, see §4** |
| `151 …` / `1F1 …` (any payload) | radio (us) | on demand | — | Answered by the display with `74` or `30`; 7/7 and 45/45 in the corpus |

**Proof of independence:** the phase between `3AF B9` and the next `3CF 69` drifts monotonically and wraps through zero with no interaction whatsoever — 319.481 ms → 59.656 ms in one capture; a minimum phase of **0.023 ms** in another (the two frames virtually collide) with no change in either cadence. A request/response pair cannot drift through 0→500 ms. [CAP]

**Consequence for the library: `B9` must NOT be emitted as a reply to `69`.** The correct behaviour is one free-running 500 ms timer and nothing else. An implementation that both paces at 500 ms and pongs each 504 ms ping emits **two `B9` about 4 ms apart every ~504 ms — double the OEM rate**.

The `~504 ms` figure for `69` is **refuted as a universal constant**: with the display ON it alternates 504/512 ms (histogram `{498:1, 502:1, 504:10, 505:3, 506:1, 510:2, 511:2, 512:10, 520:1}`). Do not build any timeout on a tight `69` period; use a generous liveness window (≥ 3 missed pings).

`69 01` (byte 1 = `01` instead of `00`) was observed only in our own bench capture, only inside the un-registered window, six times at ~504 ms, and never in the OEM corpus. Treat it as identical to `69 00`. **[INF]**

---

## 4. The mandatory `0x5C1` acknowledgement rule

> **Every frame received on `0x1C1` MUST be answered with `5C1 74 00 00 00 00 00 00 00` within ~0.5 ms, regardless of payload, regardless of session state, and regardless of whether we understand the payload.**

Evidence: **12/12 `0x1C1` frames acknowledged across all four OEM captures.** Latencies (artifacts excluded): 0.470, 0.480, 0.327, 0.483, 0.261, 0.288, 0.249, 0.467, 0.453, 0.253 ms — **range 0.249–0.483 ms, mean ≈ 0.36 ms.** [CAP]

Payloads observed on `0x1C1` and all acknowledged identically:

| Payload | Meaning | When |
|---|---|---|
| `70 A3 A3 A3 A3 A3 A3 A3` | channel open / register | between B0#1 and B0#2 |
| `02 64 0F A3 A3 A3 A3 A3` | capability report | ~6 ms after the power command |
| `05 63 30 30 34 30 A3 A3` | software version, ASCII `"0040"` (also `"0043"`, `"0038"`) | ~12 ms later |
| `03 89 …` | key press | during operation |

**The ACK must not be gated on session state.** It is emitted at `1C1 70` time, which is *before* our own `151`/`1F1` registration, *during* the announce burst, and in path A it happens while `61 11 01` is still the only thing the panel has ever said. The correct construction is: the ACK is a raw, immediate, unconditional reflex on the receive path; it is deliberately **separate** from the gate that releases our own registration/power/text/clock traffic.

The ACK is byte-exact `74` followed by **seven `0x00` filler bytes** (our filler, not the panel's `0xA3`).

**Framing correction.** The rule is not `1C1`-specific and not radio-specific. The general law on this bus is: *every frame on a requester id `0xNN1` is answered by the peer on `0xNN1 + 0x400`, with `74` (complete) or `30` (flow control).* The display acks us on `551`/`5F1` with the same discipline (7/7 and 45/45). `5C1` is simply our half of a symmetric contract.

---

## 5. ISO-TP on this bus

Both segmented transfers in `aknowledge on on display.csv` reassemble byte-exact with correct sequence numbering.

**PCI, standard ISO 15765-2 single-byte form, no address extension:**

| PCI nibble | Form | Encoding |
|---|---|---|
| `0x0n` | Single Frame | `0n` + n payload bytes, n ≤ 7, pad to 8 with filler |
| `0x1n mm` | First Frame | 12-bit length `n mm`, then 6 payload bytes |
| `0x2n` | Consecutive Frame | SN `n` = 1..F then wraps 0..F, then 7 payload bytes |
| `0x30 BS STmin` | Flow Control | see below |

**Flow control:**

- **The FC is sent by the receiver, on the responder id.** `0x551` issues FC for a `0x151` transfer; `0x5F1` issues FC for a `0x1F1` transfer. Never the sender, never on the requester id. This is also the independent proof of the role assignment in §1.
- **The only FC payload ever observed is `30 01 00 A3 A3 A3 A3 A3`** — FS = 0 (ContinueToSend), **BS = 0x01**, **STmin = 0x00**.
- **BS = 1 means one FC per single CF.** Observed: 43 FCs for 43 CFs on the 0x1F1 transfer, 2 FCs for 2 CFs on the 0x151 transfer. Strict alternation CF→FC→CF→FC. A sender that emits a CF burst will desynchronise the panel.
- **STmin = 0 is not the pacing constraint.** Real CF→CF gaps are 0.645–11.077 ms, mean 2.399 ms; FC round-trip latency mean 1.533 ms, max 10.611 ms. The rate is set by the FC round trip plus sender think-time, three orders of magnitude above the 0 ms floor. Throughput ≈ 2.9 kB/s (302 bytes in 104.934 ms).
- **A transfer terminates with `74` on the responder id**, 2.593 ms after the last CF.
- **FC state machines are per-ID-pair, not global.** The `0x1F1` FF was sent while the `0x151` transfer was still mid-flight (its CF#2 arrived 2.4 ms later). Transfers interleave.

**Do not send FC ourselves in the current configuration.** All display-originated traffic (`0x1C1`) is single-frame in the entire corpus. If the display ever segments on `0x1C1`, we must issue `5C1 30 01 00 00 00 00 00`.

**Reassembled samples, for validation:**

*`0x151`, FF `10 0E 77 09 55 FF 31 01`, FF_DL = 14, 2 CFs:*
```
77 09 55 FF 31 01 20 20 20 31 30 35 36 20      →  opcode 0x77 (write text)
                                                  + 5 selector bytes
                                                  + ASCII "   1056 "
```

*`0x1F1`, FF `11 2E 21 0B 00 25 41 42`, FF_DL = 302, 43 CFs (SN 1..F,0..F,0..B), last CF carries 2 bytes:*

| offset | bytes | meaning |
|---|---|---|
| 0 | `21` | opcode (load/draw pictogram) |
| 1–2 | `0B 00` | unresolved |
| 3–9 | `25 41 42 43 44 45 46` = `%ABCDEF` | 7-char image handle |
| 10–11 | `00 01` | unresolved |
| **12–13** | **`30 30`** | **width 48, height 48** |
| 14–301 | 288 bytes | bitmap, 6 bytes/row × 48 rows, MSB-first |

302 − 14 = 288 = 48×48/8 exactly; the declared `0x30,0x30` matches; nonzero bytes are periodic at stride 6; rendering at stride 6 yields a coherent figure while stride 4 and 8 yield noise. This decode is self-confirming.

---

## 6. Opening application sequence after registration

The **400 ms gap is measured from `5F1 74`, the final registration ACK** — name the anchor, because measured from B0#3 the same interval reads 401.1–402.0 ms.

| capture | first `0x151` payload | Δ from `5F1 74` |
|---|---|---|
| `aknowledge on on display.csv` | `03 52 **09** 00 00 00 00 00` | **+399.809 ms** |
| `aknowledge offed display.csv` | `03 52 **00** 00 00 00 00 00` | **+399.561 ms** |
| `aknowledge offed display 2.csv` | `03 52 **00** 00 00 00 00 00` | **+400.514 ms** |
| `…cONNECT OT POWER.csv` | `03 52 **00** 00 00 00 00 00` | **+399.757 ms** |

**400 ms ± 0.5 ms.** [CAP 4/4]

The third byte tracks the capture filename — `09` in the display-ON capture, `00` in all three "offed" captures — independently confirming both the labelling and that `0x52` is the display-state command.

**Full opening, in order, `aknowledge on on display.csv`:**

```
 t+0        151  03 52 09 00 00 00 00 00     power ON   (len 3, cmd 0x52, arg 0x09)
 t+0.65 ms  551  74 A3 A3 A3 A3 A3 A3 A3     ack
 t+6.2 ms   1C1  02 64 0F A3 A3 A3 A3 A3     display -> radio, capability report
 t+6.7 ms   5C1  74 00 00 00 00 00 00 00     WE MUST ACK
 t+14.2 ms  151  02 54 01 00 00 00 00 00     cmd 0x54 close, arg 0x01
 (ack)      551  74 A3 …
 t+16.5 ms  151  02 54 03 00 00 00 00 00     cmd 0x54 close, arg 0x03
 (ack)      551  74 A3 …
 t+18.4 ms  1C1  05 63 30 30 34 30 A3 A3     display version, ASCII "0040"
 (ack)      5C1  74 00 00 00 00 00 00 00     WE MUST ACK
 t+25.0 ms  151  10 0E 77 09 55 FF 31 01     first text, ISO-TP FF
 (fc)       551  30 01 00 A3 A3 A3 A3 A3
```

**Rules:**

1. **`03 52 xx` is always the first application payload.** Never a clock, never text. The clock is only meaningful once the glass is on.
2. `0x52` payload is **zero-padded**: `03 52 09 00 00 00 00 00`. MeganeCAN's `03 52 09 FF FF 00 00 00` spelling is **not** what the OEM radio puts on this bus. All four captures use zeros.
3. `02 54 01` and `02 54 03` follow within ~15 ms. `0x54` is the close/hide opcode; the arguments are unresolved (§9). They are almost certainly a "clear whatever was on screen" pair. Not required for a clock to render — our bench run set the clock without them.
4. A short warm-up after `03 52 09` before the next command is prudent; the OEM radio waited 14 ms before its next `0x151` frame, and our working example uses 50 ms.

---

## 7. Putting 10:00 on the clock — exact transmit order

Preconditions: registration complete (`551 74` and `5F1 74` both received), 400 ms settle elapsed, `5C1 74` reflex armed.

| # | Dir | CAN id | DLC | 8 bytes | Meaning |
|---|---|---|---|---|---|
| 1 | TX | `0x151` | 8 | `03 52 09 00 00 00 00 00` | SF, len 3, cmd `0x52`, arg `0x09` = display ON |
| 2 | RX | `0x551` | 8 | `74 A3 A3 A3 A3 A3 A3 A3` | panel ack — **wait for this** |
| — | — | — | — | *(warm-up ~50 ms)* | |
| 3 | TX | `0x151` | 8 | `05 56 31 30 30 30 00 00` | SF, len 5, cmd `0x56`, ASCII `"1000"` |
| 4 | RX | `0x551` | 8 | `74 A3 A3 A3 A3 A3 A3 A3` | panel ack |

Byte map of frame 3:

| offset | value | meaning |
|---|---|---|
| 0 | `0x05` | ISO-TP single frame, SF_DL = 5 |
| 1 | `0x56` | clock opcode |
| 2 | `0x31` | ASCII `'1'` — hours tens |
| 3 | `0x30` | ASCII `'0'` — hours units |
| 4 | `0x30` | ASCII `'0'` — minutes tens |
| 5 | `0x30` | ASCII `'0'` — minutes units |
| 6–7 | `00 00` | filler |

Interleaved throughout, at any point, without exception: **every `0x1C1` → `5C1 74 00 00 00 00 00 00 00`.**

Provenance of `0x56`: **no OEM radio has ever been captured sending it** — the four OEM captures end a few seconds past registration and contain no clock update. It rests on (a) the MeganeCAN log corpus, (b) `docs/WIRE-SPEC.md` §8.2 and `docs/PROTOCOL.md:416`, (c) our own bench run `captures/authclock-2026-08-03-ctx4-crx3-success-01.txt` at t=35938.045 → `551 74` at t=35939.075, and (d) `docs/BENCH-VERIFIED.md:22` — *"set to 10:00; panel read 10:17 an hour later, free-running"*, direct visual confirmation on glass. That last item is decisive: the command works and the panel free-runs the clock afterwards. See §9.

---

## 8. DELTA LIST — required changes to the AffaDisplay source tree

Ordered most-important first. All paths relative to `C:\Users\Andru\source\repos\AffaDisplay\`. Line numbers are against the current **uncommitted working tree** (HEAD = `715105b`).

---

### D1 — BLOCKING: four member functions are declared and called but never defined. The tree does not link.

| Symbol | Declared | Called from | State |
|---|---|---|---|
| `armUnauthControl(uint32_t)` | `src/core/AffaDisplayBase.h:361` | `src/core/AffaDisplayBase.cpp:386`, `:473` | **no definition anywhere** |
| `pumpUnauthControl(uint32_t)` | `src/core/AffaDisplayBase.h:362` | never called | **no definition** |
| `invalidateInFlightForSession(uint32_t)` | `src/core/AffaDisplayBase.h:413` | `src/core/AffaDisplayBase.cpp:367` | **no definition** |
| `advanceSessionEpoch()` | `src/core/AffaDisplayBase.h:414` | `src/core/AffaDisplayBase.cpp:21` (in `begin()`) | **no definition** |

`git show HEAD:src/core/AffaDisplayBase.{cpp,h}` contains none of these names; a repo-wide grep over `*.cpp *.h *.ino` finds no definition. Nothing else in this list can be tested until these exist.

Required semantics:
- `armUnauthControl(now)` — set `_unauthControlPending = true`, `_nextUnauthControlMs = now`, `_unauthControlStage = BootstrapStage::None`, `_unauthControlBusyRetries = 0`. It must **not** set `_unauthControlIssued`; that latch is set at `AffaDisplayBase.cpp:878` when the B9→BA pair is actually accepted, and D2 keys off it.
- `invalidateInFlightForSession(now)` — bump the session epoch and mark any in-flight job as belonging to the dead epoch so a late `551`/`5F1` cannot credit the new session.
- `advanceSessionEpoch()` — monotonic counter bump.
- `pumpUnauthControl(uint32_t)` — either implement it and call it from `pumpSync`, **or delete the declaration at `:362` together with the now-dead `_unauthControlStage` (`AffaDisplayBase.h:463`) and `_unauthControlBusyRetries` (`:464`)**, which are written in `begin()`/teardown and read nowhere. The bootstrap logic currently lives inline at `AffaDisplayBase.cpp:867-882`; the cleaner fix is deletion.

---

### D2 — `61 11 01` must be able to authorize. This is the bug.

**`src/core/AffaDisplayBase.cpp:337`**

```cpp
      if (f.data[2] == _profile.authRequestByte2) {
```

This single predicate routes `61 11 01` away from the good path. Per §2.0, `…cONNECT OT POWER.csv` completes an entire session on `01` with zero `00` frames. Under the current code the library sets `_syncRequestObserved`, arms one B9/BA pair, and then stops forever: `_authRequestObserved` never becomes true, so `pumpSync` bails at `:894`, `linkReady()` returns false at `:1280`, and every payload is held for `AFFA_TX_HOLD_MS` and given up as `Result::NoSync`.

**Change to:**

```cpp
      const bool authorizing = (f.data[2] == _profile.authRequestByte2) ||
                               (_profile.helloAfterBootstrapRequest && _unauthControlIssued);
      if (authorizing) {
```

where `helloAfterBootstrapRequest` is a new `bool` appended to the end of `SyncProfile` (see D4). `_unauthControlIssued` is the "we have already sent BA" evidence and is exactly the precondition §2.0 requires. Nothing else in the good branch needs to move — `:341-350` already sets `_authHelloPending` and calls `queueHello(now)`, which produces the 31 ms-staged B0 burst correctly.

**Do NOT attempt the minimal patch at `:381`** (`if (_profile.helloOnNonAuthRequest) queueHello(now);`). It queues the B0 frames but leaves `_authRequestObserved == false`, so `:894` and `:1280` still block registration, power, text and clock. The B0s would go out and nothing else ever would — a silent half-fix that looks right on a sniffer.

---

### D3 — a repeated `61 11 01` must not tear down the session it just created.

**`src/core/AffaDisplayBase.cpp:356-380`** — the `wasAuthorized` teardown block (`setSync(SyncState::Failed…)`, `invalidateInFlightForSession`, `dropRegistrations`, re-arm hold windows, clear the bootstrap latches).

Once D2 lands, `01` reaches the good branch and never gets here — but only for the frame that arrives *after* `_unauthControlIssued`. The panel emits one more `61 11 01` at ~104 ms in the capture and would keep emitting them if we were slow. Guard the block so a duplicate START cannot void a session:

```cpp
      const bool duplicateStart = (f.data[2] == kSyncStartFlag) && _unauthControlIssued;
      const bool wasAuthorized = !duplicateStart &&
                                 (_authRequestObserved || _authHelloPending ||
                                  !hasFlag(_sync, SyncState::Failed));
```

Also add, per §2.3: a `61 11 xx` arriving while `_helloPending` is true must be ignored outright — do not re-arm `_nextHelloMs`, do not restart `_helloIndex`.

---

### D4 — profile data encodes the wrong model.

**`src/carminat/CarminatConstants.h:108-123`** (`kSync`) and `src/core/AffaSyncProfile.h`.

| Line | Field | Current | Required |
|---|---|---|---|
| `CarminatConstants.h:114` | `authRequestByte2` | `0x00` | **keep `0x00`** — it stays the primary good byte; D2 adds the second door |
| `CarminatConstants.h:117` | `helloOnNonAuthRequest` | `false` | **keep `false`** if D2 lands (`01` now goes through the good branch); set `true` only if D2 is deferred, and note that alone is insufficient (see D2) |
| *(new, appended)* | `helloAfterBootstrapRequest` | — | **`true`** for `kSync` and `kLegacyMeganeCanSync` |

Append the new field at the **end** of `struct SyncProfile` in `src/core/AffaSyncProfile.h` (after `oneShotResyncOnPeerAlive`), per the existing comment convention at `:99-102` — positional aggregate initialisers downstream must keep their meaning.

The comment at `CarminatConstants.h:117` — *"01 is discovery only; the B0 announce belongs to the later 00 confirmation"* — is factually wrong against `…cONNECT OT POWER.csv` and must be replaced with the §2.0 rule.

Timing constants at `CarminatConstants.h:81-85` are **already correct** and must not be touched: `kHelloFirstDelayMs = 31`, `kHelloFrameGapMs = 31`, `kPayloadAfterRegistrationMs = 400`, `kSyncIntervalMs = 500`. `kHelloMinMs = 90` is a safe floor against the 104 ms panel cadence.

---

### D5 — registration must be triggered by the hello burst, not by the application.

**`src/core/AffaDisplayBase.cpp:846-849`** (end of `pumpHello`, where `_authHelloPending` clears) — **add** `(void)queueRegistrations();`.

Today `queueRegistrations()` has exactly three callers, none of them in `pumpHello`:
- `:1171-1187` in `enqueue()` — lazy, only when the application submits a payload
- `:1355-1371` in `pumpTx()` gate 3 — only when the queue head is already a `Payload`
- `:1310-1312` — the cached-power restore path

**If the application never renders, the library never sends `151 70` / `1F1 70`.** The OEM radio registers 0.1–0.3 ms after B0#3, unconditionally, with no application involvement (§2.1 A11–A12). This is the second structural divergence after D2 and it is why a passive/idle build can sit registered-looking and never complete a session.

`queueRegistrations()` at `:1056-1069` walks `_funcIds` in declaration order — `kFuncIds = {kIdSetText, kIdNav}` = `{0x151, 0x1F1}` (`CarminatConstants.h:150-151`) — which matches the wire order. No change needed there.

---

### D6 — `replyToPing` must be off. It doubles our `B9` rate.

**`src/carminat/CarminatConstants.h:110`** — `replyToPing`: `true` → **`false`**.

Per §3, the OEM `B9` is a free-running 500.08 ms timer with σ ≤ 0.5 ms that never flinches as the display's 504/512 ms `69` drifts past it through a full cycle, including a 0.023 ms near-collision. It is categorically not a reply.

The current code emits both. Trace with the real 504 ms panel cadence, `_nextSyncMs` armed at `AffaDisplayBase.cpp:973`, pong path at `:494-508`, `AFFA_PING_REPLY_MIN_MS = 250`:

| t | event | result |
|---|---|---|
| t | `69` → pong | **B9**; `_nextPongMs = t+250`, `_nextSyncMs = t+500` |
| t+500 | `pumpSync` fires | **B9**; `_nextSyncMs = t+1000` |
| t+504 | `69`, `_nextPongMs` long expired | **B9**, 4 ms after the last one |

Steady state: **two `B9` ~4 ms apart every ~504 ms ≈ 4/s, against the OEM's 2/s.** The anti-double guard at `:506` (`if (pong == Accepted) _nextSyncMs = now + syncIntervalMs();`) only defends in one direction and fails from the second cycle.

The justification recorded in `src/core/AffaSyncProfile.h:28-41` is explicit inference from a legacy `tick()` call site (`MeganeCAN CarminatDisplay.cpp:346`) and is flagged there as *"nobody has a spec"*. These four captures are direct measured evidence against it. Update that comment block to cite the captures.

**If `replyToPing` is retained for another family**, the guard at `:506` must become symmetric: suppress the pong when a paced `B9` went out recently, i.e. gate on `_nextSyncMs` as well as `_nextPongMs`.

---

### D7 — the `5C1` ACK must not be gated on a prior `61 11`.

**`src/core/AffaDisplayBase.cpp:263-265`**

```cpp
    if (!_passive && (!_profile.requireAuthRequest || _syncRequestObserved) &&
        !isOurTxId(static_cast<uint16_t>(f.id)) && shouldAutoAck(f))
      sendGenericAck(static_cast<uint16_t>(f.id));
```

Drop the `_syncRequestObserved` term. Per §4 the ACK is an unconditional reflex; `shouldAutoAck` (`src/carminat/CarminatDisplay.h:153-155`, `return f.id == carminat::kIdKeyPressed;` = `0x1C1`) plus `isOurTxId` already scope it correctly to exactly one id we never transmit on. In the four OEM captures the panel always leads with `61 11`, so this is latent rather than observed — but a panel that leads with `1C1 70` would go unanswered forever, and the failure mode (panel stuck retrying at 610 ms) is exactly what our bench capture shows for 12 seconds.

The rest of `sendGenericAck` (`:538-568`) is **byte-exact correct** and must not be changed: `a.id = id | _profile.replyFlag` → `0x5C1`, `a.data[0] = kAckDone` = `0x74`, `a.data[1..7] = packetFiller()` = `0x00`. Verified against `5C1 74 00 00 00 00 00 00 00`, 12/12.

---

### D8 — the `5C1` ACK must never be dropped or coalesced away.

**`src/core/AffaDisplayBase.cpp:542`** — `if (_genericAckPending && _genericAckId == id) return;`. If a Busy ACK for `0x1C1` is pending and a second `0x1C1` arrives, only one `74` is ever sent. Per §4 the contract is one ACK per frame. Replace the coalescing with a small pending count (2 is sufficient — the panel retries at 610 ms).

**`src/core/AffaDisplayBase.cpp:570-595`** — `pumpGenericAck` gives up after exactly one retry (`_genericAckBusyRetries == 0` at `:586`) and drops the ACK **silently**. Raise the retry bound and log at warning level on give-up; a lost `5C1` costs the whole session.

---

### D9 — parse ISO-TP flow control instead of constant-matching it.

**`src/core/AffaDisplayBase.cpp:526-527`** matches `f.data[0..2]` against `kAckPartial0/1/2` = `0x30/0x01/0x00` (`src/core/AffaConstants.h:48-50`). An FC with any other `BS` or `STmin` (e.g. `30 00 14`) is not recognised, falls to `:532-534` → `finishJob(Result::SendFailed)`, and `retryable()` at `:1248` deliberately refuses to retry it. The transfer dies.

Change to: match `(f.data[0] & 0xF0) == 0x30`, then switch on the FS nibble — `0` CTS, `1` WAIT, `2` OVERFLOW/abort — and honour `data[1]` as BlockSize and `data[2]` as STmin. The panel only ever sends `30 01 00` in this corpus, so this is hardening, not a live defect.

The existing BS=1 behaviour is otherwise **correct and must be preserved**: `creditAck(false)` at `:1433-1455` emits exactly one CF and re-enters `WaitAck` at `:1427`, structurally locking the sender to one CF per FC. That is precisely what §5 requires (43 FCs for 43 CFs). Do not "optimise" it into a burst.

Note that `src/proto/IsoTp.cpp` is **not** on the transmit path (`src/proto/IsoTp.h:14-17`); `pumpTx` builds frames inline. Its `Reassembler` never generates FC — correct for now, since all display-originated traffic is single-frame (§5).

---

### D10 — pipeline the two registration probes.

**`src/core/AffaDisplayBase.cpp:1056-1069`** / the queue in `pumpTx`.

The OEM radio sends `151 70` then `1F1 70` **0.29 ms apart, before either is acknowledged**, and receives `551 74` then `5F1 74`. Our implementation serialises: `151 70` → wait `551 74` → `1F1 70` → wait `5F1 74`. Functionally equivalent, observably different on a sniffer, and it costs one extra round trip inside a latency-sensitive window.

Lower priority than D1–D9 — nothing is known to break — but it is a real wire-shape divergence and it makes bench captures harder to compare against the OEM traces.

---

### D11 — cosmetic wire-fidelity items.

- **`src/core/AffaDisplayBase.cpp:774`** — `sendAlive()` hard-codes `data[1] = 0x00`. The OEM radio's very first frame after a cold co-boot is `B9 **01**` (once, ever). Emitting `B9 01` as the first alive frame after `begin()` would be wire-identical; there is no evidence the panel cares. Optional.
- **`src/carminat/CarminatConstants.h:63-67`** — `kHello` = three identical `B0 14 11 00 1F 00 00 00` frames is **confirmed correct 4/4** (exactly three, never `70 1A 11 …`, which appears in **zero** OEM frames). `kLegacyHello` at `:72-76` should stay behind its explicit opt-in and its comment should record that the captured corpus does not contain it.

---

### D12 — example and test alignment.

- **`examples/06_authclock/main.cpp:80`** — `kEnable01Compatibility = false` → **`true`**. This is the single highest-value one-line change to put on the wire: the panel on our bench emitted `61 11 01` for 15 seconds and only registered when it eventually volunteered `61 11 00`, and §2.0 proves `01` is a valid trigger.
- **`examples/06_authclock/main.cpp`** goal FSM — the working-tree 0.4.1 chain `WaitSync → Register → SettleBeforePower(400 ms) → PowerOn(03 52 09) → WarmUp(50 ms) → SetClock → Done` matches §6/§7 and is correct. It has **never been flashed**. The board at `192.168.100.85` is still running HEAD 0.3.0, which has no `03 52` step at all (`grep -c "03 52"` over the success capture returns 0) — which is exactly why the clock command was ACKed at t=35939 and never appeared on glass.
- **`test/test_sync_profiles/test_sync_profiles.cpp`** — the strict-`00` cases assert the behaviour D2 removes and will fail. Rewrite them to assert the §2.0 rule: a `61 11 01` arriving **after** our `BA` produces the full B0×3 burst and opens `linkReady()`; a `61 11 01` arriving **before** any `BA` produces only the bounded B9/BA pair.
- **`test/test_session_epoch/test_session_epoch.cpp`** (untracked, 408 lines, 7 tests) has **never been executed** — the prior session ended on the approval request for `pio test -e native -f test_session_epoch`. Run it once D1 makes the tree link.
- Add a golden-vector test for the §7 pair: `151 03 52 09 00 00 00 00 00` then `151 05 56 31 30 30 30 00 00`. The existing vector `kSetTime1234` (`test/test_carminat_wire/test_carminat_wire.cpp:44-46`) covers the clock frame; the zero-padded power frame is already flipped from MeganeCAN's `FF FF` spelling and is correct.

---

## 9. Open questions and unverified claims

Clearly separated from everything above.

### Confirmed beyond reasonable doubt
Filler→sender mapping (0/579 exceptions, proven by same-message/different-filler); responder = requester + 0x400; exactly three `B0` frames at 31 ms; the 31 ms anchor on the hello rather than on `BA` (0.79 ms vs 80 ms spread); `61 11 01` triggers the burst identically to `61 11 00`; `151 70`/`1F1 70` < 1 ms after B0#3; 12/12 `1C1` acknowledged; 400 ms ± 0.5 ms from `5F1 74`; `B9` free-runs at 500.1 ms; `B9` and `69` are mutually independent (phase wraps 0→500 ms).

### Unverified — flagged, do not treat as settled

1. **No OEM radio has ever been captured sending `0x56` (setTime)** — but it is CONFIRMED ON GLASS: 2026-08-04, 06_authclock 0.6.0, user read `10:00` off a real Carminat after `151 05 56 31 30 30 30`. The command is proven; only its OEM provenance is not. The four OEM captures end a few seconds past registration. The clock frame rests on the MeganeCAN corpus, our own bench ACK, and the visual confirmation in `docs/BENCH-VERIFIED.md:22`. Strong, but not OEM-observed.
2. **`02 54 01` and `02 54 03` are undecoded.** `0x54` is the close/hide opcode; the two arguments are unresolved. Whether they are required before a clock or text render is unknown — our bench run set the clock without them.
3. **`1C1 02 64 0F` is undecoded** beyond "capability report". Three bytes, one sample per capture, always identical.
4. **The 5 selector bytes in the text header `77 09 55 FF 31 01` are one sample.** Do not generalise. The ASCII payload `"   1056 "` is plausibly an FM frequency (105.6) and plausibly a track/clock field — one sample cannot settle it, and it is **not** a clock update.
5. **`0x1F1` pictogram offsets 1–2 (`0B 00`) and 10–11 (`00 01`) are unresolved.** The 48×48 geometry at offsets 12–13 is certain; the surrounding header is not.
6. **"Must ACK `1C1`" is unfalsifiable from this corpus.** There is no negative case — no unacked `1C1` and no observed consequence of omission in the OEM traces. The 12 s stall in our own bench capture is the only evidence that omitting it is harmful, and that is a different (our) implementation.
7. **What happens if the radio never answers a `61 11 xx`** is unknown beyond "the panel repeats at 104 ms". No capture contains a panel giving up, and no timeout is observable.
8. **Session teardown on a post-registration `61 11 xx`** is inferred, not observed. In every capture the panel stops sending `61 11` permanently once registered.
9. **`69 01`** appears only in our own bench capture, only inside the un-registered window. Not in the OEM corpus. Treated as equivalent to `69 00`; unconfirmed.
10. **`B9 01`** appears exactly once, as the first frame of one cold co-boot. Interpreted as a boot marker. Whether the panel reads the low bit is unknown.
11. **The role labels are proven; the physical identity is not.** Nothing in the CSVs distinguishes "genuine Renault radio" from "an ESP32 emulating one". The `0x00`-padding node is the content source and the `0xA3` node is the renderer — that much is certain.
12. **`docs/captures/aknowledge offed display.csv` has two timestamp discontinuities** (−62.33 s at L2→L3, +80.9 s at 4026994→84945066). Cross-segment deltas in that file are meaningless and none were used above.
13. **The `~504 ms` `69` period is not a constant.** With the display ON it is bimodal 504/512 ms, mean 507.83, σ 4.60. Any liveness timeout built on a tight `69` period will false-trip.
14. **STmin behaviour under a nonzero value is untested.** Only `30 01 00` was ever observed. D9 hardens against it blind.
15. **None of §8 has been flashed or run on hardware.** The board is on HEAD 0.3.0; the working tree is an uncommitted 0.4.1 with a different hello spelling, an added power-on step, and D12's compatibility flag still off. The tree does not currently link (D1).