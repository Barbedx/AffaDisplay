# AFFA display protocol — normative specification

Renault OEM dash panels over CAN — Carminat and UpdateList. This document is the **wire
contract**: what goes on the bus, in what order, and what the other end does with it.

**Read §2 first.** Two different protocols are involved and the `AFFA2`/`AFFA3` labels
attached to them in the wild are not reliable.

Rebuilt 2026-07-29 from four independent sources rather than from any single
implementation, because every implementation of this protocol contains at least one bug
that the panel tolerates, and copying one wholesale copies its bugs as facts.

| tag | source |
|---|---|
| `[REF]` | `MeganeCAN/notes/archive_mhroczny/affa3.{c,h}` — third-party implementation of the **UpdateList** dialect (§2.1). Independent; **names** constants everyone else open-codes. |
| `[IMPL]` | `MeganeCAN/src/display/**` — driver proven against a real Carminat panel. |
| `[EMU]` | `MeganeCAN/src/vdisplay/**` — panel emulator. States the contract from the *panel's* side. |
| `[CAP]` | literal bytes in a capture log against a real panel. |
| `[OEM]` | captured from a factory head unit or cluster — i.e. not our code. Strongest evidence. |
| `[DERIVED]` | hand-executed from a builder. **No capture. Treat as unproven.** |

**Rule for readers:** where sources disagree, this document states both and names which is
which. Do not "fix" a value tagged `[CAP]` or `[OEM]` because it looks wrong. Several are
wrong and the panel depends on them.

---

## 1. Link layer

- **500 kbit/s**, standard 11-bit identifiers, **DLC always 8**. Short payloads are padded.
- No checksum, no sequence number, no addressing beyond the CAN id.
- **Every frame is individually acknowledged at the application layer.** See §4.

### 1.1 Filler is per-node and carries no meaning

| node | filler |
|---|---|
| our Carminat driver | `0x00` |
| our UpdateList driver | `0x81` |
| `[REF]` AFFA2 | `0x81` |
| bench Carminat panel | `0xA3` |
| OEM cluster | `0x84` |
| OEM radio | `0xFF` |

**Never match on, validate, or assert a received filler byte.** A decoder that does will
work against one panel and fail against the next.

---

## 2. Two dialects, and the naming is a trap

### 2.1 What is actually different

There are **two head-unit dialects**, and they are named after the RADIO, not the display:

| dialect | the radio | the display it drives |
|---|---|---|
| **Carminat** | the Carminat navigation head unit — **our case** | the Carminat panel |
| **UpdateList** | the other Renault radio family | an **8-character segment** screen *or* an LCD |

**THE CARMINAT DISPLAY UNDERSTANDS BOTH.** It is the superset device: it answers the
Carminat dialect and the UpdateList dialect. So a Carminat panel on the bench can be driven
either way, and "the panel replied" does not by itself tell you which dialect it replied
to.

The reverse does not hold — an UpdateList segment display has no menu screen, no
fullscreen, no popup and no clock command, so most of §5 simply does not exist for it.

### 2.2 The names `AFFA2` and `AFFA3` are unreliable — do not navigate by them

- The Carminat panel is *also* called **"AFFA3 NAV"** in places.
- `archive_mhroczny/affa3.c` is named `affa3` and implements the **UpdateList** dialect.
- Our own tree uses `AFFA2`/`AFFA3` inconsistently in comments.

**Navigate by the identifier set in §2.3, never by the AFFA2/AFFA3 label.** Every table in
this document is keyed on the dialect name, and the numbers are what disambiguate.

> Where an earlier reading of this material reported the Carminat constants as
> "disagreeing" with the mhroczny reference, that was wrong. They are not competing
> accounts of one protocol — they are two protocols. **Every Carminat value here is
> confirmed working against a real panel in MeganeCAN.** Do not reconcile them.

### 2.3 Identifiers

Picking the wrong column is silent — the bus simply never answers.

| | **Carminat** *(ours)* | **UpdateList** | Cluster |
|---|---|---|---|
| sync, us → panel | `0x3AF` | `0x3DF` | `0x3AF` |
| sync, panel → us | `0x3CF` | `0x3CF` | `0x3CF` |
| text / screen | `0x151` | `0x121` | `0x121` (encoding unknown) |
| display control | `0x151` *(same id)* | `0x1B1` | `0x1B1` |
| second registered fn | `0x1F1` (NAV) | — | — |
| keys, panel → us | `0x1C1` | `0x0A9` | `0x1C1` |
| filler | `0x00` | `0x81` | `0xFF` |
| alive / sync-request opcode | `0xB9` / `0xBA` | `0x79` / `0x7A` | `0x59` / `0x5A` |
| hello frames | 3 | 1 | 3 |

