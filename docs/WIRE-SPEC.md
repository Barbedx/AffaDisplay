# AFFA2 / AFFA3 wire specification

Byte-level oracle for the AffaDisplay library. Everything here was derived by reading the
frame builders in **`MeganeCAN/src/display/**`** — the settled source project — and
hand-executing them, then cross-checked against five independent witnesses (§0), the
strongest of which is ~4 MB of real bus capture in `MeganeCAN/logs/`.

This document is normative. The library's regression tests assert against the
[Golden vectors](#golden-vectors) at the bottom; if an implementation and this file
disagree, the wire wins and this file is corrected — never silently the other way round.

Bus parameters: **500 kbit/s, standard (11-bit) identifiers.**

**Everything *we* transmit is DLC 8**, in both families, without exception: `CanUtils::sendCan`
takes eight bytes and `affa3_do_send` pads to eight with the family filler. **Frames we
*receive* are not.** The OEM dashboard capture (§1.1) contains `3CF` with DLC 1, `3AF` and
`3CF` with DLC 2, `2E8` with DLC 3. Any RX decoder that indexes `data[1]` or `data[2]`
without first checking `len` is reading uninitialised memory — and legacy does exactly
that (§5.3, §5.6). Fix it in the library; do not reproduce it.

---

## 0. The five witnesses

Every claim below is tagged with which of these corroborate it. A claim carrying only
**[CODE]** is DERIVED-ONLY: nothing but the source says it is true.

| Tag | Witness |
|---|---|
| **[LIVE]** | Live capture, ESP32-C3 + genuine Carminat panel, 2-node bus (§1) |
| **[CAP]** | **The firmware capture corpus, `MeganeCAN/logs/*.log` (~4 MB, 21 files).** Serial dumps carrying both an `@TX <id> <bytes>` transmit mirror and an `[RX] ID: 0x… Data: { … }` receive dump, recorded against a genuine Carminat panel — and in one session against a genuine OEM Renault head unit as well. This is the strongest witness in the repository and it was not previously consulted; §1.2 (§1.2) |
| **[OEM]** | OEM dashboard bus log, `MeganeCAN/notes/notes1` — a real Renault head unit talking to a real cluster, neither of them ours (§1.1) |
| **[REF]** | Third-party AFFA3 reference implementation, `MeganeCAN/notes/archive_mhroczny/affa3.{c,h}` |
| **[TWIN]** | The panel twins + screen decoder, `MeganeCAN/src/vdisplay/**` and `MeganeCAN/src/affa/ScreenDecode.cpp` — an independent *decode-side* statement of the same field offsets |
| **[CODE]** | The transmit builders themselves, `MeganeCAN/src/display/**` |

Note what **[TWIN]** is worth and what it is not: the twins were written from the same
understanding as the encoders, so agreement between them confirms *internal consistency*,
not correctness against the panel.

> **[CAP] outranks [CODE] wherever they disagree, and in one place they do.** §3.6 shows
> that the frame count this document previously derived for `showMenu` was wrong: the real
> panel terminates the transfer one frame earlier than the builder is willing to send. That
> correction came out of the capture corpus and out of nothing else.

---

## 1. Ground truth: live capture

Taken from an ESP32-C3 SuperMini + SN65HVD230 + genuine Renault Carminat panel, two nodes
only, 500 kbit/s, while MegaOpen was running the legacy code. This is the reference
against which every byte below is justified.

```
RX  3CF  61 11 00 A3 A3 A3 A3 A3     panel: sync request  (panel filler is 0xA3)
RX  3CF  69 00 A3 A3 A3 A3 A3 A3     panel: peer-alive ping, ~1 Hz
RX  1C1  70 A3 A3 A3 A3 A3 A3 A3     panel: function-registration request
TX  5C1  74 00 00 00 00 00 00 00     us: DONE ack (0x1C1 | 0x400)
TX  3AF  70 1A 11 00 00 00 00 01     us: hello/announce, in reply to 61 11
TX  3AF  B0 14 11 00 1F 00 00 00     us: sent TWICE, in reply to 61 11
TX  3AF  B9 00 00 00 00 00 00 00     us: alive heartbeat, 1 Hz
TX  3AF  BA 00 00 00 00 00 00 00     us: sync request (only while FAILED|START)
```

Two facts fall straight out of this capture and are easy to get wrong:

* **The panel's filler byte is 0xA3, ours is not.** `0xA3` never appears in anything we
  transmit. Carminat TX filler is `0x00`; UpdateList TX filler is `0x81`. A decoder must
  not assume symmetric padding.
* **`61 11` arrives with `data[2] = 0x00` here.** `data[2] == 0x01` is the "cold start"
  variant that additionally latches `START` and therefore provokes a `0xBA` sync request
  on the next heartbeat. Both are seen in the field.

## 1.1 Second witness: an OEM bus, neither node ours

`MeganeCAN/notes/notes1` carries a short log of a **genuine Renault head unit talking to a
genuine cluster** — no ESP32 on the bus at all. It is the only evidence in the repository
of what the protocol looks like when we are not one of the participants, and it is worth
more than any amount of reading our own code. Format is `time id dlc bytes… comment`.

```
04,339 3AF 2 5A 01                       OEM radio, DLC 2
04,409 3CF 2 61 23                       cluster: sync request, data[1] = 0x23  (NOT 0x11)
04,412 3AF 8 50 29 00 23 00 00 00 69     OEM radio: init request
04,419 1C1 8 70 84 84 84 84 84 84 84     cluster registers its key function (filler 0x84)
04,419 5C1 8 74 FF FF FF FF FF FF FF     OEM radio ACKs           (filler 0xFF)
04,473 121 8 70 FF FF FF FF FF FF FF     OEM radio registers text function
04,473 1B1 8 70 FF FF FF FF FF FF FF     OEM radio registers display-ctrl function
04,475 521 8 74 84 84 84 84 84 84 84     cluster ACKs 0x121
04,475 5B1 8 74 84 84 84 84 84 84 84     cluster ACKs 0x1B1
04,818 3AF 2 59 00                       OEM radio alive, DLC 2
04,836 3CF 1 69                          cluster peer-alive, DLC 1
04,873 1B1 8 03 52 00 00 FF FF FF FF     OEM radio: display OFF
04,877 5B1 8 74 84 84 84 84 84 84 84     cluster ACKs
04,893 1C1 8 02 64 0F 84 84 84 84 84     cluster -> radio, single frame, 2 content bytes
04,893 5C1 8 74 FF FF FF FF FF FF FF     OEM radio ACKs
```

Five things fall out of this that nothing else in the repository tells you:

1. **The lazy-registration handshake (§4) is exactly right, and it is symmetric.** Both
   directions register: the radio sends `70` to `0x121` and `0x1B1`, the cluster sends `70`
   to `0x1C1`, and each is answered with `74` on `id | 0x400`. Our implementation is the
   radio half of a real, observed exchange. **[OEM]**
2. **The filler byte is per-node and carries no meaning.** In this one log the radio pads
   with `0xFF` and the cluster with `0x84`; in `notes1`'s other fragment the cluster uses
   `0xA2`; on our bench panel it is `0xA3` (§1); we transmit `0x00` (Carminat) or `0x81`
   (UpdateList). **Never match on, validate, or assert a received filler byte.** The only
   bytes that mean anything on the ACK channel are `data[0]` and, for PARTIAL, `data[0..2]`.
3. **`0x3CF` can carry `61 23`, not just `61 11`.** Our `recv()` matches
   `data[0]==0x61 && data[1]==0x11` and silently ignores everything else, so against this
   cluster we would never answer the sync request at all. Whether `0x23` is a different
   protocol revision or a field we are reading as a constant is unknown — see Appendix C.
   The bench panel does send `61 11`, so this is not a blocker; it is a known hole. **[OEM]**
4. **Short DLC is real.** `3CF 1 69` has one byte. Legacy's `recv()` shim copies `fr.len`
   bytes into a stack `CAN_FRAME` and then reads `data[1]` and `data[2]` unconditionally,
   so on this frame it evaluates `data[2] == 0x01` against uninitialised stack. It happens
   to be harmless for `69` (that branch reads only `data[0]`), but on a 2-byte `61 23` it
   would latch `START` at random. The library must reject any sync frame with `len < 3`
   before testing `data[2]`.
5. **`03 52 00 00`, sent by the OEM radio on `0x1B1`, is display-ctrl OFF.** It confirms
   the `0x52` command byte and the single-frame PCI reading of byte 0 — and it uses
   SF_DL `0x03`, matching *Carminat's* `setState` (§8.3) rather than the archive's and
   UpdateList's `0x04` (§9.3). See §9.3.

## 1.2 Third witness: the firmware capture corpus (`MeganeCAN/logs/`)

Twenty-one serial logs, ~4 MB, recorded between April 2025 and June 2026. Two line formats
matter:

* `@TX 151 10 5A 21 01 7E 80 00 00` — the firmware's **transmit mirror**, printed from
  inside the send path. Every byte that went on the wire, in order.
* `[RX] ID: 0x551 Len: 8 Data: { 30 01 00 A3 A3 A3 A3 A3 }` — the **receive** dump.
* `CAN MSG: 0x3AF [8] <B9:0:0:0:0:0:0:0>` — an older (April 2025) receive format.

Because both directions are timestamped into one stream, a whole ISO-TP transfer with its
interleaved ACKs can be read off directly. That is what makes §3.6 provable.

`device-monitor-260616-230730.log` is the most valuable single file in the repository: the
ESP32 was in `_skipFuncReg` (passive) mode on a bus that also carried a **genuine OEM
Renault head unit**, so the log contains the OEM radio's own screen traffic on `0x151`,
decoded by nobody but observed by us:

```
[RX] ID: 0x151 Len: 8 Data: { 10 60 21 05 FF 00 00 40 }     OEM radio: fullscreen screen (§8.6)
[RX] ID: 0x551 Len: 8 Data: { 30 01 00 A3 A3 A3 A3 A3 }     panel: PARTIAL, filler 0xA3
[RX] ID: 0x151 Len: 8 Data: { 25 50 6C 65 61 73 65 20 }     "Please "
[RX] ID: 0x151 Len: 8 Data: { 26 69 6E 73 65 72 74 20 }     "insert "
[RX] ID: 0x151 Len: 8 Data: { 27 20 20 20 0D 20 20 6E }     "   \r  n"
[RX] ID: 0x151 Len: 8 Data: { 28 61 76 69 67 61 74 69 }     "avigati"
[RX] ID: 0x151 Len: 8 Data: { 2D 20 0D 00 00 00 00 00 }     tail, PCI 0x2D
[RX] ID: 0x151 Len: 8 Data: { 02 54 03 00 00 00 00 00 }     OEM radio: close window (§8.8)
```

This corroborates §8.6 and §8.8 from a transmitter that is not ours and not derived from
our source — the strongest kind of evidence available. It also confirms the panel's ACK
filler is `0xA3` and that the OEM radio uses `10 60 …` with a 14-frame transfer ending at
PCI `0x2D`, exactly as our `showFullscreenText` builder does.

### Frames observed verbatim in the corpus

Every one of these is quoted from a log line, not derived. Counts are across all 21 files.

| Frame | × | Operation |
|---|---|---|
| `@TX 151 10 0E 77 55 55 FF 60 01` | 68 | `setText` header (§8.1) |
| `@TX 151 21 52 45 4E 41 55 4C 54` + `@TX 151 22 00 00 00 00 00 00 00` | 8 | `setText("RENAULT")`, both tail frames |
| `@TX 151 10 0E 74 09 55 FF 60 01` | 17 | `showPopupText` header, icon `0x09` (§8.7) |
| `@TX 151 21 56 4F 4C 20 32 38 20` + `@TX 151 22 20 00 00 00 00 00 00` | 16 | the OEM `"VOL 28"` popup, space-padded to 8 |
| `@TX 151 03 52 09 FF FF 00 00 00` / `03 52 00 FF FF 00 00 00` | 42 / 11 | `setState` enable / disable (§8.3) |
| `@TX 151 07 29 01 7E 80 00 00 00` / `07 29 01 7F 80 00 00 00` | 11 / 12 | `highlightItem(0)` / `(1)` (§8.4) |
| `@TX 151 02 54 03 00 00 00 00 00` | 9 | close window / popup (§8.8) |
| `@TX 151 10 0B 76 60 41 41 55 58` | 7 | info row, offset `0x41`, `"AUX…"` (§8.10) |
| `@TX 151 10 0B 76 60 44 41 55 54` | 7 | info row, offset `0x44`, `"AUT…"` |
| `@TX 151 10 0B 76 60 48 53 50 45` | 7 | info row, offset `0x48`, `"SPE…"` |
| `@TX 1C1 03 89 01 01 00 00 00 00` | 11 | emulated RollUp key, bytes 4..7 padded `0x00` (§7) |
| `[RX] 0x1F1 { 70 00 00 00 00 00 00 00 }` | 3 | our `0x1F1` registration frame (§4) |
| `[RX] 0x551 { 30 01 00 A3 A3 A3 A3 A3 }` | 33 k | panel PARTIAL ACK |
| `[RX] 0x551 { 74 A3 A3 A3 A3 A3 A3 A3 }` | 2.7 k | panel DONE ACK |
| `[RX] 0x5F1 { 30 01 00 A3 A3 A3 A3 A3 }` | 60 | panel PARTIAL on the NAV function |
| `[RX] 0x1C1 { 70 A3 A3 A3 A3 A3 A3 A3 }` | 3 | panel registration request (matches §1) |
| `[RX] 0x1C1 { 02 64 0F A3 A3 A3 A3 A3 }` | 17 | panel → radio, **not a key** (§7.1) |
| `[RX] 0x1C1 { 05 63 30 30 33 37 A3 A3 }` | 2 | panel → radio, `63 "0037"` — **not a key** (§7.1) |
| `CAN MSG: 0x3CF [8] <61:11:A3:A3:A3:A3:A3:A3>` | 791 | panel sync request (§5.3) |
| `CAN MSG: 0x3CF [8] <69:A3:A3:A3:A3:A3:A3:A3>` | 1157 | panel peer-alive (§5.4) |

Three of these contradict, refine or close something stated elsewhere:

1. **The panel's sync frames pad from `data[1]`, not `data[2]`.** The corpus has
   `69:A3:A3:…` and `61:11:A3:A3:…`, where §1's live capture reads `69 00 A3…` and
   `61 11 00 A3…`. `data[1]` of the ping and `data[2]` of the request are **don't-care**;
   never match on them. The one byte that is read, `data[2]` of `61 11`, has **never been
   observed as `0x01`** in any capture — see §5.3.
2. **`0x1C1` carries panel→radio traffic that is not a key** (`02 64 0F`, `05 63 "0037"`).
   The key decoder's `03 89` guard is load-bearing, not defensive coding (§7.1).
3. **`0x1F1` is registered but the OEM radio does write to it**, with a 12-bit ISO-TP
   length and a *wrapping* sequence counter — see §3.7 and §3.3.

---

## 2. Identifier map

| Role | Carminat (AFFA3) | UpdateList (AFFA2) | Source |
|---|---|---|---|
| Sync TX (ours) | `0x3AF` | `0x3DF` | `PACKET_ID_SYNC` |
| Sync RX (panel) | `0x3CF` | `0x3CF` | `PACKET_ID_SYNC_REPLY` |
| Reply flag (OR-mask) | `0x400` | `0x400` | `PACKET_REPLY_FLAG` |
| Text function | `0x151` | `0x121` | `PACKET_ID_SETTEXT` |
| Display-control function | `0x151` | `0x1B1` | `PACKET_ID_DISPLAY_CTRL` |
| Second registered function | `0x1F1` (NAV) | — | `PACKET_ID_NAV` |
| Key input (panel -> us) | `0x1C1` | `0x0A9` | `PACKET_ID_KEYPRESSED` |
| TX filler byte | `0x00` | `0x81` | `PACKET_FILLER` |

**Registered function table** (`initializeFuncs()`), order matters — it is the order the
lazy `0x70` registration walks:

* Carminat: `{ 0x151, 0x1F1 }`
* UpdateList: `{ 0x121, 0x1B1 }`

ACK identifiers are `funcId | 0x400`:

| Function | ACK id |
|---|---|
| `0x151` | `0x551` |
| `0x1F1` | `0x5F1` |
| `0x121` | `0x521` |
| `0x1B1` | `0x5B1` |
| `0x1C1` (panel -> us; we ACK it) | `0x5C1` |
| `0x0A9` (panel -> us; we ACK it) | `0x4A9` |

Compute the ACK id, never hard-code it: `0x0A9 | 0x400 = 0x4A9`, **not** `0x5A9` — bit 8 is
already clear in `0x0A9`, unlike every other id in this table.

---

## 3. The ISO-TP-ish transport (`affa3_do_send`)

Source: `MeganeCAN/src/display/AffaDisplayBase.cpp:12-112` (`affa3_do_send`).

The transport is a stripped ISO 15765-2: a first frame carrying **8** payload bytes with no
PCI added by the transport, then consecutive frames carrying **7** payload bytes each,
prefixed with `0x20 + n` where `n` counts from 1. The tail frame is padded to DLC 8 with the
family filler.

> The `0x10 <len>` you see at the head of most payloads is **not** added by the transport.
> Every caller builds it into its own buffer by hand. That is why `setTime` (`0x05 …`) and
> `setState` (`0x03 …` / `0x04 …`) look different: they are single-frame messages whose
> first byte is an ISO-TP *single frame* PCI, and the transport never touches it.

### 3.0 Byte 0 of every payload is an ISO-TP PCI — read the whole repertoire that way

This is the single idea that makes the rest of the document fall into place, and it is
missing from every comment in the source. Every payload the two families build starts with
a genuine ISO 15765-2 PCI nibble:

| Byte 0 | ISO-TP meaning | Content bytes it declares | Operations |
|---|---|---|---|
| `0x0N` | single frame, SF_DL = N | N | `setState` (`03`/`04`), `setTime` (`05`), `highlightItem` (`07`), `hidePopup` (`02`), inbound key (`03`) |
| `0x10 LL` | first frame, total length = `0x0LL` | LL | every screen builder |

Check it against the repertoire and it holds exactly:

| Payload | Byte 0 | Declared | Content actually built | Verdict |
|---|---|---|---|---|
"Content" below means *bytes present in the builder's payload buffer after byte 0 (and,
for a first frame, after byte 1)* — i.e. what the builder is prepared to transmit, before
the transport pads with filler.

