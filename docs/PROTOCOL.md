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

> **Superseded 2026-08-04.** Everything in this section was rewritten against four passive
> sniffs of a real OEM Renault radio talking to a real Carminat panel (579 frames, no ESP32
> on the bus) plus a bench session on glass the same day. The full derivation, with the
> per-capture timing tables, is `docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md`; that document is
> authoritative and this section is its summary. Four claims that used to live here are now
> disproven and are called out inline below, because a reader who remembers them needs to
> know they were tested rather than quietly dropped.

**Direction is carried by the padding byte, and this is proven rather than assumed.**
Semantically identical messages carry different filler depending on the id group — the same
`70` "open" appears as `1C1 70 A3 A3…` and as `151 70 00 00…`, the same `74` "ack" appears as
`551 74 A3…` and as `5C1 74 00…`. Filler is therefore a property of the transmitting node,
not of the message: **`0xA3` = the DISPLAY, `0x00` = the RADIO (us), 0/579 exceptions.**
`[OEM]` On a single-ended monitor `Dir=Rx` only means the monitor received the frame. The id
map is the primary key and the filler corroborates it: `3CF` and `1C1` are panel -> ESP32,
while `3AF`, `151`, `1F1` and `5C1` are ESP32 -> panel. Reading the filler is encouraged;
*validating* it is still forbidden (§1.1) — `0xA3` is a positive fingerprint of the display,
but `0x00` only means "not the display" and is indistinguishable from genuine zero data.

**The opening, in order. Every step below is measured 4/4 across the OEM captures.** `[OEM]`

```text
  (bus silent)
TX  3AF  B9 00 00 00 00 00 00 00     we announce ourselves — bounded, ONE pair
TX  3AF  BA 00 00 00 00 00 00 00     +0.3 .. 8.2 ms.  BA is the load-bearing frame.
RX  3CF  61 11 xx A3 A3 A3 A3 A3     panel request.  xx = 00 OR 01, same request.
                                     THIS ONE ONLY ARMS THE ANNOUNCE.
RX  3CF  61 11 xx A3 A3 A3 A3 A3     the panel's NEXT request, ~104 ms later, on its
                                     own free-running timer.  THIS is the trigger.
   +30.75 ms  TX 3AF  B0 14 11 00 1F 00 00 00     announce #1
      +0.8 .. 1.6 ms  RX 1C1  70 A3 A3 A3 A3 A3 A3 A3   panel opens ITS channel
         +0.25 .. 0.48 ms  TX 5C1  74 00 00 00 00 00 00 00   MANDATORY, unconditional
   +31 ms     TX 3AF  B0 14 11 00 1F 00 00 00     announce #2, byte-identical
   +31 ms     TX 3AF  B0 14 11 00 1F 00 00 00     announce #3, byte-identical
      +0.10 ms  TX 151  70 00 00 00 00 00 00 00   register function 1
      +0.29 ms  TX 1F1  70 00 00 00 00 00 00 00   register function 2 (PIPELINED)
      +0.48 ms  RX 551  74 A3 A3 A3 A3 A3 A3 A3
      +0.47 ms  RX 5F1  74 A3 A3 A3 A3 A3 A3 A3   <-- the 400 ms anchor
   +399.8 .. 400.5 ms  TX 151  03 52 09 00 00 00 00 00   display ON — ALWAYS first
```

Four consequences, each of which overturns something this document previously asserted:

1. **We speak first, into silence.** The old text — *"the ESP32 is silent after boot until a
   complete `3CF 61 11 xx` arrives"* — is wrong. In all four captures the radio's `B9`/`BA`
   pair precedes the request that triggers the burst; in the co-boot capture the panel's very
   first `61 11 00` arrives **7.24 ms after our `BA`**, answering it. `BA` is the precondition,
   not the response. It must be one bounded pair, repeated at most slowly (seconds apart) —
   **never a periodic BA stream, never a storm.**