**Reply id = `funcId | 0x400`.** `0x151 → 0x551`, `0x1F1 → 0x5F1`, `0x1C1 → 0x5C1`.

> **The `| 0x400` rule does NOT apply to the sync pair.** `0x3CF` is not `0x3AF | 0x400`.
> The sync ids are a hard-coded, unrelated pair and must be tested *before* the reply-flag
> test. `[REF][IMPL]`

> **Reply matching is a bit test, not an equality test.** Every implementation checks
> `id & 0x400` and then strips the bit. Any id in `0x400..0x7FF` therefore enters the ACK
> branch and is consumed before text or key handling ever sees it. `[IMPL]`

---

## 3. Session

### 3.0 Carminat opening and recovery -- authoritative captured profile

The Carminat panel starts the session. The ESP32 is silent after boot until a complete
`3CF 61 11 xx` (DLC >= 3) arrives. On a single-ended monitor, `Dir=Rx` only means that
the monitor received the frame; direction is determined primarily by the IDs:
`3CF` and `1C1` are panel -> ESP32, while `3AF`, `151`, `1F1`, and `5C1` are
ESP32 -> panel. The usual fillers corroborate that reading (`A3` from the panel,
`00` from this driver), but a received filler is never a validation rule.

The default `CarminatHelloProfile::CapturedB0x3` is the sequence measured in the supplied
real-display monitor captures:

```text
RX  3CF  61 11 00 A3 A3 A3 A3 A3
   +31 ms  TX 3AF  B0 14 11 00 1F 00 00 00
             RX 1C1  70 A3 A3 A3 A3 A3 A3 A3
             TX 5C1  74 00 00 00 00 00 00 00
   +31 ms  TX 3AF  B0 14 11 00 1F 00 00 00
   +31 ms  TX 3AF  B0 14 11 00 1F 00 00 00
```

One opening frame is offered to `ICanLink` at a time; the `1C1 -> 5C1` control ACK is allowed
to interleave between B0 frames. Only after the third B0 has been accepted by the link may
the application side register `151`, wait for `551 74`, register `1F1`, and wait for
`5F1 74` -- strictly in that order. Wait about 400 ms after the final registration ACK
before the zero-padded display-on frame `151 03 52 09 00 00 00 00 00`. A clock request for
10:00 is `151 05 56 31 30 30 30 00 00`, and it too requires `551 74`.

`61 11 01` is discovery only in the strict Carminat profile: issue one nonblocking
`3AF B9 ...` + `3AF BA ...` pair, keep registration/render/power/time locked, and wait
for `61 11 00`. Repeated `01` frames never create a periodic BA stream. A full request
also opens the panel control plane, so `1C1 70 -> 5C1 74` remains permitted during discovery;
that ACK is not authorization for application output.

Healthy traffic is a paced roughly-500-ms `3AF B9` / `3CF 69` liveness exchange. A lost
session drops registrations and held application work waits for the next panel-originated
opening; recovery replays the selected profile, never a timer-driven BA probe.

The historical `CarminatHelloProfile::MeganeCanLegacy70B0B0` profile is explicit
compatibility only. It preserves the old MeganeCAN source's immediate
`70 1A 11 ...`, `B0 ...`, `B0 ...` opening for a panel that has demonstrated that
requirement. It is not the capture-backed default, and selecting it does not weaken the
strict `61 11 00` gate or permit periodic BA.

### 3.1 State

Four flags. `FAILED = 0x01`, `PEER_ALIVE = 0x02`, `START = 0x04`, `FUNCSREG = 0x08`.
Initial state is `FAILED`. `[REF][IMPL]`

### 3.2 Historical MeganeCAN transmit tick (not the current profile)

The original driver sent this heartbeat once per second on the sync id:

```
3AF   B9 00 00 00 00 00 00 00      "we are alive"
```

and additionally, **while `FAILED` or `START` is set**:

```
3AF   BA 00 00 00 00 00 00 00      "please sync"
```

then cleared `START`. `[IMPL]`