| Payload | Byte 0 | Declared | Content built | Verdict |
|---|---|---|---|---|
| `02 54 03` (raw frame) | SF | 2 | 2 (`54 03`; rest is pad) | exact |
| `03 52 09 FF FF` | SF | 3 | 4 (`52 09 FF FF`) | 1 surplus |
| `04 52 02 FF FF` | SF | 4 | 4 (`52 02 FF FF`) | exact |
| `05 56 <hhmm> 00 00` | SF | 5 | 7 (`56 h h m m 00 00`) | 2 surplus |
| `07 29 01 7E 80 00 00 00` | SF | 7 | 7 | exact |
| `03 89 <hi> <lo>` (inbound key) | SF | 3 | 3 (`89 hi lo`; rest is pad) | exact |
| `10 0B 76 …` info row | FF | 11 | 13 | 2 surplus |
| `10 0E 77 …` setText | FF | 14 | 20 | **6 surplus** (§8.1) |
| `10 19 76 …` UL segment | FF | 25 | 27 | 2 surplus (the two trailing `0x81`) |
| `10 1C 7F …` UL LCD | FF | 28 | 28 | exact |
| `10 5A 21 01 …` showMenu | FF | 90 | 94 | **4 surplus — and they never reach the wire, §3.6** |
| `10 60 21 05 …` fullscreen | FF | 96 | 96 | exact |
| `10 6F 21 05 …` confirm box | FF | 111 | 111 | exact |
| `11 2E 21 0B …` OEM NAV on `0x1F1` | FF | **302** | (not ours) | 12-bit length, §3.7 |

**[REF] corroborates this reading decisively.** The archive `affa3_do_set_text`
(`notes/archive_mhroczny/affa3.c:261-296`) branches between `0x19` and `0x1C` and its own
comments call them commands — *"0x19 = tekst, 0x1C = tekst + ikony"*. They are not commands.
They are the two ISO-TP lengths that the two branches produce: the icons branch prepends
`7F <icons> 55 <mode>` where the plain branch prepends only `76`, i.e. **exactly three more
bytes**, and `0x1C - 0x19 = 3`. `UpdateListMenuDisplay.cpp:31-38` copies the misreading
forward in its comment block.

> **Trap.** Do not model byte 1 of a first frame as a "sub-command" or "screen variant".
> It is a length. If you add a byte to a payload and forget to bump it, the panel silently
> truncates the message and everything still ACKs.

### 3.1 Chunking algorithm

```
left = len; num = 0
while left > 0:
    i = 0
    if num > 0: frame[i++] = 0x20 + num
    while i < 8 and left > 0: frame[i++] = *data++; left--
    while i < 8:              frame[i++] = FILLER
    transmit(frame)                 // id = funcs[idx].id, DLC 8
    wait for ACK on (funcs[idx].id | 0x400), deadline 2000 ms
      DONE (0x74)          -> stop, success
      PARTIAL (30 01 00)   -> if left == 0: FAIL(SendFailed); else num++, continue
      anything else        -> FAIL(SendFailed)
      no ACK by deadline   -> FAIL(Timeout)
```

### 3.2 Frame-count arithmetic

For a pre-ISO-TP payload of `L` bytes:

```
frames(L) = 1                        , L <= 8
frames(L) = 1 + ceil((L - 8) / 7)    , L >  8

last PCI counter  = 0x20 + (frames(L) - 1)
payload bytes in tail frame = L - 8 - 7*(frames(L) - 2)      , frames(L) >= 2
filler bytes in  tail frame = 8 - 1 - (payload bytes in tail) , frames(L) >= 2
filler bytes in  tail frame = 8 - L                           , frames(L) == 1
```

These are the frames the builder **would** emit if it were never stopped. §3.6 shows the
panel stops it early in one case, so read this table together with that one.

| Operation | `L` | frames built | last PCI | tail payload | tail filler |
|---|---|---|---|---|---|
| registration `0x70` | 1 | 1 | — | 1 | 7 |
| Carminat `setState` | 5 | 1 | — | 5 | 3 |
| Carminat `setTime` | 8 | 1 | — | 8 | 0 |
| UpdateList `setState` | 5 | 1 | — | 5 | 3 |
| Carminat `setText` | 22 | 3 | `0x22` | 7 | 0 |
| Carminat `showPopupText` (tlen 8) | 16 | 3 | `0x22` | 1 | 6 |
| Carminat `showPopupText` (tlen 16) | 24 | 4 | `0x23` | 2 | 5 |
| Carminat `showMenu` | 96 | 14 | `0x2D` | 4 | 3 | 
| Carminat `showFullscreenText` | 98 | 14 | `0x2D` | 6 | 1 |
| Carminat `showConfirmBox` | 113 | 16 | `0x2F` | 7 | 0 |
| UpdateList `setText` (segment) | 29 | 4 | `0x23` | 7 | 0 |
| UpdateList `setText` (LCD) | 30 | 5 | `0x24` | 1 | 6 |

### 3.3 Hard payload ceiling: 113 bytes

The counter is literally `0x20 + num` — the legacy code does **not** wrap it into the low
nibble the way real ISO-TP does. `num = 16` produces `0x30`, which is the ISO-TP
flow-control PCI and is not a consecutive frame at all.

```
num_max = 15  ->  L_max = 8 + 15*7 = 113
```

`showConfirmBoxWithOffsets` sits at exactly 113 bytes — one byte of headroom does not
exist. **Reject payloads longer than 113 with `Result::TooLong` at enqueue time.** The
`AFFA_MAX_PAYLOAD` default of 120 quoted in the design brief is above the wire-safe limit
and must be clamped to 113.

> **What the counter should do past `0x2F`, on evidence.** The OEM head unit's `0x1F1`
> message is 302 bytes long and therefore needs 43 consecutive frames. Collecting every
> distinct PCI byte seen on `0x1F1` across the corpus gives exactly:
> `11 20 21 22 23 24 25 26 27 28 29 2A 2C 2D 2E 2F 70` — `0x20` is present, nothing above
> `0x2F` is. **The OEM wraps the sequence number modulo 16, i.e. real ISO 15765-2**
> (`0x21…0x2F`, then `0x20`, then `0x21` again). **[CAP]**
>
> So the correct extension is `0x20 | (num & 0x0F)`, which is byte-identical to legacy for
> `num <= 15` and correct beyond it. Implement it that way — it costs nothing — but keep
> the 113-byte clamp as the default ceiling until a >113-byte transmit has actually been
> validated against the panel. Nothing in the current repertoire needs it.

### 3.4 ACK semantics

The panel answers each transmitted frame on `funcId | 0x400`:

| `data[0..2]` | Meaning | Legacy `FuncStatus` |
|---|---|---|
| `74 xx xx` | End of data — message accepted | `DONE` |
| `30 01 00` | Partial accepted, send the next consecutive frame | `PARTIAL` |
| anything else | Rejected | `ERROR` |

Only `data[0]` is inspected for DONE, and `data[0..2]` for PARTIAL. The remaining bytes are
don't-care and in practice carry the panel's `0xA3` filler.

The ACK is only consumed when the matching function slot is in state `WAIT`
(`CarminatDisplay.cpp:407`, `UpdateListBase.cpp:113`). An ACK arriving for an idle
function is dropped silently — keep that, it is what stops a stale late ACK from
completing the wrong ticket.

**Expected ACK sequence for an N-frame message: `30 01 00` × (N-1), then `74`.** Two error
cases follow directly from the algorithm and must be preserved:

* `74` arriving before the last frame ends the send early and reports **success** with a
  truncated message. (Legacy `break`.) **This is not an edge case — it is the normal path
  for `showMenu`, on every render. See §3.6 before you "fix" it.**
* `30 01 00` arriving after the last frame reports **SendFailed** (`if (!left) return
  SendFailed`).

### 3.5 Gating

`affa3_do_send` returns `NoSync` and puts **nothing** on the wire when
`!_skipFuncReg && (sync_status & FAILED)`. In `_skipFuncReg` ("radio owns sync") mode the
FAILED gate is bypassed entirely and registration is skipped.

### 3.6 The panel terminates the transfer at FF_DL — and one builder over-runs it

**This is the correction that the capture corpus forced, and the single most expensive
off-by-one in this document.**

The transmit loop is ACK-driven: it `break`s the moment a `DONE` arrives (§3.1). The panel
issues `DONE` as soon as it holds the number of content bytes the first frame *declared*.
A first frame carries 6 content bytes (`data[2..7]`) and each consecutive frame carries 7,
so the panel is satisfied after

```
n_cf = ceil((FF_DL - 6) / 7)          consecutive frames
frames on the wire = 1 + n_cf
```

frames — **regardless of how many the builder was prepared to send.** Any frame the builder
would have sent beyond that is never transmitted at all.

| Operation | FF_DL | `n_cf` | frames on wire | frames built | verdict |
|---|---|---|---|---|---|
| `setText` | 14 | 2 | 3 | 3 | agree |
| `showPopupText` (tlen 8) | 14 | 2 | 3 | 3 | agree |
| `showPopupText` (tlen 16) | 22 | 3 | 4 | 4 | agree |
| info row (raw, unacked) | 11 | 1 | 2 | 2 | agree |
| **`showMenu`** | **90** | **12** | **13** | **14** | **one frame is dropped** |
| `showFullscreenText` | 96 | 13 | 14 | 14 | agree |
| `showConfirmBox` | 111 | 15 | 16 | 16 | agree |
| UL `setText` (segment) | 25 | 3 | 4 | 4 | agree |
| UL `setText` (LCD) | 28 | 4 | 5 | 5 | agree |

`showMenu` is the only over-run, and it is caused by the `0x5A` (=90) length byte being
4 short of the 94 content bytes the builder holds (§3.0). `6 + 12*7 = 90` exactly, so the
panel `DONE`s after PCI `0x2C` and PCI `0x2D` is never sent.

**Proof, from a session with the real panel** (`logs/device-monitor-260616-230730.log`):

```
[CAN] do_send totalLen: 96
@TX 151 10 5A 21 01 7E 80 00 00
[RX] ID: 0x551 Len: 8 Data: { 30 01 00 A3 A3 A3 A3 A3 }
@TX 151 21 82 FF 00 4D 65 67 61        …twelve consecutive frames…
[RX] ID: 0x551 Len: 8 Data: { 30 01 00 A3 A3 A3 A3 A3 }
@TX 151 2C 00 00 00 00 00 00 00        <- twelfth and last consecutive frame
[RX] ID: 0x551 Len: 8 Data: { 74 A3 A3 A3 A3 A3 A3 A3 }   <- DONE, transfer over
```

Counted across that file: `@TX 151 2C` occurs **2589** times, `@TX 151 2D` **zero** times.
`2D` appears only in the two logs recorded through the bench self-ACK emulator
(`[route] virtual`), where our own code fabricates `PARTIAL` on every frame and the whole
96-byte buffer therefore goes out. **[CAP]**

Consequences the library must encode:

* The `showMenu` golden vector for a **real panel** is **13 frames**, ending at PCI `0x2C`.
  The 14-frame form is the loopback/emulator form. `test_isotp` must assert both, against
  an ACK-model parameter, or it will pin the wrong one.
* `item2` (payload `[66..95]`) is truncated at payload offset 91 on the wire: **26 usable
  characters, not 30**. Longer strings are silently cut. This was previously listed as an
  unconfirmed inference; it is now confirmed.
