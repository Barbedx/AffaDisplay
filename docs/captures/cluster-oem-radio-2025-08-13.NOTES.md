# The cluster capture, read against the library (2026-09-27)

Raw file: `cluster-oem-radio-2025-08-13.txt`. It has 46 timestamped frames covering 1.5 s,
plus one clock frame from a separate, undated snippet. The capture is third-party, it is the
only sample we have, and nobody here has run it on hardware. The poster's comments are
interpretations and are checked below. **This file is a reading, not a fix:** no code or
spec was changed. The contradictions are listed in §3.

## 1. Frame by frame

| t (s,ms) | Frame | Poster says | What it most likely is | Confidence |
|---|---|---|---|---|
| 4.339–4.410 | `3AF 2 5A 01` ×5 | radio frame | Radio **sync request**, the `XA` byte; Carminat's is `BA 00`. | high |
| 4.409 | `3CF 2 61 23` | cluster frame | **The cluster's answer to the request**, Carminat's `61 11` with `23` in place of `11`. **The library does not recognise it** (§3.1). | high on role, byte meaning unknown |
| 4.412 / .442 / .472 | `3AF 8 50 29 00 23 00 00 00 69` ×3 | "init request" | Radio **hello burst**, the `X0` byte (Carminat sends `B0 …`). Three frames 30 ms apart, the same as Carminat's 31 ms. Bytes 1–7 have no known meaning. The `23` may echo the `61 23`, as Carminat's `B0 14 11` seems to echo `61 11`. Unproven. | high on role, low on content |
| 4.419 | `1C1 8 70 84…` | "cluster's answer to init" | **Wrong reading.** This is not an answer. The cluster is **opening its own channel**: a `70` probe with `84` = cluster filler. It came 7 ms after the FIRST hello. | high |
| 4.419 | `5C1 8 74 FF…` | "radio request 1(1)" | **Wrong reading.** This is the radio's **ACK** of the cluster's `1C1` (`74` = done, `FF` = radio filler). It lands in the same ms, so the 1 ms timestamps cannot order the pair. | high |
| 4.473 | `121 70 FF…`, `1B1 70 FF…` | "radio request 2/3" | Correct: the radio registers its two functions, 1 ms after hello #3. | high |
| 4.475 | `521 74 84…`, `5B1 74 84…` | "cluster answers" | Correct: the cluster ACKs both registrations. | high |
| 4.818 / 5.319 / 5.820 | `3AF 2 59 00` | — | Radio **alive**, the `X9` byte, **period 501 ms**. | high |
| +15–17 ms | `3BF 2 49 00` | — | A **third node** answering with the same `X9` shape (`4x`). **Unknown who**: a second part of the cluster, a nav computer, or the radio itself on a second id. | **unknown** |
| +1–13 ms after 3BF | `3CF 1 69` | — | Cluster **alive** (Carminat's panel also uses `69`), DLC 1. | high |
| 4.873 | `1B1 8 03 52 00 00 FF…` | — | Radio **display control**, SF_DL 3. It arrives **exactly 400 ms after registration**, which is Carminat's measured gap. The library reads `52 00` as "power OFF". Why would an OEM radio switch the display off right after connecting? More likely `00` is a mode or state value. | high on timing, **low on meaning** |
| 4.877 | `5B1 74 84…` | — | Cluster ACK. | high |
| 4.893 | `1C1 8 02 64 0F 84…` | — | The cluster's **one-shot reply to the radio's first `03 52`**. The Carminat panel does the same: in all 11 connection CSVs of the OEM corpus it appears **exactly once**, 5–20 ms after the first `151 03 52 xx`, with no key pressed. It is **not a key**, since keys are `03 89 xx yy` (`AffaConstants.h:35`). MegaOpen `HANDOFF-2026-09-23-panel-registered.md` calls it "stalk keys", which is wrong. Most likely a status or "display ready" report, `64` being the panel's command byte and `0F` a bitmask. Carminat follows it with `05 63 "2111"`/`"0037"`, ASCII digits that differ per unit (version?). The cluster sends nothing like that within 1.5 s. | role high, bytes low |
| 4.893 | `5C1 74 FF…` | — | Radio ACK. | high |
| every ~100 ms | `2E8 3 91 00 00` (15 frames) | — | **Unknown**: sender, meaning, and whether it is AFFA at all. It first appears 31 ms after hello #2. | **unknown** |
| every ~500 ms | `3FF 2 92 01` (3 frames) | — | **Unknown**. Like `3AF`/`3BF`/`3CF` it is `3xF`, and its first byte also ends in 1/2. It may belong to the same sync family. | **unknown** |
| — | `3EF 3 A6 0C 03` | "Time, 0C hours 03 min" | **Radio → cluster: set the cluster's clock** to 12:03, `A6 hh mm` in binary. It is a raw frame with no PCI and no ACK, outside AFFA. (The "20:06/20:07" lines are the post dates, not related.) We only lack the context around it: when and how often it is sent, and whether the cluster must be registered first. The bench failure in PROTOCOL-NOTES §9.4 was on an **UpdateList** panel, a different device, so it proves nothing here. | high on meaning, context unknown |

**How much we understand:** 25 of the 46 frames are the AFFA handshake and ACKs, and we know
the **role** of every one of them. We know the **content** of almost none past the first byte:
the hello bytes, `61 23`, `52 00` and `64 0F`. The other **21 frames (46 %)** come from three
ids, `2E8`, `3FF` and `3BF`, and we understand nothing about them.

## 2. What the capture actually shows: Carminat's opening, re-byted

```
radio   3AF XA   request          (Carminat: BA 00       | cluster: 5A 01)
panel   3CF 61 xx answer          (Carminat: 61 11 xx    | cluster: 61 23)
radio   3AF X0 … hello ×3, ~30 ms (Carminat: B0 …         | cluster: 50 29 00 23 …)
panel   1C1 70   opens its channel, radio ACKs on 5C1
radio   121/1B1 70  registers, panel ACKs on |0x400
        +400 ms  1B1 03 52 …      first payload
radio   3AF X9   alive every 500 ms; panel 3CF 69
```

The one visible difference in shape: the cluster-side radio sent its hello **3 ms** after the
`61 23`. Carminat's radio waits ~31 ms after `61 11`.

## 3. Where the library contradicts this capture

1. **The opening can never start.** `handleSyncFrame()` (`src/core/AffaSync.cpp:24`) only acts
   on `61` + `kSyncRequestByte1` = `0x11` (`AffaConstants.h:85`), so the cluster's `61 23`
   is ignored. `docs/NOTES.md §1.1` says *"the cluster sends only `3CF 1 69`"* and that the
   trigger is "either the `69` or nothing". **That is false.** The `61 23` line was dropped
   when the capture was quoted. The trigger is `61 23`, and the fix belongs in the profile
   (a request-token byte), not in new FSM logic.
2. **`ClusterConstants.h`: "`waitForPanel` INVERTED FROM CARMINAT … the cluster never sends a
   `61 11`".** Not inverted: the order is exactly Carminat's (request → `61 xx` → hello). The
   right model is Carminat's flags with a different token. `requireAuthRequest = false` is
   built on the same wrong premise.
3. **`syncIntervalMs = 0` → `AFFA_SYNC_INTERVAL_MS` = 1000**, with the comment "cadence not in
   the capture". The capture shows **500 ms** (`59 00` at 4.818 / 5.319 / 5.820), the same as
   Carminat's B9.
4. **DLC.** The capture's radio sends `59 00` and `5A 01` at **DLC 2**. The library always
   sends alive and request at DLC 8, padded (`AffaSync.cpp:339,350`). The hello and
   registrations are DLC 8 in both. Nobody knows whether the cluster cares.
5. **`kPayloadAfterRegistrationMs`: "borrowed … the capture contains NO payload at all".** It
   does contain one: `1B1 03 52 00 00`, 400 ms after registration. So the value is
   **corroborated**, not borrowed. The comment is wrong and the number is right.
6. **`kPowerOff = 0x00 // [CAP]`**. The byte is in the capture; the meaning "off" is not (§1).
7. Things that match: ids and `|0x400`, the hello text and its 30 ms gap, the `FF` radio
   filler (the ACKs in `AffaSync.cpp:270,302` use `packetFiller()` = `FF`), no pong to `69`,
   registration gated on the peer's `1C1` opening, and the two functions `121`/`1B1`.

MegaOpen itself cannot select this panel at all: `displayType` accepts only
`carminat | updatelist | updatelist_menu`.

## 4. What to record when a cluster is on the bench

What each item in the next capture should answer:

- **Start before power.** Record from before the radio and the cluster power up, so the
  capture shows who speaks first and what came before `5A 01`. Here `5A 01` arrives three
  times in 6 ms, which may be CAN retransmits of an unACKed frame rather than three sends.
  Say whether the sniffer ACKs (normal mode) or is listen-only.
- **Timestamps finer than 1 ms** if the tool allows it. Several request/ACK pairs share a ms
  here.
- **Cluster alone, then radio alone.** This settles who sends `2E8`, `3FF` and `3BF`. The
  bench can also do it: with only the cluster on the bus, whatever is still transmitting
  belongs to the cluster.
- **Unplug and replug the cluster while running.** Does it re-send `61 23`? What does the
  radio do? Is the token ever something other than `23`?
- **Text.** Neither side sends text anywhere in this capture: `121` is registered, then stays
  silent for the whole 1.4 s that follow. Record one run in which the
  radio visibly writes to the cluster (station name, track, volume), so we get the
  setText encoding.
- **Display on/off.** Switch the radio off and on, change source, dim the lights. Watch which
  `1B1 03 52 xx` values appear, which gives the real meaning of `00`.
- **Keys and knobs on the radio or stalk.** See what `1C1` carries besides `02 64 0F`.
- **Change the clock** in the radio menu, with the time written down, to confirm `3EF A6 hh mm`
  and find who sends it.
- **Longer than 1.5 s.** At least a minute idle, to see whether `2E8`/`3FF` stay constant and
  whether `61 23` ever repeats.
- **Save it raw** (CSV/TRC from the tool, never retyped) into this folder, with a line
  describing the car and what was done.