That is historical behaviour, not the Carminat library's retry policy. The current
capture-backed rules are in section 3.0: one discovery `B9` + `BA` pair for `01`, no
periodic BA, a roughly-500-ms heartbeat once the panel has opened the session, and no
application output before the later good `00`.

> `requestArg` (`data[1]`) is `0x00` on Carminat **and it is filler, not an argument**.
> UpdateList's `7A 01` carries a genuine `0x01`. The two look symmetric on the wire and
> are not. `[REF][IMPL]`

### 3.3 What the panel sends, unprompted

```
3CF   61 11 xx A3 A3 A3 A3 A3      sync request
3CF   69 00 A3 A3 A3 A3 A3 A3      peer-alive ping, roughly every 500 ms when healthy
```

Match rules — **and the loose matching is deliberate**:

- Sync request: `data[0] == 0x61 && data[1] == 0x11`, with `len >= 3` before reading
  `data[2]`. Bytes 3..7 are filler.
- Peer-alive: `data[0] == 0x69` **only**. Bytes 1..7 are never examined, and DLC may be
  as low as 1. `[REF][IMPL][OEM]`

#### `data[2]` of `61 11` — bootstrap versus authorization

| value | meaning |
|---|---|
| `0x00` | authorization request: run the selected hello profile, then registration and output may proceed |
| **`0x01`** | **discovery only: one bounded `B9` + `BA`, no hello/register/output** |
| any other `xx` | ignore for application authorization and output |

`0x01` means the panel is asking us to bootstrap its session, not that application traffic
may resume. The historical driver treated this as a registration-loss indication and may
receive it repeatedly while the panel is not ACKed at the CAN link layer.

The legacy behaviour latches `START`, re-arms `BA` on its next tick, and leaves `FUNCSREG`
set. On a retransmitted request that turns into a `BA` stream while the head unit still
believes it is registered. The library does the opposite: it holds/drops registration and
application payloads, sends one nonblocking `B9` + `BA` control sequence, and waits for a
later full `61 11 00`. That good request is what restarts registration from function index 0
and permits queued display output.

**Read it only after checking `len >= 3`.** Short DLCs are real on this channel; reading
`data[2]` blind reads uninitialised memory. `[REF][IMPL][CAP]`

> **A cluster can send `61 23` instead of `61 11`.** Matching `data[1] == 0x11` means such
> a peer is never answered. Known hole, not a bug — the Carminat panel sends `61 11`.
> `[OEM]`

### 3.4 Captured hello default and the historical compatibility profile

For the default Carminat profile, a good `61 11 00` emits **three identical**
`B0 14 11 00 1F 00 00 00` frames at approximately +31 ms, +62 ms, and +93 ms from
the request. The gaps are protocol timing, not a blocking delay: only one B0 is offered at
a time so the panel's `1C1 -> 5C1` control exchange can occur between them. The default does
not send either B0 or `70 1A 11` for `01`.

```text
TX  3AF  B0 14 11 00 1F 00 00 00   // +31 ms
TX  3AF  B0 14 11 00 1F 00 00 00   // +62 ms
TX  3AF  B0 14 11 00 1F 00 00 00   // +93 ms
```

`CarminatHelloProfile::MeganeCanLegacy70B0B0` is a separately selectable compatibility
profile. It preserves the immediate historical MeganeCAN source sequence below, which was
proven on a real panel, but it is not inferred from or silently mixed into the capture-backed
default:

```text
TX  3AF  70 1A 11 00 00 00 00 01
TX  3AF  B0 14 11 00 1F 00 00 00
TX  3AF  B0 14 11 00 1F 00 00 00
```

Both profiles retain the same strict good-`00` authorization and bounded recovery policy.
They differ only in the three opening frames and their timing.

### 3.5 Function registration (`FUNCSREG`) — strictly sequential

`FUNCSREG` is a one-byte payload `{0x70}` on each registered function ID. It is unrelated
to the `70 1A 11 ...` sync frame used only by the optional legacy hello profile.

After the good-`00` opening has completed, registration is held behind the application gate.
The library performs it before the queued application payload; the 10:00 POC performs it
explicitly before display power and time. A preceding `61 11 01` never starts registration or
a time/text/power command. Our `funcs[]` is `{0x151, 0x1F1}`, in that order, and the order is
on the wire:

```
151   70 00 00 00 00 00 00 00      →  wait for ACK on 551
1F1   70 00 00 00 00 00 00 00      →  wait for ACK on 5F1
```

Only after **every** entry is acknowledged is `FUNCSREG` set. After the final `5F1 74`,
wait approximately 400 ms before `151 03 52 09 00 00 00 00 00` (display ON).

#### Registration is BIDIRECTIONAL, and the filler is what tells you which way

Captured `22:37:41` — three `70` probes in one second, each individually acknowledged. A
sniffer logs all six as `[RX]`, so the ids alone do not say who sent what. **The filler
does** (§1.1): `0x00` is the radio's signature, `0xA3` is the panel's.

```
0x151  70 00 …   radio  →   0x551  74 A3 …   panel ACKs      radio registers
0x1F1  70 00 …   radio  →   0x5F1  74 A3 …   panel ACKs      radio registers
0x1C1  70 A3 …   PANEL  →   0x5C1  74 00 …   RADIO ACKs      PANEL registers
```

**So `0x1C1` is the panel's channel to register, not ours**, and `funcs[] = {0x151, 0x1F1}`
is correct. That is exactly right for a key channel: the joystick is wired to the panel, so
keys flow one way — the panel decodes the stick, sends on `0x1C1`, we answer `74` on
`0x5C1`, and the key goes up to the application.

> **THE FILLER IS A DIRECTION SIGNATURE.** Never validate it (§1.1), but *do* read it: on a
> single-ended capture where both directions share an id family, the pad byte is often the
> only thing that says who transmitted. Misreading `1C1 70 A3 …` as ours would put us
> transmitting on the panel's own channel.

This `70` exchange is the only re-sync event in the corpus. It occurred twice, one second
apart, **immediately after a transfer was truncated mid-flight**, and was followed by
`03 52 09` (display ON) and a full screen redraw from the first frame. So the factory
recovery for a broken session is: re-register, re-power the display, redraw. `[OEM]`

#### Registration is strictly sequential — one probe, one ACK, then the next

`70` on the funcId → **wait** for `74` on `funcId | 0x400` → next funcId. `FUNCSREG` is
latched on those ACKs and on nothing else.

> **Registration therefore cannot be performed blind.** Putting the probes on the wire
> without reading the replies latches nothing, leaves unanswered probes on the bus, and
> achieves exactly zero. Only the *hello* (§3.4) is fire-and-forget — it is an unconditional
> answer to an unconditional request and carries no state.

> **Any failure aborts the whole pass and the flag is never set**, so the next render
> retries the list from index 0. `0x1F1` is registered but never written to — if the panel
> does not ACK on `0x5F1`, registration stalls at a 2 s timeout on every single render,
> for ever, and nothing is ever drawn. This is a real and easily-hit failure mode.

### 3.6 Peer-alive watchdog

- **Re-armed by** any `0x69` on the sync-reply id.
- **Timeout:** a 5000-ms wall-clock deadline.
- **On expiry:** state is *assigned* `FAILED` — which clears `PEER_ALIVE`, `START` **and
  `FUNCSREG` together**. Registration must be redone before the next render. `[IMPL]`

For the Carminat library, `69` remains liveness only: a bare ping before any complete
`61 11 xx` produces no session/control traffic and can never authorize registration or
application output.

#### Captured heartbeat cadence

The normal captured liveness pair is approximately `3CF 69 ...` and `3AF B9 ...` every
500 ms. A B9 offered in reply to a received 69 is paced/coalesced so retransmitted 69 frames
cannot become a transmit storm. No B9 path is allowed to append a BA; BA remains the single
discovery response to `61 11 01`.

---

## 4. Transport — ISO-TP shaped, but it is not ISO 15765-2

Close enough to fool a standard stack, and different in four ways that each break it.

### 4.1 Framing

```
frame 0   : 8 bytes of raw payload.   NO PCI IS INSERTED BY THE TRANSPORT.
frame N>0 : 0x20 + N, then 7 bytes of payload.
```

The `10 <len>` that appears at the head of a multi-frame message is written by the
**command builder** as payload bytes `[0]` and `[1]`. The transport does not know it is
there. `[REF][IMPL]`

Consequences:

- **Frame 0 carries 8 payload bytes**, not 6 or 7 as real ISO-TP would.
- **The length field is hand-written per command and is not derived from the payload.**
  Two commands declare it wrong; see §5.