* The TX state machine must treat "`DONE` while bytes remain" as **success**, not as a
  short write. Legacy does (`break`), and the panel depends on it.
* Do not "fix" the `0x5A` to `0x5E`. That is a wire change to a screen the panel has been
  rendering correctly for months, and it would add a fourteenth frame nothing has tested.

### 3.7 First-frame length is 12 bits, not 8

Legacy only ever builds `0x10 <len>`, so it never exercises the high nibble. The OEM head
unit does:

```
[RX] ID: 0x1F1 Len: 8 Data: { 11 2E 21 0B 00 25 41 42 }
```

`0x11 0x2E` is `FF_DL = ((0x11 & 0x0F) << 8) | 0x2E = 0x12E = 302` bytes — the standard
ISO 15765-2 first frame. **[CAP]**, corroborated by
`MeganeCAN/notes/AFFA3_SCREENS.md` ("ISO-TP first frame `11 2E` -> 302-byte message").

> **Bug carried by the code being ported.** `MeganeCAN/src/affa/IsoTp.cpp:34` matches
> `f.data[0] == 0x10` exactly, so the reassembler silently drops every OEM `0x1F1` message
> and any future long screen. The library's `proto/IsoTp.cpp` must match
> `(data[0] & 0xF0) == 0x10` and decode the 12-bit length. Transmit side may keep emitting
> `0x10 <len>` — nothing we build exceeds 255 content bytes — but it must not *parse* that
> way.
>
> **Status: NOT DONE.** `proto/IsoTp.cpp` still matches `f.data[0] == 0x10` exactly, and
> `docs/API.md`'s `Reassembler` doc-block describes that behaviour — so by the arbiter rule
> the code is not in breach and this remains an open item, tracked as Appendix B #9. It is
> RX-side only: no byte this library transmits depends on it.

---

## 4. Lazy function registration

Source: `AffaDisplayBase.cpp:122-169`.

On the first `affa3_send()` after `FUNCSREG` is clear, and only when `_skipFuncReg` is
false, the sender walks the **whole** function table in declaration order and sends a
1-byte payload `0x70` to each. `FUNCSREG` latches only after **all** of them completed;
any failure aborts and propagates the error to the caller — the user payload is never
transmitted.

Carminat, cold, first `setText`:

```
TX  151  70 00 00 00 00 00 00 00      register text function
RX  551  74 ..                        DONE
TX  1F1  70 00 00 00 00 00 00 00      register NAV function
RX  5F1  74 ..                        DONE
--- FUNCSREG latches here ---
TX  151  10 0E 77 55 55 FF 60 01      the actual setText, frame 0
...
```

UpdateList, cold, first `setText`:

```
TX  121  70 81 81 81 81 81 81 81
RX  521  74 ..
TX  1B1  70 81 81 81 81 81 81 81
RX  5B1  74 ..
--- FUNCSREG latches here ---
TX  121  10 19 76 7A 01 ...
```

Registration is a **single-frame** message, so it expects `74` (DONE) directly. A
`30 01 00` here means `SendFailed` (rule in §3.4).

`FUNCSREG` is cleared again whenever the peer-alive watchdog fires (§5.4), so registration
re-runs on the next send after a link drop. That is deliberate: the panel forgets us too.

---

## 5. Sync handshake

The sync state machine is byte-for-byte identical between the two families apart from the
profile constants below. Legacy duplicates it in `CarminatDisplay::tick()` and
`UpdateListBase::tick()`; the library lifts it into `AffaDisplayBase` parameterised by
`SyncProfile`.

| Profile field | Carminat | UpdateList |
|---|---|---|
| `syncId` | `0x3AF` | `0x3DF` |
| `syncReplyId` | `0x3CF` | `0x3CF` |
| `replyFlag` | `0x400` | `0x400` |
| `aliveByte` | `0xB9` | `0x79` |
| `requestByte` | `0xBA` | `0x7A` |
| `requestArg` (`data[1]` of the request) | `0x00` | `0x01` |
| `filler` | `0x00` | `0x81` |
| `helloCount` | 3 | 1 |

### 5.1 Alive heartbeat — every 1000 ms, unconditional

```
Carminat    TX  3AF  B9 00 00 00 00 00 00 00
UpdateList  TX  3DF  79 00 81 81 81 81 81 81
```

`data[1]` is a literal `0x00` in both families (not the filler — for Carminat they happen
to coincide, for UpdateList they do not: `79 00 81 …`, not `79 81 81 …`).

### 5.2 Sync request — only while `FAILED` or `START` is set

```
Carminat    TX  3AF  BA 00 00 00 00 00 00 00
UpdateList  TX  3DF  7A 01 81 81 81 81 81 81
```

Carminat's `data[1]` is the filler (`0x00`); UpdateList's is a literal `0x01`. After
transmitting, `START` is cleared. Legacy then called `delay(100)` — **deleted outright in
the library**, it has no wire meaning; the request is idempotent and the panel answers
whenever it feels like it.

### 5.3 Reply to the panel's `61 11` (RX on `0x3CF`)

Trigger: `data[0] == 0x61 && data[1] == 0x11`.

Carminat sends three frames on `0x3AF`, in this order, back to back with no inter-frame
delay:

```
TX  3AF  70 1A 11 00 00 00 00 01      hello / announce
TX  3AF  B0 14 11 00 1F 00 00 00      capability announce
TX  3AF  B0 14 11 00 1F 00 00 00      IDENTICAL, sent a second time
```

The duplicate `B0 14 11` is not a bug in the logging — it is two `sendCan` calls in the
source (`CarminatDisplay.cpp:375-376`) and it is present in the capture. Reproduce it.

UpdateList sends one frame on `0x3DF`:

```
TX  3DF  70 1A 11 00 00 00 00 01
```

Then, both families: clear `FAILED`; if `data[2] == 0x01`, set `START`.

> **`data[2]` is the panel's filler, and `START` has never been observed.** The capture
> corpus contains the request 791 times, always as
> `0x3CF [8] <61:11:A3:A3:A3:A3:A3:A3>` — `data[2] = 0xA3`, the panel's pad byte. The live
> capture in §1 transcribes it as `61 11 00 A3 …`. Whichever it is, it is **not** `0x01`,
> in any capture we hold. **[CAP]**
>
> Keep the `data[2] == 0x01 -> START` test — it is [REF]-attested and costs nothing — but
> do not build anything on `START` being reachable, and require `len >= 3` before reading
> `data[2]` (§1.1 note 4). In practice the `0xBA` / `0x7A` sync request is emitted only
> while `FAILED`, which matches the live capture's annotation exactly.

### 5.4 Peer-alive ping (RX on `0x3CF`, `data[0] == 0x69`)

Observed 1157 times in the corpus as `0x3CF [8] <69:A3:A3:A3:A3:A3:A3:A3>` — **`data[1]`
is the panel's filler, not `0x00`** as §1's transcription reads. `0x69` in `data[0]` is
the entire test; nothing else in the frame is inspected and nothing else may be. **[CAP]**

Sets `PEER_ALIVE`. The watchdog consumes the flag on the next heartbeat and re-arms a
**5000 ms wall-clock deadline**. When the deadline expires:

```
sync_status = FAILED           (all other bits, including FUNCSREG, dropped)
```

which re-arms the `0xBA` / `0x7A` sync request and forces re-registration on the next send.

> **This is where one of the two expensive defects lived.** Legacy used
> `static int8_t timeout = SYNC_TIMEOUT` decremented once per `tick()` **call**. That only
> means "five seconds" if the caller ticks at exactly 1 Hz. From a free-running `loop()` it
> expired in milliseconds: the panel's `61 11` cleared FAILED, five loop iterations later
> the watchdog re-armed FAILED and dropped FUNCSREG with it, and the handshake restarted at
> `0xBA` forever — the panel never got a chance to send its ~1 Hz `69` in between.
> UpdateList still carries the unfixed counter version (`UpdateListBase.cpp:23,43-51`).
> The library's version is a millisecond deadline against `IClock::millis()`, in one place,
> for both families.

> **Legacy quirk, decide deliberately:** both `recv()` implementations call `tick()`
> *immediately* on receiving `69`, which emits an extra `B9`/`79` on the spot in addition to
> the free-running one. The library paces the heartbeat from its own 1 Hz timer inside
> `poll()` and does **not** emit an extra heartbeat from the `69` handler; the observed
> cadence on the wire stays ~1 Hz, matching the capture. The `test_sync` requirement
> ("one million `poll()` calls in a simulated second emit exactly one `0xB9`") only holds
> with the timer-only behaviour.

### 5.5 Unknown sync bytes

Anything on `0x3CF` that is neither `61 11` nor `69 ..` is logged and ignored. No reply.

---

## 6. Inbound frames we auto-acknowledge

Any frame that is **not** on the sync-reply id and does **not** have `0x400` set is
answered with a DONE on `id | 0x400`, DLC 8, `0x74` followed by seven filler bytes:

```
Carminat     RX 1C1 70 A3 ...   ->  TX 5C1  74 00 00 00 00 00 00 00
UpdateList   RX 0A9 03 89 ...   ->  TX 4A9  74 81 81 81 81 81 81 81
```

Note the filler asymmetry: Carminat pads with `0x00`, UpdateList with `0x81`.

Exceptions:

* Carminat: suppressed entirely when `_skipFuncReg` is set.
* UpdateList: `0x121` (radio text) returns early and is **never** acknowledged — that frame
  is addressed to the display, not to us.
* UpdateList: a malformed key frame (`data[0] == 0x03 && data[1] != 0x89`) returns early
  and is not acknowledged.
* Carminat acknowledges `0x1C1` unconditionally, including malformed key frames — the ACK
  is emitted *before* the key validity check (`CarminatDisplay.cpp:441-456`).

### 6.1 The auto-ACK rule is a self-echo landmine — and it has already gone off

The rule as written is "**any** frame that is not on the sync-reply id and does not have
`0x400` set". It contains no test for who sent the frame. Feed it a transport that echoes
our own transmissions — `LoopbackLink`, a driver with self-reception enabled, a bus
monitor replaying a log — and the library will acknowledge *itself*: it will answer its own
`0x151` screen frames on `0x551` with `74`, its own ACK matcher will then see that `74`,
and the in-flight ISO-TP transfer will complete after **one frame** with a bogus success.
Silent, total, and it looks like a panel fault.

This is not hypothetical. The April 2025 logs show an early build doing exactly this on the
sync channel:

```
CAN MSG: 0x3AF [8] <BA:0:0:0:0:0:0:0>        our own sync request, received back
final answer sended
ID: 0x7AF DLC: 8 Data: 74 0 0 0 0 0 0 0      we ACKed it: 0x3AF | 0x400 = 0x7AF
```

`0x7AF` is not an ACK channel for anything; nothing on the bus was listening. The frame
existed only because the auto-ACK rule fired on our own transmission. **[CAP]**

Rules for the library, all three mandatory:

1. **Tag every transmitted frame `Frame::fromSelf` and drop it before the auto-ACK, before
   the ACK matcher, and before the key decoder.** The transport's echo behaviour must not
   be able to change a single wire byte or a single delivered event.
2. **Never auto-ACK an id we are registered to transmit on.** `0x151`, `0x1F1`, `0x121`,
   `0x1B1` and both sync ids are ours; an inbound frame on one of them is either an echo or
   another node's traffic, and in neither case do we owe it a `74`.
3. **Never auto-ACK the sync ids at all.** The sync channel has no ACK semantics; `0x7AF`
   and `0x7DF` must never appear on the wire.

---

## 7. Inbound key decode

Identifier: Carminat `0x1C1`, UpdateList `0x0A9`. Layout:

```
byte 0   0x03    fixed; anything else is not a key frame
byte 1   0x89    fixed
byte 2   key code, high byte
byte 3   key code, low byte (may carry the hold mask)
byte 4-7 don't care (panel filler)
```

```
rawKey  = (data[2] << 8) | data[3]
if rawKey == 0x0101 or rawKey == 0x0141:      // encoder detents — EXEMPT
    isHold = false
    key    = rawKey
else:
    isHold = (data[3] & 0xC0) != 0            // KEY_HOLD_MASK = 0x80 | 0x40
    key    = rawKey & 0xFF3F                  // clears bits 6-7 of the LOW byte only
```

| Code | Key |
|---|---|
| `0x0000` | Load (the button at the bottom of the stalk) |
| `0x0001` | SrcRight |
| `0x0002` | SrcLeft |
| `0x0003` | VolumeUp |
| `0x0004` | VolumeDown |
| `0x0005` | Pause |
| `0x0101` | RollUp |
| `0x0141` | RollDown |

**Why the two encoder codes are exempt.** `RollDown` is `0x0141`: bit 6 of its low byte is
set, which is exactly one of the two hold-mask bits. Applying the mask would rewrite it to
`0x0101` — i.e. every wheel-down detent would be reported as a wheel-up. The exemption is
not a special case for elegance, it is the only thing keeping the encoder usable. `0x0101`
is exempted alongside it for symmetry (it has no mask bits set, so masking would be a
no-op).

Consequence to keep in mind: **the encoder can never report a usable hold**. The `Increase` /
`Decrease` coarse-step `NavCommand`s therefore cannot originate from a real panel wheel
detent — they exist for injected input (`injectKey` / `nav`) and for the legacy
`editFieldValue(delta, isHold)` path that multiplies by `Field::stepMultiplier`.

> **The `0x01C1` collision.** The exemption covers only the two bare codes. Set the hold
> mask on either detent and both collapse onto the same value:
> `0x0101 | 0xC0 == 0x01C1` and `0x0141 | 0xC0 == 0x01C1`. That value is not exempt, so it
> is masked to `0x0101` (RollUp) with `isHold = true` — **wheel-down-and-hold is
> indistinguishable from wheel-up-and-hold on the wire.** This is a property of the
> encoding, not a bug to repair: `0x40` is simultaneously the direction bit and half the
> hold mask. Decode `0x01C1` as RollUp + hold to stay byte-compatible with legacy, pin it
> with the vector below, and never build a feature on a held detent. No capture contains
> `0x01C1`; whether the panel emits it at all is unverified (Appendix C).

The masking expression is written `rawKey & ~KEY_HOLD_MASK` in legacy. `KEY_HOLD_MASK` is
`uint8_t 0xC0`; integer promotion makes `~0xC0` equal `0xFFFFFF3F`, so the high byte of
`rawKey` survives. Do not "fix" this into `rawKey & ~(uint16_t)0xC0` thinking it is the
same — it is, but only by accident of promotion, so write `rawKey & 0xFF3F` explicitly.

Hold is `(data[3] & 0xC0) != 0` — **either** bit. The key emulator in legacy
(`CarminatDisplay.cpp:30-58`) sets both (`data[3] |= 0xC0`).

### 7.1 `0x1C1` is not a key-only channel — the `03 89` guard is load-bearing