2. **`61 11 00` and `61 11 01` are THE SAME REQUEST.** The old text — *"`61 11 01` is
   discovery only … wait for `61 11 00`"* — is disproven by
   `docs/captures/aknowledge offed display cONNECT OT POWER.csv`, which contains **sixteen
   `61 11 01` frames and zero `61 11 00`** and completes an entire session off `01` alone:
   registration, power command, ISO-TP text. The low bit is a state indication from the
   panel, not an authorization grade. Any code or comment that gates authorization on `00`
   is wrong. See §3.3.
3. **The burst answers the panel's NEXT request, not the first one, and it is timed from
   that request rather than from our `BA`.** Δ from the triggering `61 11 xx` to B0#1 across
   the four captures: 30.740, 31.527, 30.817, 30.751 ms — **spread 0.79 ms**. Δ from our last
   `BA` to B0#1 for the same four: 37.98, 31.63, 61.91, 111.78 ms — **spread 80 ms**. A 100×
   difference in spread names the anchor unambiguously. **Any implementation that times the
   burst off `BA` is wrong.** `[OEM]`
4. **Registration is part of the opening, not of the first render.** `151 70` and `1F1 70` go
   out 0.10–0.30 ms after B0#3, unconditionally, with no application involvement, and they
   are **pipelined** — `1F1 70` is on the wire 0.29 ms after `151 70`, before either has been
   acknowledged. See §3.5, which used to describe this as lazy and strictly sequential.

One opening frame is offered to `ICanLink` at a time; the `1C1 -> 5C1` control ACK is allowed
to interleave between B0 frames, and in the captures it always does — the panel's `1C1 70`
lands *between* B0#1 and B0#2, which is the entire reason the 31 ms staging exists. Wait
400 ms ± 0.5 ms from the final registration ACK (`5F1 74`, name the anchor — measured from
B0#3 the same interval reads 401.1–402.0 ms) before the zero-padded display-on frame
`151 03 52 09 00 00 00 00 00`. A clock request for 10:00 is `151 05 56 31 30 30 30 00 00`,
and it too requires `551 74`.

**Session loss.** A registered display never sends `61 11` at all — its disappearance after
B0#1 is the definitive signal that registration took, in all four OEM captures and in our own
successful bench run. Therefore **any complete `61 11 xx` arriving while we hold registrations
means the panel has voided us, and the third byte is irrelevant.** Tear the session down,
drop the registrations, stop application traffic, and re-open from the top. The failure this
protects against was measured on the bench: the panel sent `61 11 00` **41 times** while our
firmware kept pushing fullscreens at it. Recovery replays the opening above; it is never a
timer-driven BA probe.

Steady state is **two independent free-running timers**, not an exchange — see §3.6.

The historical `CarminatHelloProfile::MeganeCanLegacy70B0B0` profile is explicit
compatibility only. It preserves the old MeganeCAN source's immediate
`70 1A 11 ...`, `B0 ...`, `B0 ...` opening for a panel that has demonstrated that
requirement. It is not the capture-backed default, and the `70 1A 11` frame appears in
**zero** of the 579 OEM frames.

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
capture-backed rules are in §3.0: one bounded announce into silence, no periodic BA, and a
**free-running** 500 ms `B9` heartbeat that does not start until registration is complete.

> **What we transmit, and where the captures disagree with each other.** The OEM is not
> consistent here and the documentation must not pretend otherwise. Two captures show the
> radio's opening as `B9` then `BA` 0.29 ms apart (`on on display.csv` at 4685844,
> `cONNECT OT POWER.csv` at 147328246); the reattach in `offed display.csv` at 84945066 is a
> **bare `BA` with no `B9` in front of it**. Both readings fit a radio whose free-running
> 500 ms heartbeat simply happened to tick during the opening.
>
> **This library transmits `BA` alone**, unconditionally and for every family. `BA` is the
> question ("is anyone there?"); `B9` says "still here", which is meaningless before there is
> a session to be still in, and it is noise in the phase that can least afford it. Proven on
> glass 2026-08-04: handshake, registration, display-on and clock all complete with the
> announce as a bare `BA`.
>
> This was `SyncProfile::bootstrapAliveFrame` until the flag collapse of 2026-08-04. It is
> code now, in `AffaDisplayBase::pumpUnauthControl()`, and the two-stage sender that existed
> to resume a half-delivered pair went with it. Reproducing the other form means changing
> that function, deliberately, with the reason written down.