- **The continuation counter does not wrap.** `0x20 + N` monotonic, so continuation 16
  would emit `0x30` — which collides with the PARTIAL opcode. **Hard ceiling: 15
  continuations = `8 + 15×7` = 113 payload bytes.** Real ISO-TP wraps `0x2F → 0x20`.
  (The OEM *does* wrap: `0x1F1` continuations run `…2E 2F 20 21…`. So an OEM message
  longer than 113 bytes is legal and we cannot send one. `[OEM]`)

### 4.2 The reply channel — `30 01 00` is NOT flow control

After **every** frame, the receiver answers on `funcId | 0x400`:

| reply | meaning | sender does |
|---|---|---|
| `74 …` | **DONE** | stop immediately and report success |
| `30 01 00 …` | **PARTIAL** — "send the next one" | continue |
| anything else | error | abort |
| *(nothing, 2000 ms)* | timeout | abort |

There is no BlockSize, no STmin, no CTS/WAIT/OVFLW. It is strictly **stop-and-wait: one
frame, one reply**, including for single-frame messages. `[REF][IMPL][EMU]`

> **`0x74` means STOP, not "all received".** If the panel DONEs early the sender truncates
> the message and reports **success**. This is the normal path for `showMenu`: the panel
> DONEs as soon as it holds the declared 90 content bytes, so 13 frames go out and the
> 14th never does. `[CAP]`

> **PARTIAL on the last frame is reported as failure** even though every byte was
> delivered. Callers must ignore that error. `[IMPL][EMU]`

### 4.3 Receive direction is asymmetric — deliberately

Reassembly keeps the **first two header bytes in the buffer**, so *every* decode offset is
`content offset + 2`. Mixing the two origins is the single most common bug in this
protocol. This document uses **payload offsets** (offset 0 = the `0x10` byte) throughout.

> **12-bit length must be parsed on receive:** `need = ((data[0] & 0x0F) << 8) | data[1]`.
> An implementation that matches `data[0] == 0x10` exactly silently drops every OEM
> message longer than 255 bytes — the OEM `0x1F1` nav message begins `11 2E` = **302
> bytes**. `[OEM]`

### 4.4 Commands that bypass the transport entirely

`highlightItem`, `hidePopup`, `hideFullscreenText` and `showInfoMenu` are written straight
to the bus: no sync gate, no ACK wait, no `funcs[]` lookup. Routing them through the
transport would add a 2 s stall each on a quiet bus. `[IMPL]`

---

## 5. Commands — Carminat, all on `0x151`

**One id carries everything.** Commands are distinguished only by the first content byte:
`0x02` close · `0x03` state · `0x05` time · `0x07` highlight · `0x21` screen ·
`0x74`/`0x76`/`0x77` text family.

### 5.1 Single-frame commands

Byte 0 is the length, byte 1 is the opcode.

```
02 54 03 00 00 00 00 00        close window / dismiss popup          [CAP][OEM]
03 52 09 00 00 00 00 00        display ON        (0x00 = OFF)        [CAP]
05 56 H H M M 00 00            set clock, 4 ASCII digits "HHMM"      [CAP]
07 29 01 7E 80 00 00 00        highlight row 0   (0x7F = row 1)      [CAP]
```

`0x56` is opcode `0x56`; the `'V'` in the legacy source is an ASCII accident, not a letter.
UpdateList's display-ON differs — length `0x04`, state `0x02`.

### 5.2 `0x77` windowed text / `0x74` popup overlay

```
[0]  10                first frame
[1]  0E                declared length
[2]  77 windowed text  |  74 full-window popup overlay
[3]  icon              55 = none, 45 = AF-RDS   (popup capture used 09)
[4]  55                second icon bank, fixed, meaning unknown
[5]  srcIcon           FF none, DF "MANU", FD "PRESET", other = "LIST"
[6]  fmt               see §6.2
[7]  01                control byte, always 01
[8..] text
```

- `setText` (`0x77`): declares `0x0E` = 14 but transmits **20** content bytes. The panel
  consumes the header plus the **first 8** text bytes. **Do not "fix" the length.** `[CAP]`
- `showPopupText` (`0x74`): declares `6 + tlen`, correct. `tlen` clamped to 8..16,
  space-padded. Captured: `10 0E 74 09 55 FF 60 01` + 8 bytes. `[CAP]`

### 5.3 `0x21` screen family

Mode is payload `[3]`: **`0x01` windowed menu, `0x05` fullscreen.**