The corpus contains three distinct panel→radio payloads on `0x1C1` and **none of them is a
key frame**:

```
[RX] ID: 0x1C1 Len: 8 Data: { 70 A3 A3 A3 A3 A3 A3 A3 }   function-registration request
[RX] ID: 0x1C1 Len: 8 Data: { 02 64 0F A3 A3 A3 A3 A3 }   SF_DL 2:  64 0F
[RX] ID: 0x1C1 Len: 8 Data: { 05 63 30 30 33 37 A3 A3 }   SF_DL 5:  63 "0037"
```

The `05 63 …` payload carries four ASCII digits (`"0037"`, `"0155"` also seen); the OEM log
in §1.1 shows the same `02 64 0F` shape from a genuine cluster. Meaning unknown — see
Appendix C. **[CAP]**, **[OEM]**

Two consequences:

* A decoder that reads `data[2..3]` as a key code without first checking `data[0] == 0x03
  && data[1] == 0x89` will manufacture phantom keys `0x640F` and `0x3030` out of this
  traffic. Both legacy implementations do check; keep the check, and add the reject vectors
  below to `test_link`.
* **[CAP] holds no real inbound key frame at all.** Every `03 89 …` in the corpus is our
  own `@TX 1C1 03 89 01 01 00 00 00 00`. The key *decode* is therefore corroborated by
  **[REF]** (`affa3.c:128-137`, same two guard bytes, same `data[2] << 8 | data[3]`),
  **[TWIN]** (`VirtualDisplayBase::pressKey`, same encoding with the same encoder
  exemption) and `notes/AFFA3_SCREENS.md` (`03 89 00 C0` for hold-Load, from a
  `/canstream` capture) — but the **hold mask and the encoder exemption are
  DERIVED-ONLY against a panel-originated frame.** Nothing in the repository shows the
  panel itself emitting a held key. Flag it in `test_link` as such and confirm it on the
  bench before relying on hold semantics for anything but `NavCommand::Open` / `Back`.

---

## 8. Carminat operations

All Carminat text-family operations transmit on `0x151` and are acknowledged on `0x551`.

### 8.1 `setText(const char* text, uint8_t digit)` — `0x151`

`digit` is **ignored** on Carminat (it exists for the UpdateList signature). Payload,
22 bytes:

| Offset | Byte | Meaning |
|---|---|---|
| 0 | `0x10` | ISO-TP first-frame PCI, built by the caller |
| 1 | `0x0E` | declared content length = 14 |
| 2 | `0x77` | mode: `0x74` full window, `0x77` windowed. Source note, verbatim: *"74- full window, 77-not full. if sended not full when not applid - it fill freze at main screen"* — i.e. sending the **windowed** `0x77` while no window is applied leaves the main screen frozen. The note is the only record of this; treat the failure mode as observed, the explanation as unverified |
| 3 | `0x55` | RDS icon: `0x45` AF-RDS icon, `0x55` none |
| 4 | `0x55` | fixed, meaning unknown |
| 5 | `0xFF` | source icon: `0xDF` "MANU", `0xFD` "PRESET", `0xFF` none, others render "LIST" etc. |
| 6 | `0x60` | text format: `0x19`-`0x3F` radio style (5 digits + `.` + 1 char), `0x59`-`0x7F` plain ASCII |
| 7 | `0x01` | control byte, always `0x01` |
| 8..21 | text | `strncpy(buf14, text, 14)` into a zero-initialised `char[15]`: NUL-padded, truncated at 14, never a stray terminator |

Frames: `L = 22` -> `1 + ceil(14/7) = 3`.

```
TX  151  10 0E 77 55 55 FF 60 01     data[0..7]
TX  151  21 <t0..t6>                 data[8..14]
TX  151  22 <t7..t13>                data[15..21]
RX  551  30 01 00 ..   30 01 00 ..   74 ..
```

> **Declared-length discrepancy — do not "fix" it.** `0x0E` says 14 content bytes; the
> caller actually transmits 20 (6-byte header + 14 text). The panel consumes only the
> declared 14, i.e. header + the **first 8** text bytes — which is exactly why the docstring
> says "max 7 characters shown". The remaining 6 text bytes go on the wire, the panel
> ACKs the frames carrying them, and it ignores their content. This is observed working
> behaviour on the live panel. Reproduce it byte-for-byte; shortening the payload to match
> `0x0E` is an untested change.

> **`setText` does NOT transliterate.** `showMenu`, `showConfirmBox`, `showFullscreenText`
> and `showPopupText` all call `transliterateToAscii` first; `setText` and both
> UpdateList `setText`s do not. In the library, transliteration is mandatory on *every*
> string reaching the wire — a deliberate behaviour change, and the only one anywhere in
> this document that can alter a transmitted byte. The other three (Appendix B) change
> timing only.

### 8.2 `setTime(const char* clock)` — `0x151`

`clock` is 4 ASCII digits, `"HHMM"`. Payload, 8 bytes — single frame, no padding:

| Offset | Byte | Meaning |
|---|---|---|
| 0 | `0x05` | ISO-TP single frame, SF_DL = 5 |
| 1 | `0x56` | `'V'`, command byte (constant) |
| 2..5 | `clock[0..3]` | ASCII hours-hours-minutes-minutes |
| 6..7 | `0x00` | padding |

```
TX  151  05 56 <h> <h> <m> <m> 00 00
RX  551  74 ..
```

> Legacy `setTime` discards the `affa3_send` return value and unconditionally returns
> `NoError` (`CarminatDisplay.cpp:678-680`). The library must propagate it.

### 8.3 `setState(bool enabled)` — `0x151`

Payload, 5 bytes:

| Offset | Byte | Meaning |
|---|---|---|
| 0 | `0x03` | ISO-TP single frame, SF_DL = 3 |
| 1 | `0x52` | display-control command |
| 2 | `0x09` enable / `0x00` disable | `Carminat::DisplayCtrl` |
| 3..4 | `0xFF` | fixed |

Filler pads bytes 5..7.

```
TX  151  03 52 09 FF FF 00 00 00     enable
TX  151  03 52 00 FF FF 00 00 00     disable
RX  551  74 ..
```

The in-source comment claims `sc 151 3 52 9 0 0 0 0 0`; the code actually emits the two
`0xFF` bytes. The code is what runs, and what the panel has been accepting.

### 8.4 `highlightItem(uint8_t id)` — `0x151`, **raw**

Sent with `CanUtils::sendFrame` directly — **not** through `affa3_send`. No registration
check, no sync gate, no ACK wait. The panel still emits a `74` on `0x551`; nothing is
listening for it.

| Offset | Byte | Meaning |
|---|---|---|
| 0 | `0x07` | ISO-TP single frame, SF_DL = 7 |
| 1 | `0x29` | highlight command |
| 2 | `0x01` | fixed |
| 3 | `0x7E` when `id == 0`, else `0x7F` | row selector: top / bottom |
| 4 | `0x80` | fixed |
| 5..7 | `0x00` | padding |

```
TX  151  07 29 01 7E 80 00 00 00     highlight top row
TX  151  07 29 01 7F 80 00 00 00     highlight bottom row
```

`0x7E` / `0x7F` are the same row tags that appear inside the `showMenu` screen payload at
payload offsets 4, 38 and 65 — they are row identifiers, not magic numbers.

> **Routing this through the TX state machine is byte-identical.** Fed to `affa3_do_send`
> as an 8-byte payload it produces exactly this one frame (`L = 8` -> one frame, no PCI, no
> filler). The same holds for `hidePopup`/`hideFullscreenText` (§8.8, `L = 3`) and for
> `showInfoMenu` (§8.10, `L = 13` chunks to precisely the two frames it hand-rolls). So the
> library can put all four raw senders behind the single TX FSM with no wire change **per
> message**. The one session-level difference: if `FUNCSREG` is not yet latched, the
> transport prepends the two `0x70` registration frames, which legacy never sent for these
> four. Document that in `docs/API.md`.

### 8.5 `showMenu(header, item1, item2, scrollLockIndicator)` — `0x151`

The 96-byte two-row menu screen. This is the wire-level construct that makes the menu a
library concern rather than an application one.

Payload buffer is `uint8_t payload[96] = {0}` and the builder always ends at exactly 96
bytes (the final `while (idx < 96)` guarantees it), so `L = 96` regardless of string
lengths.

| Offset | Byte(s) | Meaning |
|---|---|---|
| 0 | `0x10` | ISO-TP first frame |
| 1 | `0x5A` | declared content length = 90 |
| 2 | `0x21` | screen command |
| 3 | `0x01` | mode: `0x01` windowed menu (`0x05` = fullscreen, see §8.6) |
| 4 | `0x7E` | first row tag |
| 5 | `0x80` | fixed |
| 6..7 | `0x00` | fixed |
| 8 | `0x82` | fixed |
| 9 | `0xFF` | fixed |
| 10 | scroll indicator | `0x00` none, `0x07` up arrow, `0x0B` down arrow, `0x0C` both |
| 11..36 | header | up to 26 chars, NUL-padded to offset 37 |
| 37 | `0x00` | row-0 index |
| 38 | `0x7E` | row-0 tag (matches `highlightItem(0)`) |
| 39..63 | item1 | up to 25 chars, NUL-padded to offset 64 |
| 64 | `0x01` | row-1 index |
| 65 | `0x7F` | row-1 tag (matches `highlightItem(1)`) |
| 66..95 | item2 | up to 30 chars, NUL-padded to offset 96 |

Frames the builder would emit: `L = 96` -> `1 + ceil(88/7) = 1 + 13 = 14`. Frames 1..12
carry 7 bytes each (84), leaving 4 for frame 13, which is padded with 3 filler `0x00`.
Last PCI = `0x2D`.

**Frames the real panel actually lets out: 13, last PCI `0x2C`.** The declared length
`0x5A` = 90 is satisfied by `6 + 12*7`, the panel answers `74` after PCI `0x2C`, and the
sender breaks. See §3.6 for the proof and the consequences. ACK on a real panel:
`30 01 00` × 12, then `74`. Against `LoopbackLink` or the bench self-ACK emulator:
`30 01 00` × 13, then `74`, and PCI `0x2D` does go out.

**Scroll indicator derivation** (`Menu::getScrollIndicator()`), reproduced in the library
because it is a function of the sliding window, not of the application's content:

```
scroll = 0x0C                                                     // both arrows
if selectedIndex == 0 || (selectedIndex == 1 && selectedRow == 1)
    scroll = 0x0B                                                 // down only
else if selectedIndex == count-1 || (selectedIndex == count-2 && selectedRow == 0)
    scroll = 0x07                                                 // up only
```

Window derivation: `topIndex = (selectedRow == 0) ? selectedIndex : selectedIndex - 1`,
`bottomIndex = topIndex + 1`. A `showMenu` is always immediately followed by a
`highlightItem(selectedRow)` (`Menu::show()` -> `HighlightCurrentSelection()`).

> **Declared-length discrepancy — resolved by capture.** `0x5A` = 90 content bytes, but the
> builder holds 94 (`payload[2..95]`). Against a real panel the last four never leave: the
> transfer ends at PCI `0x2C` (§3.6). item2 is therefore limited to **26 usable characters**
> — `payload[66..91]` — even though the builder accepts 30. Do not change either number
> without panel testing; the screen has been rendering correctly like this for months.

### 8.6 `showFullscreenText(line1, line2, line3)` — `0x151`

Same `0x21` screen command as `showMenu` with mode byte `0x05` instead of `0x01`. The panel
takes the whole screen and renders later menu/volume screens as popups over it.
Reverse-engineered from the OEM "Please insert navigation CD" screen.

Payload is `uint8_t payload[2 + 96]`, so `L = 98` always.

| Offset | Byte(s) | Meaning |
|---|---|---|
| 0 | `0x10` | ISO-TP first frame |
| 1 | `0x60` | declared content length = 96 — **correct**, `payload[2..97]` is exactly 96 |
| 2 | `0x21` | screen command |
| 3 | `0x05` | mode = fullscreen |
| 4 | `0xFF` | fixed |
| 5..6 | `0x00` | fixed |
| 7 | `0x40` | fixed |
| 8..33 | `0x00` | zero header region (content offsets 6..31) |
| 34..97 | text block | pre-filled with `0x20` (space); writing starts at offset 36 (content offset 34), i.e. two leading spaces, as captured. Lines are separated by `0x0D`. |

Line writing stops at `payload[96]` inclusive for characters and appends `0x0D` while
`p < 98`. Empty lines are skipped entirely (no separator emitted for them).

Frames: `L = 98` -> `1 + ceil(90/7) = 1 + 13 = 14`. Frames 1..12 carry 7 (84), total 92,
tail frame carries 6 + 1 filler `0x00`. Last PCI = `0x2D`. Declared `0x60` = 96 needs
`ceil(90/7) = 13` consecutive frames, so all 14 go out — no truncation here (§3.6).

> **Independently corroborated by a transmitter that is not ours.** The OEM head unit's
> own "Please insert navigation CD" screen appears in
> `logs/device-monitor-260616-230730.log` as `[RX] 0x151 { 10 60 21 05 FF 00 00 40 }`
> followed by consecutive frames up to and including PCI `0x2D`, with the panel answering
> `30 01 00` throughout. Header bytes, mode byte, frame count and the `0x0D` line
> separators all match this builder. **[CAP]** — this is the strongest corroboration any
> operation in this document has.

### 8.7 `showPopupText(text, icon, srcIcon, fmt)` — `0x151`

The transient overlay box (the "VOL 28" popup). Same wire family as `setText` but with mode
`0x74` (full-window overlay) instead of `0x77`.

```
tlen = clamp(strlen(text), 8, 16)
L    = 8 + tlen
```

| Offset | Byte | Meaning |
|---|---|---|
| 0 | `0x10` | ISO-TP first frame |
| 1 | `6 + tlen` | declared content length — **correct** here |
| 2 | `0x74` | mode: full-window popup overlay |
| 3 | `icon` | left icon set (capture: `0x09`) |
| 4 | `0x55` | fixed |
| 5 | `srcIcon` | source/right icon (capture: `0xFF` = none) |
| 6 | `fmt` | text format (capture: `0x60` = plain) |
| 7 | `0x01` | control byte |
| 8.. | text | space-padded to `tlen`, truncated at 16 |

Observed property: the popup is a **non-destructive overlay**. The screen underneath keeps
being redrawn while it is up; the popup stays until it auto-reverts or `hidePopup()` closes
it.

Minimum `tlen` of 8 keeps the captured "VOL 28" byte-identical (`10 0E …`).

### 8.8 `hidePopup()` / `hideFullscreenText()` — `0x151`, **raw**

Both emit the identical single frame, directly via `CanUtils::sendCan`, no ACK wait:

```
TX  151  02 54 03 00 00 00 00 00
```

`0x02` is an ISO-TP single frame with SF_DL = 2 (`54 03`). `0x54 0x03` is the close-window
command. The OEM head unit closes the window by re-selecting the audio source; this is the
raw equivalent.