> **Retracted here 2026-08-04:** this paragraph used to end *"…one discovery `B9` + `BA` pair
> for `01` … and no application output before the later good `00`."* There is no "later good
> `00`" to wait for. `docs/captures/aknowledge offed display cONNECT OT POWER.csv` runs a
> complete session — announce, registration, power, ISO-TP text — on sixteen `61 11 **01**`
> frames and zero `61 11 00`. See §3.3.

> `requestArg` (`data[1]`) is `0x00` on Carminat **and it is filler, not an argument**.
> UpdateList's `7A 01` carries a genuine `0x01`. The two look symmetric on the wire and
> are not. `[REF][IMPL]`

### 3.3 What the panel sends, unprompted

```
3CF   61 11 xx A3 A3 A3 A3 A3      sync request, ~104 ms while unregistered
3CF   69 00 A3 A3 A3 A3 A3 A3      peer-alive ping, ~504 ms — an INDEPENDENT timer, §3.6
```

Match rules — **and the loose matching is deliberate**:

- Sync request: `data[0] == 0x61 && data[1] == 0x11`, with `len >= 3` before reading
  `data[2]`. Bytes 3..7 are filler.
- Peer-alive: `data[0] == 0x69` **only**. Bytes 1..7 are never examined, and DLC may be
  as low as 1. `[REF][IMPL][OEM]`

#### `data[2]` of `61 11` — SETTLED 2026-08-04: it is not an authorization grade

| value | meaning |
|---|---|
| `0x00` | the request |
| `0x01` | **the same request.** Low bit is the panel reporting its own state |
| any other `xx` | unobserved; treat as the request, the trigger does not read this byte |

> **RETRACTED — this table used to read:** *`0x00` = authorization request … **`0x01` =
> discovery only: one bounded `B9` + `BA`, no hello/register/output** … `0x01` means the
> panel is asking us to bootstrap its session, not that application traffic may resume.*
>
> **What overturned it.** `docs/captures/aknowledge offed display cONNECT OT POWER.csv`
> contains **sixteen `3CF 61 11 01` frames and zero `61 11 00`**, and completes a full
> session on `01` alone — B0 burst, `151`/`1F1` registration, `03 52 00` power command and a
> segmented text transfer. Its burst is triggered 30.751 ms after a `61 11 **01**`, which is
> inside the 30.740–31.527 ms band the three `00` captures produce. The trigger is
> byte-blind at offset 2. Believing otherwise cost a bench session: the panel emitted
> `61 11 01` for fifteen seconds and the library refused to answer it, waiting for a `00`
> that the panel had no reason to send. `[OEM][BENCH]`

**What the byte actually gates is nothing.** The gate is positional: a `61 11 xx` arriving
**after** we have transmitted `3AF BA` arms the announce, and the panel's *next* request
draws the B0 burst 31 ms later (§3.0). Before any `BA` has gone out, a `61 11 xx` produces
only the bounded announce — a bare `BA` as this library sends it — because there is nothing
else to say yet.

**A registered panel never sends `61 11` at all.** So the byte that matters is not `data[2]`
but *when the frame arrives*: a complete `61 11 xx` received while `FUNCSREG` is latched is
the panel telling us our registration is void, whatever the third byte says. Tear down, drop
registrations, stop application output, re-open. `[OEM 4/4][BENCH]`