**`0x21` mode `0x01` — menu, 96 payload bytes**

```
[0]  10
[1]  5A     declared 90 — SHORT BY 4, deliberate, panel DONEs at 13 frames
[2]  21
[3]  01
[4]  7E
[5]  80
[6]  00
[7]  00
[8]  82
[9]  FF
[10] scroll     00 none · 07 up · 0B down · 0C both
[11..36] header    26 chars, NUL-padded
[37] 00
[38] 7E          row-0 tag
[39..63] item0    25 chars, NUL-padded
[64] 01
[65] 7F          row-1 tag
[66..95] item1    30 in buffer, only [66..91] reach the panel
```

**`0x21` mode `0x05` — fullscreen, 98 payload bytes**

```
[0]  10
[1]  60     declared 96 — CORRECT
[2]  21
[3]  05
[4]  FF
[5]  00
[6]  00
[7]  40
[8..33]  00 × 26
[34..97] text block, pre-filled 0x20; writing starts at [36]
         (so two leading spaces), lines separated by 0x0D
```

Corroborated frame-for-frame against the OEM "Please insert navigation CD" screen. `[OEM]`

**Confirm box** shares `0x21`/`0x05` with header `21 05 00 00 01 49`, declares `0x6F`
(correct), and is 113 payload bytes — exactly the transport ceiling. Caption region
(content `0x1A..0x20`) **overlaps** the row region at content `0x20`, so a 7-character
caption corrupts row 1. Keep captions ≤ 6. `[DERIVED]` — no capture exists.

### 5.4 `0x76` info/settings row — one message per row

```
frame A:  10 0B 76 <prefix> <slot> t0 t1 t2
frame B:  21 t3 t4 t5 t6 t7 <pad> <pad>
```

`0x0B` = 11 = 3 header + 8 text, correct. OEM values: prefix `0x60`, slots `0x41`,
`0x44`, `0x48`. Captured rows: `76 60 41 "AUX  ON"`, `76 60 44 "AF   ON"`,
`76 60 48 "SPEED 0"`. `[OEM]`

### 5.5 Screen lifetimes — which ones need closing

| screen | behaviour |
|---|---|
| `0x21` menu, `0x21` fullscreen, `0x77` text | **REPLACE.** No teardown. A later screen of any of these simply supersedes the earlier one, so a fullscreen can be re-rendered continuously (~190 ms per screen) with nothing in between. `[CAP]` |
| `0x74` popup | **TRUE OVERLAY.** Survives redraws of the screen beneath — the base visibly updates underneath it — and is cleared **only** by `02 54 03`. `[CAP]` |

> Older notes had these two the wrong way round. The corrected reading was established on
> a real panel on 2026-07-28. If you are holding a document that says a fullscreen owns the
> glass until closed, it is the old one.

---

## 6. Text

### 6.1 Character set

**7-bit printable ASCII only.** The panel cannot render UTF-8; non-ASCII must be
transliterated before transmission or it reaches the glass as mojibake.

One known non-ASCII glyph: **`0xB0` is a blinking cursor** on the radio CODE screen,
advancing `B0 → B1 → …` as digits are accepted. `[OEM]`

### 6.2 The format byte

```
bit 6 (0x40) : 0 = radio rendering (digits + decimal point), 1 = plain ASCII
bits 5..0    : ASCII code of the channel glyph, masked to 6 bits
```

So `0x60` = plain rendering of a space = **no channel glyph**, the correct default.
Verified against the capture `77 09 55 FF 31 01 "   1056"` which renders **`105.6`** —
fmt `0x31`, bit 6 clear, low bits `'1'`. `[CAP]`

### 6.3 Padding and separators

| | |
|---|---|
| row separator inside a text block | `0x0D`, **fullscreen and confirm box only** |
| menu rows | **not** separator-based — fixed offsets with index/tag bytes |
| empty fullscreen line | **skipped entirely**, emits no `0x0D`. `("A","","B")` → `A\rB\r` |
| menu / confirm padding | `0x00` |
| fullscreen / popup padding | `0x20`, so unused cells render blank |

> **`0x00` inside a text region terminates the string** — decoders break on it, and
> `showMenu` depends on that to separate header from rows. It cannot be used as intra-text
> padding. Use `0x20`.

### 6.4 Visible widths