### 8.9 `showConfirmBoxWithOffsets(caption, row1, row2)` — `0x151`

The big confirm box: a captioned button plus two text rows.

Buffer: `0x10 0x6F` + a fixed 6-byte header + a 105-byte zeroed content region.
`L = 2 + 6 + 105 = 113` — the maximum the transport can carry (§3.3).

| Offset | Byte(s) | Meaning |
|---|---|---|
| 0 | `0x10` | ISO-TP first frame |
| 1 | `0x6F` | declared content length = 111 = 6 + 105 — **correct** |
| 2..7 | `21 05 00 00 01 49` | fixed 6-byte header |
| 8..112 | content[0..104] | see below |

Content region layout (offsets are into `content`, add 8 for the payload index):

| Content offset | Meaning |
|---|---|
| `0x1A` (26) .. `0x20` (32) | button caption, max 7 chars, NUL-padded |
| `0x20` (32) .. `0x35` (53) | row1, then `0x0D`, then row2, then `0x0D`; writing is bounded by `off < 0x36` |
| everything else | `0x00` |

Note the caption region and the row region are adjacent: a 7-character caption fills
`content[0x1A..0x20]`, i.e. it can write into offset 32 which row1 then overwrites. Keep the
caption at 6 characters or fewer to stay clear.

Frames: `L = 113` -> `1 + ceil(105/7) = 1 + 15 = 16`, exactly, with no filler in the tail.
Last PCI = `0x2F`.

### 8.10 `showInfoMenu(item1..3, offset1..3, infoPrefix)` — `0x151`, **raw**

The 3-item settings-list popup. Emitted with six raw `CanUtils::sendCan` calls with a
hand-rolled `0x21` continuation and `delay(5)` between frames — it never goes through
`affa3_send`, so there is no registration check, no sync gate and no ACK wait. The `delay`
has no wire meaning and must be dropped in the library.

Per item, two frames:

```
TX  151  10 0B 76 <infoPrefix> <offset> <t0> <t1> <t2>
TX  151  21 <t3> <t4> <t5> <t6> <t7> 00 00
```

| Byte | Meaning |
|---|---|
| `0x10` | ISO-TP first frame |
| `0x0B` | declared content length = 11 = 3 header + 8 text — **correct** |
| `0x76` | info-item command |
| `infoPrefix` | text format, same semantics as `setText` byte 6. `0x60` = plain (OEM capture); `0x70` is the legacy default parameter but `showInfoPopup` passes `0x60` |
| `offset` | row slot. OEM settings list uses `0x41`, `0x44`, `0x48` |
| `t0..t7` | 8 text bytes |

`showInfoPopup(l1, l2, l3)` == `showInfoMenu(l1, l2, l3, 0x41, 0x44, 0x48, 0x60)`.
Reference OEM capture: `76 60 41 .. AUX ON`, `76 60 44 .. AF ON`, `76 60 48 .. SPEED 0`.

> **Trap in the text padding — RESOLVED, and the library diverges here on purpose.**
> The extracted builder wrote `char padded[8] = {' '};`, which initialises `padded[0]` to
> `' '` and `padded[1..7]` to `0` — not eight spaces. `strncpy(padded, text, 8)` then
> overwrites the lot and NUL-pads. Net effect in the legacy code: the text was **NUL**-padded
> to 8, never space-padded, and that was an uncaught bug in an array initialiser rather than
> a decision.
>
> Note what the corpus does and does not witness. The `[CAP-VERBATIM]` evidence for this
> command covers the **first frame of each row only** (`10 0B 76 60 41 41 55 58`, seven
> occurrences each) — and a first frame carries `t0..t2`, i.e. never a pad byte. **No
> capture in the corpus shows a continuation frame of an info row**, so the pad bytes are
> `[DERIVED]` under either reading, and the NUL form printed in earlier revisions of this
> document was derived from the legacy source, not observed on a bus.
>
> `affa::CarminatDisplay::showInfoMenu()` therefore emits the **space**-padded form, per
> project finding #9 ("emit the OEM form"), and the golden vector below has been corrected
> to match. Only `t6`/`t7` of the continuation frame differ between the two readings; every
> `[CAP-VERBATIM]` byte is identical in both. If a bench capture ever shows a real OEM
> continuation frame with `00` in the tail, this is the decision to revisit —
> `test_carminat_wire/test_showInfoPopup_is_three_messages_space_padded` is where it is
> pinned and the vector below is the only other place it appears.

`hideInfoPopup()` is a best-effort dismiss: it just calls `setText("RENAULT", 0)`.

### 8.11 Inbound AUX-mode classifier — observed, and NOT shipped

Not a transmit path, and **not library code**. This was `AuxModeTracker`, behind the
default-off `AFFA_ENABLE_AUX_TRACKER`; both are deleted. The observation is recorded here
because it is a real reading of a real bus, and the application-facing version — the same
table plus the reasons each index and threshold is what it is — is
`docs/PROTOCOL-NOTES.md` §8.

It watches **`0x151` frames sent by the radio**, pairs a `0x10` header frame with the
`0x21` continuation that follows it within 200 ms, and classifies the text. The verdicts
describe one Renault radio family, not the panel and not the protocol:

| Test on the `0x21` frame | Verdict |
|---|---|
| `text[1..3] == "AUX"` | AUX |
| `text[1..7] == "RENAULT"` | radio |
| `text[1..3] == "TR "` and `text[4]` space-or-digit and `text[5]` digit and `text[6] == ' '` and `text[7] == 'C'` | CD |
| `text[1..2] == "> "` and `text[3] != ' '` and `header[6] >= 0x59` | radio (short) |
| `text[1] == 'M'`, `text[2] == ' '`, `text[3]` space-or-digit, `text[4..6]` digits, `text[7] == ' '` | radio (M) |
| `text[1] == 'L'`, `text[2..3] == "  "`, `text[4..6]` digits, `text[7] == ' '` | radio (L) |
| `text[1..3] == "   "`, `text[4..7]` digits, `header[6] < 0x59` | radio (mode 1) |
| otherwise | retain previous state |

`header[6]` is the `setText` text-format byte (§8.1), which is why the `0x59` threshold
appears: it separates radio-style formats (`0x19`-`0x3F`) from plain ASCII (`0x59`-`0x7F`).

---

## 9. UpdateList operations

### 9.1 `setText(const char* text, uint8_t digit)` — segment encoding, `0x121`

The 8+12 "old text / new text" segment encoding.

```
chan = (digit <= 9) ? (0x70 + digit) : 0x7A       // 0x7A = "no channel"
loc  = 0x01
```

Payload, 29 bytes:

| Offset | Byte(s) | Meaning |
|---|---|---|
| 0 | `0x10` | ISO-TP first frame |
| 1 | `0x19` | declared content length = 25 — **correct**, covers `data[2..26]` |
| 2 | `0x76` | text type |
| 3 | `chan` | channel: `0x70 + digit` for digit 0..9, else `0x7A` |
| 4 | `0x01` | location |
| 5..12 | `oldBuf[0..7]` | 8-cell "old text" field |
| 13 | `0x10` | separator |
| 14..25 | `newBuf[0..11]` | 12-cell "new text" field |
| 26 | `0x00` | terminator (last byte inside the declared length) |
| 27..28 | `0x81` | padding, outside the declared length |

Frames: `L = 29` -> `1 + ceil(21/7) = 1 + 3 = 4`, exactly full, no filler. Last PCI `0x23`.

```
TX  121  10 19 76 <chan> 01 <o0> <o1> <o2>
TX  121  21 <o3> <o4> <o5> <o6> <o7> 10 <n0>
TX  121  22 <n1> <n2> <n3> <n4> <n5> <n6> <n7>
TX  121  23 <n8> <n9> <n10> <n11> 00 81 81
RX  521  30 01 00 ..  30 01 00 ..  30 01 00 ..  74 ..
```

> **Padding trap, again.** `char oldBuf[8] = {' '};` / `char newBuf[12] = {' '};` set only
> element 0 to a space and the rest to `0`; `strncpy` then NUL-pads. Both fields are
> **NUL**-padded on the wire, never space-padded.

### 9.2 `setText(const char* text, uint8_t)` — LCD encoding, `0x121`

`UpdateListMenuDisplay` overrides `setText` with the icons variant from the archive affa3
library (`affa3_do_set_text` style). `digit` is ignored. Payload, 30 bytes:

| Offset | Byte(s) | Meaning |
|---|---|---|
| 0 | `0x10` | ISO-TP first frame |
| 1 | `0x1C` | declared content length = 28 — **correct**, covers `data[2..29]` |
| 2 | `0x7F` | fixed |
| 3 | `0x55` | icons: NO_TRAFFIC \| NO_NEWS \| NO_AFRDS \| NO_MODE |
| 4 | `0x55` | literal separator |
| 5 | `0xFF` | icon mode = NONE |
| 6 | `0x60` | channel 0 (LCD encoding is `0x60 \| chan`) |
| 7 | `0x03` | `LOCATION(0,0) \| SELECTED \| FULLSCREEN` |
| 8..15 | `oldBuf[0..7]` | 8-cell "old text" |
| 16 | `0x10` | separator |
| 17..28 | `newBuf[0..11]` | 12-cell "new text" |
| 29 | `0x00` | terminator |

Here `oldBuf` / `newBuf` **are** correctly initialised to all spaces — but `strncpy` still
NUL-pads, so the space initialisation only survives when the source string is at least 8
(resp. 12) characters. In practice the fields are NUL-padded, same as §9.1.

Frames: `L = 30` -> `1 + ceil(22/7) = 1 + 4 = 5`. Frames 1..3 carry 7 (21), total 29, tail
frame carries 1 byte + 6 filler `0x81`. Last PCI `0x24`.

### 9.3 `setState(bool enabled)` — `0x1B1`

Payload, 5 bytes:

| Offset | Byte | Meaning |
|---|---|---|
| 0 | `0x04` | ISO-TP single frame, SF_DL = 4 |
| 1 | `0x52` | display-control command |
| 2 | `0x02` enable / `0x00` disable | `UpdateList::DisplayCtrl` |
| 3..4 | `0xFF` | fixed |

Filler `0x81` pads 5..7.

```
TX  1B1  04 52 02 FF FF 81 81 81     enable
TX  1B1  04 52 00 FF FF 81 81 81     disable
RX  5B1  74 ..
```

Note the differences from Carminat `setState` (§8.3): different id, different SF_DL
(`0x04` vs `0x03`), different enable value (`0x02` vs `0x09`), different filler.

### 9.4 `setTime()` — not supported

`UpdateListBase::setTime` returns `NoError` and puts nothing on the wire.

### 9.5 `showMenu()` — not supported

`UpdateListBase::showMenu` returns `NoError` and puts nothing on the wire. The UpdateList
panel has no two-row menu screen; menu rendering for that family goes through `setText`.

### 9.6 Inbound radio text — `0x121`

When the **radio** transmits on `0x121`, the UpdateList display decodes it (and does **not**
acknowledge it):

```
data[0] == 0x10 && data[1] == 0x19          // the segment setText encoding, §9.1
    isAux = (data[5] == 'A' && data[6] == 'U' && data[7] == 'X')
```

`data[5..7]` are the first three cells of the "old text" field. `isAux` drives
`onRadioText()`.

---

## Golden vectors

Ready to paste. `affa::Frame` is `{ id, len, data[8], extended }`. Every frame is
DLC 8, standard identifier. Arithmetic for each vector is shown immediately above it.

Each vector is tagged with its strongest witness:

* **[CAP-VERBATIM]** — every byte of this vector appears as a literal line in
  `MeganeCAN/logs/*.log`. Trust it.
* **[CAP-PATTERN]** — the header/framing is capture-verbatim; only the text bytes are
  substituted. Trust the framing.
* **[DERIVED]** — hand-executed from the builder, no capture. Review it before it becomes a
  test's definition of correct.

### Registration

```cpp
// Carminat lazy registration: affa3_send() walks funcs[] = {0x151, 0x1F1} in order.
// payload = {0x70}, L = 1 -> 1 frame, 1 payload byte + 7 filler (0x00).
// [CAP-VERBATIM] for the 0x1F1 frame: logs contain `0x1F1 { 70 00 00 00 00 00 00 00 }`.
static const affa::Frame kCarminatRegister[] = {
  { 0x151, 8, {0x70,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },   // expect 0x551: 74
  { 0x1F1, 8, {0x70,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },   // expect 0x5F1: 74
};

// UpdateList lazy registration: funcs[] = {0x121, 0x1B1}, filler 0x81.
static const affa::Frame kUpdateListRegister[] = {
  { 0x121, 8, {0x70,0x81,0x81,0x81,0x81,0x81,0x81,0x81}, false },   // expect 0x521: 74
  { 0x1B1, 8, {0x70,0x81,0x81,0x81,0x81,0x81,0x81,0x81}, false },   // expect 0x5B1: 74
};
```

### Sync

```cpp
// Reply to RX 3CF "61 11 xx ..". Carminat sends three frames; the second and third
// are IDENTICAL (two sendCan calls in the legacy source, present in the capture).
static const affa::Frame kCarminatHello[] = {
  { 0x3AF, 8, {0x70,0x1A,0x11,0x00,0x00,0x00,0x00,0x01}, false },
  { 0x3AF, 8, {0xB0,0x14,0x11,0x00,0x1F,0x00,0x00,0x00}, false },
  { 0x3AF, 8, {0xB0,0x14,0x11,0x00,0x1F,0x00,0x00,0x00}, false },
};

static const affa::Frame kUpdateListHello[] = {
  { 0x3DF, 8, {0x70,0x1A,0x11,0x00,0x00,0x00,0x00,0x01}, false },
};

// 1 Hz alive heartbeat. data[1] is a literal 0x00 in BOTH families -- for UpdateList
// that is NOT the filler (0x81).
static const affa::Frame kCarminatAlive =
  { 0x3AF, 8, {0xB9,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false };
static const affa::Frame kUpdateListAlive =
  { 0x3DF, 8, {0x79,0x00,0x81,0x81,0x81,0x81,0x81,0x81}, false };

// Sync request, emitted only while FAILED or START is set. Carminat's arg byte is the
// filler (0x00); UpdateList's is a literal 0x01.
static const affa::Frame kCarminatSyncRequest =
  { 0x3AF, 8, {0xBA,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false };
static const affa::Frame kUpdateListSyncRequest =
  { 0x3DF, 8, {0x7A,0x01,0x81,0x81,0x81,0x81,0x81,0x81}, false };

// Auto-ACK we emit for any non-sync, non-reply-flagged inbound frame. Filler differs.
static const affa::Frame kCarminatAckToKeyId =
  { 0x5C1, 8, {0x74,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false };   // RX 1C1 -> TX 5C1
static const affa::Frame kUpdateListAckToKeyId =
  { 0x4A9, 8, {0x74,0x81,0x81,0x81,0x81,0x81,0x81,0x81}, false };   // RX 0A9 -> TX 4A9
```