**Read `data[2]` only after checking `len >= 3`** — a short `61 11` is not a request and must
be ignored rather than answered. Short DLCs are real on this channel; reading `data[2]` blind
reads uninitialised memory. `[REF][IMPL][CAP]`

> **A cluster can send `61 23` instead of `61 11`.** Matching `data[1] == 0x11` means such
> a peer is never answered. Known hole, not a bug — the Carminat panel sends `61 11`.
> `[OEM]`

### 3.4 Captured hello default and the historical compatibility profile

For the default Carminat profile, a `61 11 xx` that arrives **after our `BA`** emits
**three identical** `B0 14 11 00 1F 00 00 00` frames at approximately +31 ms, +62 ms, and
+93 ms from that request. Exactly three — never two, never four, `[OEM 4/4]`. The gaps are
protocol timing, not a blocking delay: only one B0 is offered at a time so the panel's
`1C1 -> 5C1` control exchange can occur between them, and in the captures the panel's
`1C1 70` always lands between B0#1 and B0#2.

> **Corrected 2026-08-04.** This paragraph used to say *"a good `61 11 **00**`"* and to end
> *"The default does not send either B0 or `70 1A 11` for `01`."* Both are disproven: the
> `cONNECT OT POWER` capture draws a textbook burst 30.751 ms after a `61 11 **01**`, and it
> is the only trigger in that entire capture because no `61 11 00` occurs in it. The `xx`
> byte is not read by the trigger (§3.3). Note also that the burst answers the panel's
> *second* request — the first one only arms the announce (§3.0).

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

Both profiles retain the same bounded recovery policy and differ only in the three opening
frames and their timing. Neither has a "good-`00` gate" any more; there is no such thing
(§3.3). `70 1A 11` appears in **zero** of the 579 OEM frames, so the legacy profile stays
behind its explicit opt-in.

### 3.5 Function registration (`FUNCSREG`) — part of the opening, and pipelined

`FUNCSREG` is a one-byte payload `{0x70}` on each registered function ID. It is unrelated
to the `70 1A 11 ...` sync frame used only by the optional legacy hello profile.

> **RETRACTED — this section used to open:** *"After the good-`00` opening has completed,
> registration is held behind the application gate. The library performs it before the queued
> application payload … A preceding `61 11 01` never starts registration."* On Carminat that
> is wrong twice over.
>
> **What overturned it.** In all four OEM captures the radio transmits `151 70` **0.10–0.30 ms
> after B0#3** and `1F1 70` **0.29 ms after that**, unconditionally, with no application
> involvement whatsoever — the first application payload does not appear until 400 ms later.
> Registration is a step of the *opening*, not of the first render. A library that registers
> lazily will sit looking registered and never complete a session if the application never
> draws. `[OEM 4/4]`

**Registration is triggered by the announce burst.** After B0#3 is accepted, both probes go
out back-to-back:

```
151   70 00 00 00 00 00 00 00      +0.10 ms after B0#3
1F1   70 00 00 00 00 00 00 00      +0.29 ms after 151 70 — SENT BEFORE 551 74 ARRIVES
551   74 A3 A3 A3 A3 A3 A3 A3      +0.48 ms
5F1   74 A3 A3 A3 A3 A3 A3 A3      +0.47 ms   <-- FUNCSREG latches here
```

**The two probes are pipelined, not serialised.** The OEM radio does not wait for `551 74`
before sending `1F1 70`; the four frames go out and come back in 0.9 ms total. Our `funcs[]`
is `{0x151, 0x1F1}` and that order is on the wire, but the *ordering* is a transmit order,
not a request/response chain.

**And the panel's own registration must come first.** The panel opens its control channel
with `1C1 70` between B0#1 and B0#2, and we must answer `5C1 74` within ~0.5 ms (§4 of the
ground-truth doc, 12/12) — *before* we register anything of our own, before any
authorization has completed. Our `151`/`1F1` probes follow ~61 ms later, after B0#3.