| screen | transmitted | actually shown |
|---|---|---|
| menu header | 26 | 26 |
| menu row 0 | 25 | 25 |
| menu row 1 | 30 | **26** |
| `setText` `0x77` | 14 | **8** |
| popup `0x74` | 8..16 | ≥8 |
| info row `0x76` | 8 | 8 |
| fullscreen | 64-cell block | ~62 after the two leading spaces |

---

## 7. Keys

Panel → us on `0x1C1` (Carminat) or `0x0A9` (UpdateList).

```
[0] 03          mandatory
[1] 89          mandatory — both must match or the frame is not a key
[2] key high
[3] key low, may carry the hold mask
[4..7] filler, don't care
```

| code | key |
|---|---|
| `0x0000` | Load (button at the bottom of the stalk) |
| `0x0001` | SrcRight |
| `0x0002` | SrcLeft |
| `0x0003` | VolumeUp |
| `0x0004` | VolumeDown |
| `0x0005` | Pause |
| `0x0101` | RollUp (encoder detent) |
| `0x0141` | RollDown (encoder detent) |

**There is no release event.** One frame per press. "Hold" is `0xC0` ORed into the **low
byte only**.

```
if raw == 0x0101 or raw == 0x0141:   # encoder detents — EXEMPT
    hold = false; key = raw
else:
    hold = (data[3] & 0xC0) != 0
    key  = raw & 0xFF3F
```

> **The encoder exemption is load-bearing.** `0x0141 & 0xC0 = 0x40`, so without it every
> wheel-**down** detent decodes as a *held* wheel-**up**. `[REF][IMPL][EMU]`

> **`0x1C1` is not a key-only channel.** The panel also sends `70 A3 …` and
> `05 63 30 30 33 37 A3 A3` on it. A decoder that skips the `03 89` guard manufactures
> phantom keys. `[OEM]`

> Wheel-down-with-hold is **indistinguishable** from wheel-up-with-hold: both are
> `0x01C1`. Never observed; unresolvable on the wire if it occurs. `[DERIVED]`

---

## 8. Timing

| | value |
|---|---|
| Carminat B9/69 liveness cadence | about 500 ms |
| peer-alive timeout | 5 ticks ≈ 5 s |
| per-frame ACK timeout | 2000 ms |
| retry policy | **none** — no retransmission at any layer |
| panel sync-request rate when unacknowledged | **line rate** — measured 1472 frames/s on a 500 kbit/s bus, ~92 % occupancy |

> **The last row is the one that bites.** A panel that has not been acknowledged does not
> ask politely once a second — it repeats at line rate. Answering each request with a
> 3-frame hello is ~4400 transmit attempts/s into a bus that carries ~4200, which fills the
> transmit queue permanently and starves everything behind it. **The hello must be paced
> independently of the request rate**; the sync *state* still advances per request.

### 8.1 An unacknowledged panel is in CAN-level retransmit, and it looks like two states

Measured 2026-07-29, listen-only, over hours. A panel with no master alternates two frames in
line-rate bursts:

```
3CF  61 11 01 A3 A3 A3 A3 A3   ×635 in 448 ms
3CF  69 00 A3 A3 A3 A3 A3 A3   ×126 in  32 ms
```

**A "1 Hz peer-alive ping" arriving 126 times in 32 ms is not a ping.** Both frames are being
retransmitted by the CAN controller because nothing is giving them an **ACK bit** — the
link-layer acknowledgement, not the application `0x74`. The panel goes error-passive, so its
error flags are recessive and invisible on the wire, and it will sit like that indefinitely.

Consequences that matter for anyone reading a capture of this bus:

- **Frame counts are meaningless as protocol events.** 635 copies of `61 11 01` is *one*
  request, retransmitted. Coalesce identical consecutive frames before interpreting anything.
- **A listen-only capture of an unacknowledged bus looks perfectly healthy** — zero stuff,
  form or CRC errors — because passive error flags are recessive. Clean does not mean synced.
- **There is no polite window after a power cycle.** Measured at 1500 frames/s within 8 s of
  the panel being powered up. Any design that assumes it must catch the panel early is wrong.
- **Bus occupancy is ~35 % on average, not 92 %.** The frames are back-to-back *within* a
  burst (4+ frames/ms), but the bursts sit in a ~500 ms macro-cycle — measured 2026-07-29
  as ~126 × `69 00` plus ~500–630 × `61 11 01` per cycle, with tens of milliseconds of idle
  between bursts. The 92 % figure came from reading one ring sample as continuous. There is
  always room to transmit; whether a transmission survives is a different question (§8.3).