### Carminat `setText("HELLO")`, digit ignored

```
Header  = 10 0E 77 55 55 FF 60 01                        (8 bytes)
Text    = strncpy into char[15]={0}: 'H','E','L','L','O' + 9 NUL, truncated at 14
L       = 8 + 14 = 22
frames  = 1 + ceil((22-8)/7) = 1 + 2 = 3
frame 0 = payload[0..7]   (8 bytes, no PCI added)
frame 1 = 0x21 + payload[8..14]  = text[0..6]   -> 'H','E','L','L','O',0,0
frame 2 = 0x22 + payload[15..21] = text[7..13]  -> 0,0,0,0,0,0,0
tail padding = none (7 bytes exactly)
```

```cpp
static const affa::Frame kCarminatSetTextHello[] = {
  { 0x151, 8, {0x10,0x0E,0x77,0x55,0x55,0xFF,0x60,0x01}, false },
  { 0x151, 8, {0x21,'H','E','L','L','O',0x00,0x00},      false },
  { 0x151, 8, {0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
};
// ACK on 0x551: 30 01 00 .. , 30 01 00 .. , 74 ..
```

### Carminat `setText("RENAULT")` — the menu-close default

```
'R'=0x52 'E'=0x45 'N'=0x4E 'A'=0x41 'U'=0x55 'L'=0x4C 'T'=0x54
text[0..6] = 52 45 4E 41 55 4C 54   (all 7 characters land in frame 1)
text[7..13] = 7 NUL
```

```cpp
// [CAP-VERBATIM] -- all three lines appear consecutively in
// logs/device-monitor-260616-235529.log:
//   @TX 151 10 0E 77 55 55 FF 60 01
//   @TX 151 21 52 45 4E 41 55 4C 54
//   @TX 151 22 00 00 00 00 00 00 00
static const affa::Frame kCarminatSetTextRenault[] = {
  { 0x151, 8, {0x10,0x0E,0x77,0x55,0x55,0xFF,0x60,0x01}, false },
  { 0x151, 8, {0x21,0x52,0x45,0x4E,0x41,0x55,0x4C,0x54}, false },
  { 0x151, 8, {0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
};
```

The same file gives the full ACK interleave for an 8-character string, straight off a real
panel — this is the canonical `test_isotp` fixture for a 3-frame transfer:

```
@TX 151 10 0E 77 55 55 FF 60 01
[RX] 0x551 { 30 01 00 A3 A3 A3 A3 A3 }      PARTIAL
@TX 151 21 42 6C 61 62 6C 61 62             "Blablab"
[RX] 0x551 { 30 01 00 A3 A3 A3 A3 A3 }      PARTIAL
@TX 151 22 61 00 00 00 00 00 00             "a" + 6 NUL
[RX] 0x551 { 74 A3 A3 A3 A3 A3 A3 A3 }      DONE
```

### Carminat `setTime("1234")`

```
L = 8 -> 1 frame, no PCI counter, no padding.
'1'=0x31 '2'=0x32 '3'=0x33 '4'=0x34 ; 'V'=0x56
```

```cpp
static const affa::Frame kCarminatSetTime1234[] = {
  { 0x151, 8, {0x05,0x56,0x31,0x32,0x33,0x34,0x00,0x00}, false },
};
// ACK on 0x551: 74 ..
```

### Carminat `setState`

```
L = 5 -> 1 frame, 5 payload bytes + 3 filler (0x00).
```

```cpp
// [CAP-VERBATIM] -- @TX 151 03 52 09 FF FF 00 00 00  (x42)
//                   @TX 151 03 52 00 FF FF 00 00 00  (x11)
static const affa::Frame kCarminatSetStateEnable[] = {
  { 0x151, 8, {0x03,0x52,0x09,0xFF,0xFF,0x00,0x00,0x00}, false },
};
static const affa::Frame kCarminatSetStateDisable[] = {
  { 0x151, 8, {0x03,0x52,0x00,0xFF,0xFF,0x00,0x00,0x00}, false },
};
```

### Carminat `highlightItem`

```
Raw send, no ISO-TP counter, no ACK wait. SF_DL = 7 covers bytes 1..7.
```

```cpp
// [CAP-VERBATIM] -- @TX 151 07 29 01 7E 80 00 00 00  (x11)
//                   @TX 151 07 29 01 7F 80 00 00 00  (x12)
static const affa::Frame kCarminatHighlightTop[] = {
  { 0x151, 8, {0x07,0x29,0x01,0x7E,0x80,0x00,0x00,0x00}, false },
};
static const affa::Frame kCarminatHighlightBottom[] = {
  { 0x151, 8, {0x07,0x29,0x01,0x7F,0x80,0x00,0x00,0x00}, false },
};
```

### Carminat `showMenu("Main Menu", "Voltage:0V", "Boost:0mbar", 0x0B)`

```
payload[96], zero-initialised. Layout walk:
  [0..7]   10 5A 21 01 7E 80 00 00
  [8..10]  82 FF 0B                      (0x0B = down arrow only)
  [11..19] "Main Menu"  = 4D 61 69 6E 20 4D 65 6E 75      (9 chars)
  [20..36] 00 x17                        pad to offset 37
  [37]     00                            row-0 index
  [38]     7E                            row-0 tag
  [39..48] "Voltage:0V" = 56 6F 6C 74 61 67 65 3A 30 56   (10 chars)
  [49..63] 00 x15                        pad to offset 64
  [64]     01                            row-1 index
  [65]     7F                            row-1 tag
  [66..76] "Boost:0mbar"= 42 6F 6F 73 74 3A 30 6D 62 61 72 (11 chars)
  [77..95] 00 x19                        pad to offset 96
L = 96
frames the builder would emit = 1 + ceil((96-8)/7) = 1 + ceil(88/7) = 1 + 13 = 14
frames 1..12 carry 7 bytes each = 84; 8 + 84 = 92; tail frame 13 carries 4 + 3 filler
last PCI built = 0x20 + 13 = 0x2D
frame n (n>=1) carries payload[8 + 7*(n-1) .. 8 + 7*n - 1]

BUT declared FF_DL = 0x5A = 90, and 6 + 12*7 = 90 exactly, so a real panel answers DONE
after PCI 0x2C and the sender breaks:
frames ON THE WIRE = 13, last PCI = 0x2C.        <- see WIRE-SPEC 3.6
```

```cpp
// [DERIVED] framing is [CAP-VERBATIM] (see kCarminatShowMenuCaptured below); only the
// strings are substituted. The LAST ENTRY IS EMULATOR-ONLY -- against a real panel the
// transfer ends at 0x2C. Slice to 13 for the hardware expectation.
static const affa::Frame kCarminatShowMenuMain[] = {
  { 0x151, 8, {0x10,0x5A,0x21,0x01,0x7E,0x80,0x00,0x00}, false },  // payload[0..7]
  { 0x151, 8, {0x21,0x82,0xFF,0x0B,0x4D,0x61,0x69,0x6E}, false },  // [8..14]  82 FF 0B M a i n
  { 0x151, 8, {0x22,0x20,0x4D,0x65,0x6E,0x75,0x00,0x00}, false },  // [15..21] ' ' M e n u 00 00
  { 0x151, 8, {0x23,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // [22..28]
  { 0x151, 8, {0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // [29..35]
  { 0x151, 8, {0x25,0x00,0x00,0x7E,0x56,0x6F,0x6C,0x74}, false },  // [36..42] 00 00 7E V o l t
  { 0x151, 8, {0x26,0x61,0x67,0x65,0x3A,0x30,0x56,0x00}, false },  // [43..49] a g e : 0 V 00
  { 0x151, 8, {0x27,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // [50..56]
  { 0x151, 8, {0x28,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // [57..63]
  { 0x151, 8, {0x29,0x01,0x7F,0x42,0x6F,0x6F,0x73,0x74}, false },  // [64..70] 01 7F B o o s t
  { 0x151, 8, {0x2A,0x3A,0x30,0x6D,0x62,0x61,0x72,0x00}, false },  // [71..77] : 0 m b a r 00
  { 0x151, 8, {0x2B,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // [78..84]
  { 0x151, 8, {0x2C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // [85..91]  <- REAL PANEL STOPS HERE
  { 0x151, 8, {0x2D,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // [92..95] + 3 filler -- EMULATOR ONLY
};
static constexpr size_t kCarminatShowMenuMain_HW  = 13;  // frames on a real panel
static constexpr size_t kCarminatShowMenuMain_EMU = 14;  // frames on LoopbackLink / self-ACK
// ACK on 0x551, real panel: 30 01 00 x12, then 74.  Emulator: 30 01 00 x13, then 74.
// Followed immediately by kCarminatHighlightTop or kCarminatHighlightBottom.
```

### Carminat `showMenu` — the full capture-verbatim vector

Lifted line for line out of `logs/device-monitor-260616-230730.log`, a session against the
real panel. `showMenu("MeganeCAN", "Waiting for phone", "for AMS device", 0x00)`. **This is
the vector `test_isotp` should pin**, because every byte of it was observed, including the
termination point.

```cpp
// [CAP-VERBATIM] -- 13 frames, ends at 0x2C, DONE follows immediately.
static const affa::Frame kCarminatShowMenuCaptured[] = {
  { 0x151, 8, {0x10,0x5A,0x21,0x01,0x7E,0x80,0x00,0x00}, false },
  { 0x151, 8, {0x21,0x82,0xFF,0x00,0x4D,0x65,0x67,0x61}, false },  // 82 FF 00 "Mega"
  { 0x151, 8, {0x22,0x6E,0x65,0x43,0x41,0x4E,0x00,0x00}, false },  // "neCAN"
  { 0x151, 8, {0x23,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
  { 0x151, 8, {0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
  { 0x151, 8, {0x25,0x00,0x00,0x7E,0x57,0x61,0x69,0x74}, false },  // 00 00 7E "Wait"
  { 0x151, 8, {0x26,0x69,0x6E,0x67,0x20,0x66,0x6F,0x72}, false },  // "ing for"
  { 0x151, 8, {0x27,0x20,0x70,0x68,0x6F,0x6E,0x65,0x00}, false },  // " phone"
  { 0x151, 8, {0x28,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
  { 0x151, 8, {0x29,0x01,0x7F,0x66,0x6F,0x72,0x20,0x41}, false },  // 01 7F "for A"
  { 0x151, 8, {0x2A,0x4D,0x53,0x20,0x64,0x65,0x76,0x69}, false },  // "MS devi"
  { 0x151, 8, {0x2B,0x63,0x65,0x00,0x00,0x00,0x00,0x00}, false },  // "ce"
  { 0x151, 8, {0x2C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // panel: 74 (DONE)
};
```

The same source file also holds the 14-frame emulator form, with a different header, for
the loopback expectation (`[route] virtual`,
`logs/device-monitor-260616-215143.log`):

```cpp
// [CAP-VERBATIM] -- showMenu("MeganeCAN", "Hello", "from your ESP", 0x0C) through the
// bench self-ACK emulator: all 14 frames go out because our own code fakes PARTIAL.
static const affa::Frame kCarminatShowMenuLoopback[] = {
  { 0x151, 8, {0x10,0x5A,0x21,0x01,0x7E,0x80,0x00,0x00}, false },
  { 0x151, 8, {0x21,0x82,0xFF,0x0C,0x4D,0x65,0x67,0x61}, false },
  { 0x151, 8, {0x22,0x6E,0x65,0x43,0x41,0x4E,0x00,0x00}, false },
  { 0x151, 8, {0x23,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
  { 0x151, 8, {0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
  { 0x151, 8, {0x25,0x00,0x00,0x7E,0x48,0x65,0x6C,0x6C}, false },
  { 0x151, 8, {0x26,0x6F,0x00,0x00,0x00,0x00,0x00,0x00}, false },
  { 0x151, 8, {0x27,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
  { 0x151, 8, {0x28,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
  { 0x151, 8, {0x29,0x01,0x7F,0x66,0x72,0x6F,0x6D,0x20}, false },
  { 0x151, 8, {0x2A,0x79,0x6F,0x75,0x72,0x20,0x45,0x53}, false },
  { 0x151, 8, {0x2B,0x50,0x00,0x00,0x00,0x00,0x00,0x00}, false },
  { 0x151, 8, {0x2C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
  { 0x151, 8, {0x2D,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },
};
```

Assert **both**. A test that only knows one of them will either pass on the host and fail on
the bench, or the reverse — and the difference is a single frame that is easy to blame on
anything else.

### Carminat `showFullscreenText("PLEASE INSERT", "NAVIGATION CD", "")`

```
payload[98]:
  [0..7]   10 60 21 05 FF 00 00 40
  [8..33]  00 x26                    zero header region (content 6..31)
  [34..97] pre-filled 0x20
  p starts at 36 (content offset 34 -> two leading spaces at [34],[35])
  "PLEASE INSERT" -> [36..48] = 50 4C 45 41 53 45 20 49 4E 53 45 52 54 ; [49] = 0D
  "NAVIGATION CD" -> [50..62] = 4E 41 56 49 47 41 54 49 4F 4E 20 43 44 ; [63] = 0D
  line3 empty -> skipped entirely, no separator
  [64..97] remain 0x20
L = 98
frames = 1 + ceil(90/7) = 1 + 13 = 14
frames 1..12 carry 7 = 84 -> 92 consumed; tail frame 13 carries 6 + 1 filler (0x00)
last PCI = 0x2D
```

```cpp
static const affa::Frame kCarminatFullscreenNavCd[] = {
  { 0x151, 8, {0x10,0x60,0x21,0x05,0xFF,0x00,0x00,0x40}, false },  // [0..7]
  { 0x151, 8, {0x21,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // [8..14]
  { 0x151, 8, {0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // [15..21]
  { 0x151, 8, {0x23,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // [22..28]
  { 0x151, 8, {0x24,0x00,0x00,0x00,0x00,0x00,0x20,0x20}, false },  // [29..35] 5 zeros + 2 spaces
  { 0x151, 8, {0x25,0x50,0x4C,0x45,0x41,0x53,0x45,0x20}, false },  // [36..42] P L E A S E ' '
  { 0x151, 8, {0x26,0x49,0x4E,0x53,0x45,0x52,0x54,0x0D}, false },  // [43..49] I N S E R T CR
  { 0x151, 8, {0x27,0x4E,0x41,0x56,0x49,0x47,0x41,0x54}, false },  // [50..56] N A V I G A T
  { 0x151, 8, {0x28,0x49,0x4F,0x4E,0x20,0x43,0x44,0x0D}, false },  // [57..63] I O N ' ' C D CR
  { 0x151, 8, {0x29,0x20,0x20,0x20,0x20,0x20,0x20,0x20}, false },  // [64..70]
  { 0x151, 8, {0x2A,0x20,0x20,0x20,0x20,0x20,0x20,0x20}, false },  // [71..77]
  { 0x151, 8, {0x2B,0x20,0x20,0x20,0x20,0x20,0x20,0x20}, false },  // [78..84]
  { 0x151, 8, {0x2C,0x20,0x20,0x20,0x20,0x20,0x20,0x20}, false },  // [85..91]
  { 0x151, 8, {0x2D,0x20,0x20,0x20,0x20,0x20,0x20,0x00}, false },  // [92..97] + 1 filler
};
```