Only after **every** entry is acknowledged is `FUNCSREG` set. Then wait 400 ms ± 0.5 ms from
`5F1 74` before `151 03 52 09 00 00 00 00 00` (display ON).

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

#### `FUNCSREG` still latches on the ACKs, and on nothing else

`70` on the funcId → `74` on `funcId | 0x400`. Both probes may be in flight at once (see
above — the OEM radio pipelines them 0.29 ms apart), but `FUNCSREG` is latched on the ACKs
and on nothing else.

> **Amended 2026-08-04.** This heading used to read *"Registration is strictly sequential —
> one probe, one ACK, then the next"*. Serialising is functionally equivalent and safe, but
> it is not what the OEM radio does and it costs one extra round trip inside a
> latency-sensitive window. Do not treat the serialised form as the wire contract when
> comparing a bench capture against an OEM trace.

> **Registration still cannot be performed blind.** Putting the probes on the wire
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

#### Captured heartbeat cadence — `B9` IS NOT A PONG

> **RETRACTED — this subsection used to read:** *"The normal captured liveness pair is
> approximately `3CF 69 ...` and `3AF B9 ...` every 500 ms. **A B9 offered in reply to a
> received 69 is paced/coalesced** so retransmitted 69 frames cannot become a transmit
> storm."* Modelling `B9` as a reply to `69` is the single most damaging error available on
> this bus, and `SyncProfile::replyToPing` is `false` for Carminat as a direct result.

**They are two independent free-running timers.** There is no exchange. `[OEM]`

| stream | sender | period | jitter | must be answered? |
|---|---|---|---|---|
| `3AF B9 00 00 00 00 00 00 00` | radio (us) | **500.08 ms** | **σ = 0.33 ms** | no — free-runs |
| `3CF 69 00 A3 A3 A3 A3 A3 A3` | display | **507.83 ms** (bimodal 504/512 with the display ON) | **σ = 4.60 ms** | **no. Answer nothing.** |

**Three measurements, any one of which is sufficient:**

1. **A reply cannot be 14× more stable than its trigger.** σ 0.33 ms against σ 4.60 ms.
2. **The phase between them slides monotonically and wraps through zero.** Measured
   178 ms → 9 ms and then *past* zero to 511 ms, with neither cadence flinching; another
   capture shows a minimum phase of **0.023 ms**, the two frames virtually colliding, and
   both carry on unchanged. A reply cannot arrive 511 ms after its trigger and keep going.
3. **`B9` does not start at all until registration is complete** — it is a registered-session
   heartbeat, not a response to anything the panel sends before that.

**Consequence for any implementation:** one free-running 500 ms timer and nothing else. Code
that both paces at 500 ms *and* pongs each ~504 ms ping emits **two `B9` about 4 ms apart
every ~504 ms — double the OEM rate**, and a one-sided anti-double guard fails from the
second cycle onward.

The `~504 ms` figure for `69` is **refuted as a constant**: with the display ON it alternates
504/512 ms (histogram `{498:1, 502:1, 504:10, 505:3, 506:1, 510:2, 511:2, 512:10, 520:1}`).
Never build a timeout on a tight `69` period; use a generous liveness window of ≥ 3 missed
pings. No `B9` path is allowed to append a `BA`; `BA` belongs to the opening (§3.0) only.

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
- **The first continuation frame is `0x21`, not `0x20`.** `N` starts at 1, so the sequence
  number starts at 1 — exactly as ISO 15765-2 specifies, where SN 0 belongs to the first
  frame. Confirmed on the OEM bus: the 43-CF `0x1F1` transfer runs SN `1..F, 0..F, 0..B`.
  An implementation that emits `0x20` for the first CF is off by one for the whole transfer.
  `[OEM]`