### 8.3 Reading the controller's own verdict: ECC and the pad, before any theory

Two measurements settle in minutes what days of counter-reading could not, and both are in
`examples/04_rows` (`/api/ecc`, `/api/pad`):

- **The TWAI error-code-capture register** latches the type, direction and exact bit-field
  of the first bus error since it was last read. The driver's ISR consumes it on every
  bus-error interrupt (the read is what re-arms the interrupt), so a poller sees only
  zeros until the bus-error interrupt enable bit is masked; with it masked, a 1 ms poll
  reads the first error of every millisecond. On the deadlocked bench this returned
  `BIT RX @ ACK-SLOT` on essentially every frame — the receiver drove its ACK dominant and
  sampled it back recessive — plus `BIT TX @ SOF` for our own queued frames: every dominant
  we drove, in any field, never appeared at our own sample point.
- **The pad correlation** samples both CAN pins in a single GPIO input-register read at
  ~170 ns resolution. It proved the TX pad really pulses dominant (one-bit-wide pulses at
  the per-frame ACK rate, GPIO matrix and output-enable verified correct) while the RX pad
  — demonstrably alive with panel traffic in the same window — showed recessive in every
  one of those instants.

Together they exonerate every firmware-controllable layer at once: mode bits, bit timing,
matrix routing, output enable, the waveform itself, and the controller's own accounting all
agree. When these two instruments disagree with a theory, the theory is wrong; run them
before proposing one.

### 8.2 Registration is bidirectional and the panel keeps its own state

See §3.5. The panel registers itself on its key channel and expects the master to acknowledge
it; the master registers its own function ids and expects the panel to acknowledge those.
Either side can decide the other's registration is void — `61 11 01` is the panel doing
exactly that — and neither side re-checks it spontaneously.

---

## 9. Traps

Ranked by how much time each has cost.

1. **Offset origin.** Reassembly keeps the 2-byte header, so decode offsets are content+2.
   Three origins circulate in the source material.
2. **`30 01 00` is a per-frame ACK, not flow control**, and `0x74` means *stop*, not
   *complete*.
3. **Declared lengths are wrong in two commands and must stay wrong** (`setText` `0x0E`,
   `showMenu` `0x5A`).
4. **A standard ISO-TP stack cannot be used.** It inserts its own PCI on frame 0 and
   shifts every payload byte by two.
5. **Registration is lazy and all-or-nothing**, and a function id the panel does not ACK
   stalls every render for ever.
6. **`0x00` terminates text.** Pad with `0x20` wherever cells must render blank.
7. **Auto-ACK has no sender test.** "ACK anything not on the sync-reply id and without
   `0x400` set" ACKs your own frames on a self-receiving link, completing transfers after
   one frame with bogus success.
8. **The reply-flag test is `id & 0x400`**, so unrelated high ids are swallowed silently.
9. **`_sync_status = FAILED` is an assignment**, not an OR — it drops `FUNCSREG` too.
10. **Never validate a received filler byte** (§1.1).
11. **The continuation counter does not wrap**, capping us at 113 bytes while the OEM
    wraps and sends 302.

---

## 10. Open questions

- The `0x55` byte at text payload `[4]` remains an unexplained constant. The current
  captured Carminat power form is zero-padded `03 52 09 00 00 00 00 00`; the older
  MeganeCAN `FF FF` spelling is historical compatibility evidence, not the default wire form.
- Icon byte values do not decode cleanly against the `[REF]` bitmap: `0x94` shows **no**
  icon and `0x9B` shows traffic, neither of which the bitmap predicts. Codes appear to
  repeat cyclically across `0x00..0xFF`. Not fully decoded.
- What makes the panel choose `data[2] == 0x01` is not known. The strict library policy
  treats it as discovery only and waits for `00`; the captured legacy radio later proceeds
  differently, so that behavior remains compatibility evidence rather than authorization.
- The confirm box has **no capture at all**. Every byte of §5.3's confirm layout is
  `[DERIVED]`.
- `0x1F1` NAV: we register it and never write it. Its 302-byte OEM payload is a field
  structure, not a bitmap, and is undecoded.
- The cluster profile was transcribed from a single OEM capture and has never been put on
  a bus.