### Carminat `showPopupText("VOL 28", 0x09, 0xFF, 0x60)` — the OEM volume popup

```
tlen = clamp(6, 8, 16) = 8 ; declared length = 6 + 8 = 0x0E
text cells = 'V','O','L',' ','2','8',' ',' '   (space-padded, NOT NUL-padded here)
L = 8 + 8 = 16
frames = 1 + ceil(8/7) = 1 + 2 = 3
frame 1 = 0x21 + data[8..14] = text[0..6]
frame 2 = 0x22 + data[15]    = text[7]  + 6 filler (0x00)
```

```cpp
// [CAP-VERBATIM] -- all three lines appear in logs/device-monitor-260617-004150.log:
//   @TX 151 10 0E 74 09 55 FF 60 01
//   @TX 151 21 56 4F 4C 20 32 38 20
//   @TX 151 22 20 00 00 00 00 00 00
// and the same file carries the ACK trace:
//   RX 551 30 01 00 A3 ... -> "PARTIAL ack on packet #0, remaining: 8 bytes"
static const affa::Frame kCarminatPopupVol28[] = {
  { 0x151, 8, {0x10,0x0E,0x74,0x09,0x55,0xFF,0x60,0x01}, false },
  { 0x151, 8, {0x21,0x56,0x4F,0x4C,0x20,0x32,0x38,0x20}, false },  // V O L ' ' 2 8 ' '
  { 0x151, 8, {0x22,0x20,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // ' ' + 6 filler
};
```

### Carminat `hidePopup()` / `hideFullscreenText()`

```cpp
// [CAP-VERBATIM] -- @TX 151 02 54 03 00 00 00 00 00 (x9), and independently observed
// FROM THE OEM HEAD UNIT: [RX] ID: 0x151 { 02 54 03 00 00 00 00 00 }.
static const affa::Frame kCarminatCloseWindow[] = {
  { 0x151, 8, {0x02,0x54,0x03,0x00,0x00,0x00,0x00,0x00}, false },  // raw, no ACK wait
};
```

### Carminat `showConfirmBoxWithOffsets("OK", "Line one", "Line two")`

```
content[105] zeroed:
  content[0x1A]=0x4F 'O', content[0x1B]=0x4B 'K'
  off = 0x20: "Line one" -> content[32..39] = 4C 69 6E 65 20 6F 6E 65 ; content[40] = 0D
  off = 41  : "Line two" -> content[41..48] = 4C 69 6E 65 20 74 77 6F ; content[49] = 0D
buf = 10 6F 21 05 00 00 01 49 | content[0..104]      -> buf[8+k] == content[k]
L = 2 + 6 + 105 = 113
frames = 1 + ceil(105/7) = 1 + 15 = 16, exactly full, NO tail padding
last PCI = 0x20 + 15 = 0x2F  (the counter's absolute ceiling -- see WIRE-SPEC 3.3)
```

```cpp
static const affa::Frame kCarminatConfirmBoxOk[] = {
  { 0x151, 8, {0x10,0x6F,0x21,0x05,0x00,0x00,0x01,0x49}, false },  // buf[0..7]
  { 0x151, 8, {0x21,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[0..6]
  { 0x151, 8, {0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[7..13]
  { 0x151, 8, {0x23,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[14..20]
  { 0x151, 8, {0x24,0x00,0x00,0x00,0x00,0x00,0x4F,0x4B}, false },  // content[21..27] -> 'O','K' at 26,27
  { 0x151, 8, {0x25,0x00,0x00,0x00,0x00,0x4C,0x69,0x6E}, false },  // content[28..34] -> 'L','i','n' at 32..34
  { 0x151, 8, {0x26,0x65,0x20,0x6F,0x6E,0x65,0x0D,0x4C}, false },  // content[35..41] -> "e one" CR 'L'
  { 0x151, 8, {0x27,0x69,0x6E,0x65,0x20,0x74,0x77,0x6F}, false },  // content[42..48] -> "ine two"
  { 0x151, 8, {0x28,0x0D,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[49..55] -> CR
  { 0x151, 8, {0x29,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[56..62]
  { 0x151, 8, {0x2A,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[63..69]
  { 0x151, 8, {0x2B,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[70..76]
  { 0x151, 8, {0x2C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[77..83]
  { 0x151, 8, {0x2D,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[84..90]
  { 0x151, 8, {0x2E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[91..97]
  { 0x151, 8, {0x2F,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // content[98..104]
};
// ACK on 0x551: 30 01 00 x15, then 74.
```

### Carminat `showInfoPopup("AUX ON", "AF ON", "SPEED 0")`

```
== showInfoMenu(l1, l2, l3, offset1=0x41, offset2=0x44, offset3=0x48, prefix=0x60)
ONE MESSAGE PER ROW through the TX FSM (the legacy raw sends and their delay(5) are gone).
Text is SPACE-padded to 8 -- see the padding note in 8.10; the legacy NUL form was an
array-initialiser bug and no capture witnesses a continuation frame either way.
  "AUX ON"  -> 41 55 58 20 4F 4E 20 20
  "AF ON"   -> 41 46 20 4F 4E 20 20 20
  "SPEED 0" -> 53 50 45 45 44 20 30 20
frame A = 10 0B 76 <prefix> <offset> t0 t1 t2
frame B = 21 t3 t4 t5 t6 t7 <filler> <filler>      filler = 0x00 on Carminat
```

```cpp
// [CAP-VERBATIM] for all three FIRST frames -- the corpus holds, 7 times each:
//   @TX 151 10 0B 76 60 41 41 55 58      offset 0x41, "AUX..."
//   @TX 151 10 0B 76 60 44 41 55 54      offset 0x44, "AUT..."   (real text: AUTO)
//   @TX 151 10 0B 76 60 48 53 50 45      offset 0x48, "SPE..."   (real text: SPEED)
// Note the real capture's middle row was "AUTO", not "AF ON" -- offsets 41/44/48 and
// prefix 0x60 are the fixed part; the text is the caller's.
// The two trailing 00s of every continuation frame are ISO-TP FILLER (Carminat pads 0x00),
// not text. The text bytes are t3..t7, space-padded -- see 8.10.
static const affa::Frame kCarminatInfoPopupOem[] = {
  { 0x151, 8, {0x10,0x0B,0x76,0x60,0x41,0x41,0x55,0x58}, false },  // "AUX ON"  A U X
  { 0x151, 8, {0x21,0x20,0x4F,0x4E,0x20,0x20,0x00,0x00}, false },  //           ' ' O N + pad
  { 0x151, 8, {0x10,0x0B,0x76,0x60,0x44,0x41,0x46,0x20}, false },  // "AF ON"   A F ' '
  { 0x151, 8, {0x21,0x4F,0x4E,0x20,0x20,0x20,0x00,0x00}, false },  //           O N + pad
  { 0x151, 8, {0x10,0x0B,0x76,0x60,0x48,0x53,0x50,0x45}, false },  // "SPEED 0" S P E
  { 0x151, 8, {0x21,0x45,0x44,0x20,0x30,0x20,0x00,0x00}, false },  //           E D ' ' 0 + pad
};
```

### UpdateList `setText("AUX", 255)` — segment encoding

```
digit = 255 > 9  -> chan = 0x7A ; loc = 0x01 ; textType = 0x76
oldBuf[8]  = strncpy("AUX") -> 41 55 58 00 00 00 00 00     (NUL-padded, see 9.1)
newBuf[12] = strncpy("AUX") -> 41 55 58 00 00 00 00 00 00 00 00 00
data[29]:
  [0]=10 [1]=19 [2]=76 [3]=7A [4]=01
  [5..12]  = oldBuf[0..7]
  [13]     = 10                       separator
  [14..25] = newBuf[0..11]
  [26]=00 [27]=81 [28]=81
L = 29
frames = 1 + ceil(21/7) = 1 + 3 = 4, exactly full, no filler in the tail
last PCI = 0x23
frame 1 = 0x21 + data[8..14]  = old3,old4,old5,old6,old7,0x10,new0
frame 2 = 0x22 + data[15..21] = new1..new7
frame 3 = 0x23 + data[22..28] = new8,new9,new10,new11,0x00,0x81,0x81
```

```cpp
static const affa::Frame kUpdateListSetTextAux[] = {
  { 0x121, 8, {0x10,0x19,0x76,0x7A,0x01,0x41,0x55,0x58}, false },  // 'A','U','X' = old[0..2]
  { 0x121, 8, {0x21,0x00,0x00,0x00,0x00,0x00,0x10,0x41}, false },  // old[3..7], sep, new[0]='A'
  { 0x121, 8, {0x22,0x55,0x58,0x00,0x00,0x00,0x00,0x00}, false },  // new[1..7] = 'U','X',0,0,0,0,0
  { 0x121, 8, {0x23,0x00,0x00,0x00,0x00,0x00,0x81,0x81}, false },  // new[8..11], 0x00, 81, 81
};
// ACK on 0x521: 30 01 00 x3, then 74.
```

Channel byte for the digit variants: `digit = 0 -> 0x70`, `digit = 9 -> 0x79`,
`digit > 9 -> 0x7A`. Only `data[3]` of frame 0 changes.

### UpdateList `setText("MENU")` — LCD (`UpdateListMenuDisplay`) encoding

```
oldBuf[8]  = strncpy("MENU")  -> 4D 45 4E 55 00 00 00 00        (strncpy NUL-pads over
newBuf[12] = strncpy("MENU")  -> 4D 45 4E 55 00 x8               the space initialiser)
data[30]:
  [0..7]   10 1C 7F 55 55 FF 60 03
  [8..15]  oldBuf[0..7]
  [16]     10                        separator
  [17..28] newBuf[0..11]
  [29]     00                        terminator
L = 30
frames = 1 + ceil(22/7) = 1 + 4 = 5
frames 1..3 carry 7 each = 21 -> 29 consumed; tail frame 4 carries 1 byte + 6 filler (0x81)
last PCI = 0x24
frame 1 = 0x21 + data[8..14]  = old0..old6
frame 2 = 0x22 + data[15..21] = old7, 0x10, new0..new4
frame 3 = 0x23 + data[22..28] = new5..new11
frame 4 = 0x24 + data[29]     = 0x00 + 6 x 0x81
```

```cpp
static const affa::Frame kUpdateListLcdSetTextMenu[] = {
  { 0x121, 8, {0x10,0x1C,0x7F,0x55,0x55,0xFF,0x60,0x03}, false },
  { 0x121, 8, {0x21,0x4D,0x45,0x4E,0x55,0x00,0x00,0x00}, false },  // old[0..6] M E N U 0 0 0
  { 0x121, 8, {0x22,0x00,0x10,0x4D,0x45,0x4E,0x55,0x00}, false },  // old[7], sep, new[0..4]
  { 0x121, 8, {0x23,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // new[5..11]
  { 0x121, 8, {0x24,0x00,0x81,0x81,0x81,0x81,0x81,0x81}, false },  // terminator + 6 filler
};
// ACK on 0x521: 30 01 00 x4, then 74.
```

### UpdateList `setState`

```
L = 5 -> 1 frame, 5 payload bytes + 3 filler (0x81). SF_DL = 4 (not 3, unlike Carminat).
```

```cpp
static const affa::Frame kUpdateListSetStateEnable[] = {
  { 0x1B1, 8, {0x04,0x52,0x02,0xFF,0xFF,0x81,0x81,0x81}, false },
};
static const affa::Frame kUpdateListSetStateDisable[] = {
  { 0x1B1, 8, {0x04,0x52,0x00,0xFF,0xFF,0x81,0x81,0x81}, false },
};
```

### Inbound key frames (decode vectors)

```cpp
struct KeyVector { affa::Frame in; uint16_t key; bool hold; };

// Carminat panel keys arrive on 0x1C1; UpdateList on 0x0A9. Bytes 4..7 are don't-care
// (the real panel fills them with 0xA3); they are shown as 0x00 as the emulator sends them.
static const KeyVector kCarminatKeyVectors[] = {
  // Load, short press: rawKey 0x0000, no mask bits -> hold=false
  { { 0x1C1, 8, {0x03,0x89,0x00,0x00,0,0,0,0}, false }, 0x0000, false },
  // Load, hold: data[3] |= 0xC0 -> raw 0x00C0, masked 0x0000, hold=true
  { { 0x1C1, 8, {0x03,0x89,0x00,0xC0,0,0,0,0}, false }, 0x0000, true  },
  { { 0x1C1, 8, {0x03,0x89,0x00,0x01,0,0,0,0}, false }, 0x0001, false },  // SrcRight
  { { 0x1C1, 8, {0x03,0x89,0x00,0xC1,0,0,0,0}, false }, 0x0001, true  },  // SrcRight hold
  { { 0x1C1, 8, {0x03,0x89,0x00,0x02,0,0,0,0}, false }, 0x0002, false },  // SrcLeft
  { { 0x1C1, 8, {0x03,0x89,0x00,0x03,0,0,0,0}, false }, 0x0003, false },  // VolumeUp
  { { 0x1C1, 8, {0x03,0x89,0x00,0x04,0,0,0,0}, false }, 0x0004, false },  // VolumeDown
  { { 0x1C1, 8, {0x03,0x89,0x00,0x05,0,0,0,0}, false }, 0x0005, false },  // Pause
  // Encoder detents: EXEMPT from hold masking. Without the exemption, RollDown's 0x40
  // low-byte bit is a hold bit and 0x0141 would be rewritten to 0x0101 = RollUp.
  { { 0x1C1, 8, {0x03,0x89,0x01,0x01,0,0,0,0}, false }, 0x0101, false },  // RollUp
  { { 0x1C1, 8, {0x03,0x89,0x01,0x41,0,0,0,0}, false }, 0x0141, false },  // RollDown
  // Held detent: NOT exempt (0x01C1 != 0x0101 && != 0x0141), so it is masked to RollUp.
  // 0x0101|0xC0 and 0x0141|0xC0 are both 0x01C1 -- direction is unrecoverable. See §7.
  { { 0x1C1, 8, {0x03,0x89,0x01,0xC1,0,0,0,0}, false }, 0x0101, true  },
};

// Same decode on the UpdateList identifier.
static const KeyVector kUpdateListKeyVectors[] = {
  { { 0x0A9, 8, {0x03,0x89,0x00,0x00,0,0,0,0}, false }, 0x0000, false },
  { { 0x0A9, 8, {0x03,0x89,0x00,0xC0,0,0,0,0}, false }, 0x0000, true  },
  { { 0x0A9, 8, {0x03,0x89,0x01,0x41,0,0,0,0}, false }, 0x0141, false },
};

// Rejected: byte 0 / byte 1 must be exactly 03 89. The first three are [CAP-VERBATIM]
// panel->radio traffic on the KEY id -- a decoder without the guard invents keys 0x640F
// and 0x3030 out of them. See §7.1.
static const affa::Frame kNotAKeyFrame[] = {
  { 0x1C1, 8, {0x70,0xA3,0xA3,0xA3,0xA3,0xA3,0xA3,0xA3}, false },  // panel registration req
  { 0x1C1, 8, {0x02,0x64,0x0F,0xA3,0xA3,0xA3,0xA3,0xA3}, false },  // panel, meaning unknown
  { 0x1C1, 8, {0x05,0x63,0x30,0x30,0x33,0x37,0xA3,0xA3}, false },  // panel, 63 "0037"
  { 0x1C1, 8, {0x03,0x88,0x00,0x00,0x00,0x00,0x00,0x00}, false },  // wrong byte 1
};
// Carminat still emits kCarminatAckToKeyId for BOTH of these (the ACK is unconditional
// and precedes the validity check). UpdateList emits the ACK for the first and NOT for
// the second (03 with a non-0x89 second byte returns early).
```