- **The continuation counter does not wrap in our transport.** `0x20 + N` monotonic, so
  continuation 16 would emit `0x30` — which collides with the flow-control opcode. **Hard
  ceiling: 15 continuations = `8 + 15×7` = 113 payload bytes.** Real ISO-TP wraps
  `0x2F → 0x20 → 0x21`. (The OEM *does* wrap: `0x1F1` continuations run `…2E 2F 20 21…`. So
  an OEM message longer than 113 bytes is legal and we cannot send one. `[OEM]`)

### 4.2 The reply channel — `30 01 00` IS flow control, with BS = 1

After **every** frame, the receiver answers on `funcId | 0x400`:

| reply | meaning | sender does |
|---|---|---|
| `74 …` | **DONE** — the declared length is satisfied | stop and report success |
| `30 01 00 …` | **flow control**, FS = 0 CTS, **BS = 0x01**, **STmin = 0x00** | send exactly one more frame |
| anything else | error | abort |
| *(nothing, 2000 ms)* | timeout | abort |

> **CORRECTED 2026-08-04.** This subsection was headed *"`30 01 00` is NOT flow control"* and
> asserted *"There is no BlockSize, no STmin, no CTS/WAIT/OVFLW."* It is ordinary ISO 15765-2
> flow control and the fields decode exactly: `0x30` = FC with FS nibble 0 (ContinueToSend),
> `data[1] = 0x01` = BlockSize 1, `data[2] = 0x00` = STmin 0.
>
> **Why the old reading looked right for so long, and why it does not matter in practice:**
> **BS = 1 means one FC per single CF**, which is observationally identical to stop-and-wait.
> The OEM capture shows **43 flow-control frames for 43 consecutive frames** on the `0x1F1`
> transfer and 2 for 2 on the `0x151` transfer, in strict alternation CF→FC→CF→FC. So the
> existing one-CF-per-reply transmit behaviour is **correct and must be preserved** — a
> sender that reads FS/BS and then bursts will desynchronise the panel. What the old reading
> costs is robustness: a decoder that constant-matches `30 01 00` fails an FC with any other
> BS or STmin (`30 00 14`, say) and kills the transfer instead of adapting. Match
> `(data[0] & 0xF0) == 0x30` and switch on the FS nibble — `0` CTS, `1` WAIT, `2` OVERFLOW.
> `[OEM]`

> **`STmin = 0` is not the pacing constraint.** Real CF→CF gaps measured 0.645–11.077 ms
> (mean 2.399); FC round-trip latency mean 1.533 ms, max 10.611 ms. The rate is set by the FC
> round trip plus sender think-time, three orders of magnitude above the 0 ms floor.
> Throughput ≈ 2.9 kB/s. `[OEM]`

> **FC state machines are per-ID-pair, not global.** In the capture the `0x1F1` first frame
> went out while the `0x151` transfer was still mid-flight. Transfers interleave. `[OEM]`

> **We never send FC in the current configuration.** All display-originated traffic on
> `0x1C1` is single-frame across the whole corpus. If the panel ever segments on `0x1C1` we
> must answer `5C1 30 01 00 00 00 00 00`. `[OEM]`

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
| our `B9` heartbeat, free-running, starts only after registration | **500.08 ms, σ 0.33 ms** `[OEM]` |
| panel `69`, free-running, unrelated timer (§3.6) | **507.83 ms, σ 4.60 ms** — bimodal 504/512 with the glass on `[OEM]` |
| panel `61 11 xx` re-issue rate while unregistered | **103.985 ms** (min 103.838, max 104.230, n=15) `[OEM]` |
| panel `1C1 70` re-issue rate while unacknowledged | **~610 ms** `[BENCH]` |
| `61 11 xx` → B0#1 | **30.74–31.53 ms**, anchored on the request `[OEM 4/4]` |
| B0 → B0 | **30.89–31.21 ms** `[OEM]` |
| `1C1` → our `5C1 74` | **0.249–0.483 ms**, mean 0.36, 12/12 `[OEM]` |
| final registration ACK (`5F1 74`) → first application payload | **400 ms ± 0.5 ms** `[OEM 4/4]` |
| peer-alive timeout | 5 ticks ≈ 5 s — but see §3.6, never build it on a tight `69` period |
| per-frame ACK timeout | 2000 ms |
| retry policy | **none** — no retransmission at any layer |
| panel sync-request rate when **unacknowledged at the CAN link layer** | **line rate** — 1472 frames/s of controller retransmit; this is §8.1, not a protocol rate |

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
Either side can decide the other's registration is void — a `61 11 xx` arriving while we hold
registrations is the panel doing exactly that — and neither side re-checks it spontaneously.