### Inbound sync frames (decode vectors)

```cpp
// [CAP-VERBATIM] -- CAN MSG: 0x3CF [8] <61:11:A3:A3:A3:A3:A3:A3>   (x791)
// data[2] is the panel's FILLER here, not 0x00 and never 0x01.
static const affa::Frame kPanelSyncRequest =            // -> reply kCarminatHello,
  { 0x3CF, 8, {0x61,0x11,0xA3,0xA3,0xA3,0xA3,0xA3,0xA3}, false };  // clear FAILED

// [LIVE] variant, same handling: the bench transcription reads data[2] = 0x00.
static const affa::Frame kPanelSyncRequestLive =
  { 0x3CF, 8, {0x61,0x11,0x00,0xA3,0xA3,0xA3,0xA3,0xA3}, false };

// [DERIVED] -- NEVER OBSERVED in any capture. Kept because [REF] affa3.c:98 tests for it.
static const affa::Frame kPanelSyncRequestColdStart =   // data[2]==0x01 also sets START,
  { 0x3CF, 8, {0x61,0x11,0x01,0xA3,0xA3,0xA3,0xA3,0xA3}, false };  // which arms 0xBA/0x7A

// [CAP-VERBATIM] -- CAN MSG: 0x3CF [8] <69:A3:A3:A3:A3:A3:A3:A3>   (x1157)
static const affa::Frame kPanelPeerAlive =              // -> set PEER_ALIVE, re-arm the
  { 0x3CF, 8, {0x69,0xA3,0xA3,0xA3,0xA3,0xA3,0xA3,0xA3}, false };  // 5000 ms deadline

// [OEM] -- short DLC is real; the decoder must not index past len.
static const affa::Frame kClusterPeerAliveDlc1 =
  { 0x3CF, 1, {0x69,0,0,0,0,0,0,0}, false };
static const affa::Frame kClusterSyncRequestDlc2 =      // data[1] = 0x23, NOT 0x11 (§1.1)
  { 0x3CF, 2, {0x61,0x23,0,0,0,0,0,0}, false };
```

### ACK frames the panel sends back

```cpp
// [CAP-VERBATIM] -- these two exact frames occur ~33000 and ~2700 times in the corpus.
// Only data[0] (DONE) and data[0..2] (PARTIAL) are inspected; the rest is the panel's
// 0xA3 filler and must be ignored by the matcher.
static const affa::Frame kAckPartial_151 =
  { 0x551, 8, {0x30,0x01,0x00,0xA3,0xA3,0xA3,0xA3,0xA3}, false };
static const affa::Frame kAckDone_151 =
  { 0x551, 8, {0x74,0xA3,0xA3,0xA3,0xA3,0xA3,0xA3,0xA3}, false };
static const affa::Frame kAckError_151 =                       // anything else -> ERROR
  { 0x551, 8, {0x7F,0x00,0x00,0xA3,0xA3,0xA3,0xA3,0xA3}, false };
```

---

## Appendix A — source citations

| Behaviour | File:line (MegaOpen) |
|---|---|
| ISO-TP chunking, `0x20+n`, filler, ACK wait | `src/display/AffaDisplayBase.cpp:12-120` |
| Lazy registration `0x70`, FUNCSREG latch | `src/display/AffaDisplayBase.cpp:122-169` |
| `KEY_HOLD_MASK`, `AffaKey`, `SyncStatus`, `FuncStatus` | `src/display/AffaCommonConstants.h` |
| Carminat identifiers, filler, `DisplayCtrl`, scroll enum | `src/display/Carminat/CarminatConstants.h` |
| Carminat sync tick (heartbeat / request / watchdog) | `src/display/Carminat/CarminatDisplay.cpp:209-270` |
| Carminat `recv` (sync, ACK match, auto-ACK, key decode) | `src/display/Carminat/CarminatDisplay.cpp:346-491` |
| Carminat `setText` | `src/display/Carminat/CarminatDisplay.cpp:547-588` |
| Carminat `showInfoMenu` (raw) | `src/display/Carminat/CarminatDisplay.cpp:618-651` |
| Carminat `setState` | `src/display/Carminat/CarminatDisplay.cpp:653-661` |
| Carminat `setTime` | `src/display/Carminat/CarminatDisplay.cpp:663-681` |
| Carminat `highlightItem` (raw) | `src/display/Carminat/CarminatDisplay.cpp:682-701` |
| Carminat `showMenu` | `src/display/Carminat/CarminatDisplay.cpp:705-766` |
| Carminat `showConfirmBoxWithOffsets` | `src/display/Carminat/CarminatDisplay.cpp:787-828` |
| Carminat `showFullscreenText` | `src/display/Carminat/CarminatDisplay.cpp:862-896` |
| Carminat `hideFullscreenText` / `hidePopup` (raw) | `src/display/Carminat/CarminatDisplay.cpp:898-903, 946-950` |
| Carminat `showPopupText` | `src/display/Carminat/CarminatDisplay.cpp:919-944` |
| Carminat func table `{0x151, 0x1F1}` | `src/display/Carminat/CarminatDisplay.h:139-145` |
| Menu window + scroll-indicator derivation | `src/display/Carminat/Menu/Menu.cpp:56-81, 154-164, 301-352` |
| AUX-mode inbound classifier | `src/display/Carminat/AuxModeTracker.cpp:9-101` |
| UpdateList identifiers, filler, `DisplayCtrl` | `src/display/UpdateList/UpdateListConstants.h` |
| UpdateList sync tick (unfixed counter watchdog) | `src/display/UpdateList/UpdateListBase.cpp:17-53` |
| UpdateList `setState` | `src/display/UpdateList/UpdateListBase.cpp:55-62` |
| UpdateList `recv` (sync, ACK match, radio text, keys) | `src/display/UpdateList/UpdateListBase.cpp:64-183` |
| UpdateList `setText` (segment) | `src/display/UpdateList/UpdateListBase.cpp:185-219` |
| UpdateList func table `{0x121, 0x1B1}` | `src/display/UpdateList/UpdateListBase.h:39-45` |
| UpdateList `setText` (LCD) | `src/display/UpdateList/UpdateListMenuDisplay.cpp:20-51` |
| `sendCan` helper (always DLC 8) | `src/utils/CanUtils.cpp:18-34` |
| ISO-TP reassembler (**8-bit length bug**, §3.7) | `src/affa/IsoTp.cpp:29-49` |
| Screen decoder offsets (independent statement of §8.5) | `src/affa/ScreenDecode.{h,cpp}` |
| Panel twin: auto-ACK, sync reply, key TX | `src/vdisplay/VirtualDisplayBase.cpp` |

### Capture evidence, by file

| File (`MeganeCAN/logs/`) | What it proves |
|---|---|
| `device-monitor-260616-230730.log` (3.6 MB) | §3.6 `showMenu` terminates at PCI `0x2C`; OEM head unit's `0x151` fullscreen + `02 54 03`; panel ACK filler `0xA3` |
| `device-monitor-260616-215143.log` | The 14-frame emulator `showMenu` (`[route] virtual`); the 3-frame `setText` ACK interleave |
| `device-monitor-260616-235529.log` | `setText("RENAULT")` verbatim, all three frames |
| `device-monitor-260617-004150.log` | `showPopupText` "VOL 28" verbatim with the `AFFA` PARTIAL/DONE trace |
| `device-monitor-250402-*.log` (9 files) | Panel sync frames `61 11 A3…` / `69 A3…`; the `0x7AF` self-ACK incident (§6.1) |
| `MeganeCAN/notes/notes1` | OEM cluster ↔ OEM radio, neither node ours (§1.1) |

## Appendix B — deliberate behaviour changes in the library

Everything else in this document is reproduced byte-for-byte. These nine are not:

1. **Transliteration on every string.** Legacy `setText` (both families) and the
   UpdateList LCD `setText` pass raw bytes through; the library transliterates on all of
   them. Rationale: the panel charset cannot render UTF-8 and a raw pass-through is a
   mojibake bug, not a feature.
2. **`delay(100)` after the sync request is gone.** No wire meaning (§5.2).
3. **`delay(5)` between `showInfoMenu` frames is gone.** No wire meaning (§8.10).
4. **The extra heartbeat emitted from the `69` handler is gone.** The 1 Hz cadence is
   paced internally against `IClock::millis()` (§5.4).

5. **The ISO-TP sequence counter wraps.** `0x20 | (num & 0x0F)` instead of `0x20 + num`.
   Byte-identical for every message in the repertoire (all are ≤ 16 frames); correct rather
   than corrupt beyond that, and matching the OEM head unit (§3.3).
6. **Self-sent frames are dropped before auto-ACK, ACK matching and key decode** (§6.1).
   On a real controller this changes nothing, because a real controller does not echo. On
   `LoopbackLink` it is the difference between a working test and a lie.
7. **`showInfoMenu` text is SPACE-padded to 8, not NUL-padded** (§8.10). The extracted
   builder's `char padded[8] = {' '}` initialises element 0 only and `strncpy` then
   NUL-pads the rest; the library emits the OEM space form. Only `t6`/`t7` of each row's
   continuation frame differ, and no capture witnesses a continuation frame either way.
   Pinned by `test_carminat_wire/test_showInfoPopup_is_three_messages_space_padded`.
8. **`showInfoMenu`'s default `infoPrefix` is `0x60`, not the legacy `0x70`** (§8.10). The
   legacy default was never exercised — its only caller, `showInfoPopup`, passed `0x60`
   explicitly, which is also the OEM capture — so no call path in the extracted code
   changes. A library caller who relies on the default now gets the OEM byte.
9. **Not yet done: the reassembler still matches `data[0] == 0x10` exactly.** §3.7 asks for
   `(data[0] & 0xF0) == 0x10` plus the 12-bit length so the OEM's 302-byte `0x1F1` message
   (`11 2E ..`) is not silently dropped; `proto/IsoTp.cpp` carries the ported behaviour
   unchanged, and `docs/API.md`'s `Reassembler` doc-block ("a frame whose `data[0]` is
   `0x10` starts a fresh message") describes what is implemented. **API.md is the arbiter,
   so the code is not in breach — this line is the open item.** RX-side only; nothing we
   transmit is affected either way.

Two further changes are structural rather than behavioural and are called out where they
occur: the four raw senders now go through the TX state machine (§8.4), which can prepend
registration frames on a cold link; and keys are decoded and delivered inside `poll()`
before the TX pump runs, instead of being queued for a later `processEvents()` — that is
what bounds key latency by the poll period alone.

## Appendix C — open questions

None of these blocks the port; all of them block a *confident* change to the bytes involved.

### Closed since the first draft

* ~~**`showMenu` declared `0x5A` vs 94 bytes transmitted.**~~ **Closed by [CAP]**: the panel
  ends the transfer at PCI `0x2C`, the last four bytes never leave, and item2's usable
  limit really is 26 characters (§3.6). This was the one place where the previously
  documented frame count was wrong.
* ~~**Panel behaviour past PCI `0x2F`.**~~ **Answered by [CAP]**: the OEM head unit wraps the
  counter modulo 16 on its 302-byte `0x1F1` message (§3.3). Legacy's non-wrapping
  `0x20 + num` is the defect; `0x20 | (num & 0x0F)` is correct and byte-identical below 16.
  `AFFA_MAX_PAYLOAD` still stays clamped at 113 until a long transmit is bench-validated.

### Still open

* **Held rotary detents (`0x01C1`).** The decode is specified (§7) but no capture contains
  the value, and no capture contains a panel-originated key frame *at all* (§7.1). The hold
  mask and the encoder exemption are DERIVED-ONLY on the RX side. Owner: library author,
  bench-verify before relying on hold semantics beyond `Open` / `Back`.
* **`setText` declared length `0x0E` vs 20 content bytes transmitted (§8.1).** All three
  frames do go out and the panel DONEs after the third (§3.6, capture-verified), so the
  surplus bytes reach the panel and are ignored. Whether they are discarded or latched into
  a scroll buffer has not been tested.
* **`0x1F1` (Carminat NAV) payload format.** Registered on every cold link, never written to
  by any renderer of ours. The OEM writes a 302-byte structured message beginning
  `21 0B 00 25 41 42 43 44 45 46 00 01 30 …`; the field layout is unknown. The `0x5F1` ACK
  channel behaves exactly like `0x551` (`30 01 00` / `74`), so the transport is settled even
  though the content is not.
* **`0x1C1` panel→radio payloads `02 64 0F` and `05 63 "0037"` (§7.1).** Observed on both
  our bench panel and the OEM cluster. Meaning unknown; the four ASCII digits vary
  (`"0037"`, `"0155"`). They are not keys and must be rejected as such.
* **`0x3CF` sync request with `data[1] == 0x23` (§1.1).** One OEM cluster sends `61 23`
  where our panel sends `61 11`. We would never answer it. Not a blocker for this panel;
  a known hole for any other.
* **`hideInfoPopup()`.** No dedicated close command is known; `setText("RENAULT")` is a
  workaround, not a specification (§8.10).
* **`showConfirmBox` caption/row overlap at content offset `0x20` (§8.9).** Reproduced as
  written. Whether a 7-character caption was ever intended is unknown.
* **`showConfirmBox` and `showFullscreenText` have no capture at all.** Both are
  hand-executed from the builder. `showFullscreenText`'s *header* (`10 60 21 05 FF 00 00
  40`) and 14-frame length are [CAP]-corroborated from the OEM head unit, but our own
  builder's output has never been observed on a wire. `showConfirmBox` is DERIVED-ONLY end
  to end, and it sits at exactly the 113-byte transport ceiling. Treat both vectors as the
  thinnest ice in this document.