**The third byte plays no part in that signal.** Corrected 2026-08-04: this paragraph used to
name `61 11 01` specifically. A registered panel sends **no** `61 11` at all, `00` or `01`, in
any of the four OEM captures; the *arrival* of a complete request is the void indication and
the payload byte is irrelevant (§3.3). On the bench the panel sent `61 11 **00**` forty-one
times while our firmware, believing only `01` meant loss, kept pushing fullscreens at a panel
that had already dropped us. `[BENCH]`

---

## 9. Traps

Ranked by how much time each has cost.

1. **Offset origin.** Reassembly keeps the 2-byte header, so decode offsets are content+2.
   Three origins circulate in the source material.
2. **`0x74` means *stop*, not *complete*.** (The companion claim that *"`30 01 00` is a
   per-frame ACK, not flow control"* is **withdrawn** — it is real ISO-TP flow control with
   BS = 1, which merely *behaves* like a per-frame ACK. See §4.2. The trap that remains is
   constant-matching all three bytes instead of parsing the FS nibble.)
3. **Declared lengths are wrong in two commands and must stay wrong** (`setText` `0x0E`,
   `showMenu` `0x5A`).
4. **A standard ISO-TP stack cannot be used.** It inserts its own PCI on frame 0 and
   shifts every payload byte by two.
5. **Registration is all-or-nothing**, and a function id the panel does not ACK stalls every
   render for ever. (It is **not lazy on Carminat** — that word was removed 2026-08-04. The
   OEM radio registers 0.1–0.3 ms after B0#3 with no application involvement; deferring the
   probes to the first render means an idle build never finishes a session. See §3.5.)
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

- The `0x55` byte at text payload `[4]` remains an unexplained constant. The Carminat power
  form is **zero-padded, settled**: `03 52 09 00 00 00 00 00`, in all four OEM captures. The
  MeganeCAN `03 52 09 FF FF 00 00 00` spelling is **not** what the OEM radio puts on this
  bus; it is historical compatibility evidence, never the default wire form.
- Icon byte values do not decode cleanly against the `[REF]` bitmap: `0x94` shows **no**
  icon and `0x9B` shows traffic, neither of which the bitmap predicts. Codes appear to
  repeat cyclically across `0x00..0xFF`. Not fully decoded.
- What makes the panel choose `data[2] == 0x01` over `0x00` is **still not known** — but it
  no longer matters to us, and the old entry here (*"the strict library policy treats it as
  discovery only and waits for `00`"*) is withdrawn. `01` is a full request (§3.3), so the
  open question is narrowed to: what panel-side state does the low bit report? All that is
  known is the correlation with the capture filenames — the `01`-only capture is the
  `cONNECT OT POWER` one — and one sample is not a decode.
- **Whether an unanswered `61 11 xx` ever times out on the panel side is unknown.** No
  capture contains a panel giving up; it simply repeats at ~104 ms. Likewise, the rule that a
  post-registration `61 11 xx` means session loss is inferred from the panel's permanent
  silence once registered plus one bench observation, not from an OEM capture of a teardown.
- The confirm box has **no capture at all**. Every byte of §5.3's confirm layout is
  `[DERIVED]`.
- `0x1F1` NAV: we register it and never write it. Its 302-byte OEM payload is a field
  structure, not a bitmap, and is undecoded.
- The cluster profile was transcribed from a single OEM capture and has never been put on
  a bus.
