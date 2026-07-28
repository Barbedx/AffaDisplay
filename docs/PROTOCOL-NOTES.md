# AFFA2 / AFFA3 protocol notes — what we actually know, and how we know it

`WIRE-SPEC.md` is the normative byte specification: it tells you what to emit. This
document is the **provenance and reconciliation** record: where each of those bytes came
from, which of them already had a name before we open-coded them as hex, which of them
are still guesses, and what the raw captures show. It exists so the library can claim to
*know* the protocol rather than to have inherited a working pile of magic numbers.

Read this before changing any constant. Read `WIRE-SPEC.md` before emitting one.

---

## 0. Witnesses

Every claim below cites one or more of these. Nothing is asserted without a witness.

| Tag | Witness | Nature |
|---|---|---|
| **W1** | Live 2-node capture, ESP32-C3 + real Carminat panel, 500 kbit/s, 2026-07-26 | Direct observation of *our* implementation against the real panel |
| **W2** | `MeganeCAN/notes/notes1`, "Dashboard log" section | OEM bus log from a real car: factory radio + instrument cluster, third-party annotations (Russian) |
| **W3** | `MeganeCAN/notes/notes1`, serial trace section | Our firmware's own `[RX]` / `[do_set_text]` serial output against a panel |
| **W4** | `MeganeCAN/notes/AFFA3_SCREENS.md` | Screens reverse-engineered from OEM car captures (`logs affa3 new`) decoded with `tools/affa_decode.py` |
| **W5** | `MeganeCAN/notes/AFFA3_ICONS.md` | Icon byte sweep driven from the `/preview` page against a real panel |
| **W6** | `MeganeCAN/notes/archive_mhroczny/affa3.{c,h}` | Third-party AFFA3 reference implementation (AVR + MCP2515). The **original names** for most of what we open-code |
| **W7** | MeganeCAN sources: `src/display/**`, `src/affa/**`, `src/vdisplay/**` | Our encoders and decoders — the definition of "what the working system emits" |
| **W8** | `MeganeCAN/notes/CARMINAT_DECOMPOSITION.md` | Which concerns are protocol and which are application. Drove the library/app boundary |
| **W9** | `MeganeCAN/notes/device-monitor-250826-*.log` (10 files) | Serial console traces. Contain **no wire bytes** — see §5.5. Witness for one behavioural fact only |
| **W10** | `MeganeCAN/notes/ARCHITECTURE-V2.md` §8.0, §8.2, §8.5, §8.6 | Research annex: bus topology, an independently-sourced key table, third-party corroboration of the UpdateList ids, and the origin of two of this library's scheduling rules — see §5.6 |

Two witness classes must not be confused. **W2/W4/W5** observe the *OEM* talking to the
*OEM panel* — they are protocol truth. **W1/W3/W7** observe *our* implementation talking
to a real panel — they prove a byte sequence is *accepted*, which is weaker. Where the two
disagree (they do, see §3.3 and §6) the disagreement is recorded, not resolved by
guesswork.

---

## 1. The layering nobody wrote down

The single most useful thing recovered from this material is that the AFFA payload is
**two independent layers stacked in one byte stream**, and almost every "magic number" in
the legacy code is one or the other:

```
  byte 0        : ISO-TP PCI          (transport)
  byte 1        : length, or command  (depends on PCI)
  byte 2..      : command operands
```

The PCI nibble follows ISO 15765-2 exactly:

| `data[0]` | Meaning | What follows |
|---|---|---|
| `0x0N` | **Single frame**, N content bytes | `data[1]` is the *command*, `data[2..]` its operands |
| `0x10` | **First frame** of a multi-frame message | `data[1]` is the declared content length, `data[2]` is the *command* |
| `0x2N` | **Consecutive frame**, sequence N (1..15) | 7 payload bytes |

*(W6 `affa3_do_send`: `if (num > 0) packet.data[i++] = 0x20 + num;` — the first frame is
emitted with no PCI added because the caller pre-built `10 <len>`. W7 `IsoTp.h` states the
same rule. W1/W3 confirm on the wire.)*

Two consequences that the legacy code obscured and the library must not:

* `02 54 03`, `03 52 09 FF FF`, `05 56 31 32 33 34`, `07 29 01 7E 80 00 00 00` are **not**
  four unrelated magic frames. They are single frames of length 2, 3, 5 and 7 carrying the
  commands `0x54`, `0x52`, `0x56`, `0x29`. Once you see that, `setState`'s leading `0x03`
  and `setTime`'s leading `0x05` stop looking like part of the command and start looking
  like what they are: a byte count. The archive (W6) sends `04 52 <state> FF FF` where we
  send `03 52 <state> FF FF` — a length disagreement, not a command disagreement, and the
  OEM (W2) disagrees with both. Audited in full in §2.4.
* The transport is **ISO-TP-shaped but not ISO-TP**. There is no flow-control frame; the
  peer acknowledges *every* frame individually with `74` (DONE) or `30 01 00` (PARTIAL) on
  `id | 0x400`. `30 01 00` is a flow-control frame's byte pattern (CTS, BS=1, STmin=0)
  repurposed as a per-frame ACK. This is why the legacy sender blocked for up to 2 s per
  frame, and why the library models transmission as a state machine.

### 1.1 Byte 0 is not always a PCI — the two frames that opt out

The rule above holds for every *content* message and nowhere else. Two frame classes carry
a command in `data[0]` with no transport layer at all, and reading them as PCI produces
nonsense (`0x70` and `0x74` are not defined nibbles in ISO 15765-2):

* **Function registration.** W6 `affa3_send` calls `affa3_do_send(idx, regdata, 1)` with
  `regdata[1] = {0x70}`. Because `num == 0`, `affa3_do_send` prepends no PCI — the frame
  on the wire is literally `70` followed by seven filler bytes. W1's `RX 1C1 70 A3 A3 …`
  and W2's `121 8 70 FF FF …` are exactly that shape.
* **ACKs on `id | 0x400`.** `74 …` and `30 01 00 …` likewise.

Consequence for the library: the ISO-TP reassembler must be fed only the content ids, and
the registration/ACK classifier must run *before* any PCI dispatch. A reassembler that sees
a registration frame first will try to interpret `0x70` as a PCI and desynchronise.

### 1.2 Three offset origins — the trap that produces off-by-two bugs

The material describes screen layouts against **three different origins**, never states
which, and mixes them within a single file. This is the single most likely source of a
silent porting error, so it is fixed here once:

| Origin | Byte 0 is | Used by |
|---|---|---|
| **Payload** | the `0x10` PCI | W7 `showMenu` builder, `ScreenDecode::OFF_*` (its header says "includes the 0x10 0x5A header") |
| **Content** | the screen command (`0x21`, `0x74`, …) | W4 `AFFA3_SCREENS.md`, the declared length byte, W7 `showFullscreenText` comments |
| **Tail** | the first byte *after* a fixed header | W7 `showConfirmBoxWithOffsets` (its 105-byte `content[]` starts after the 6-byte header) |

`payload = content + 2`, and for the confirm box `content = tail + 6`, so `payload = tail + 8`.

Restated in **content** offsets, which is the origin this document and `WIRE-SPEC.md` use
throughout:

| Field | Content offset | Payload offset | Source's own origin |
|---|---|---|---|
| menu scroll-arrow byte | **8** | 10 | payload (`OFF_SCROLL = 10`) |
| menu header (26 B) | 9..34 | 11..36 | payload |
| menu item0 marker `0x7E` | 36 | 38 | payload |
| menu item0 text | 37..61 | 39..63 | payload |
| menu item1 marker `0x7F` | 63 | 65 | payload |
| menu item1 text | 64..93 | 66..95 | payload |
| fullscreen two leading spaces | 32..33 | 34..35 | content |
| fullscreen text block | 34..95 | 36..97 | content |
| confirm-box caption (7 B) | 32..38 | 34..40 | tail (`0x1A`) |
| confirm-box rows | 38..60 | 40..62 | tail (`0x20`..`0x36`) |

Note what this exposes: the confirm box's caption at tail `0x1A` and its first row at tail
`0x20` are **six bytes apart, for a field documented as seven characters wide**. The source's
own comment ("caption max 7 chars", rows "from 0x20") is self-overlapping by one byte. That
is §6 item 15's real content, and it is only visible once the origins are reconciled.

---

## 2. The AFFA3 command map

Commands are grouped by the PCI that carries them. "Witness" is the strongest evidence for
that command's existence and shape.

### 2.1 Single-frame commands

| Cmd | Full frame | Operands | Effect | Witness |
|---|---|---|---|---|
| `0x29` | `07 29 01 <row> 80 00 00 00` | `row` = `0x7E` (item 0) / `0x7F` (item 1); the `01` and `80` are fixed and unexplained | Move the highlight bar to a menu row | W7 `CarminatDisplay::highlightItem`; W7 `ScreenDecode::frame` decodes it; W1 |
| `0x52` | `03 52 <state> FF FF` | `state`: Carminat `0x09` enable / `0x00` disable; UpdateList `0x02` / `0x00` | Display control (panel on/off) | W6 `affa3_display_ctrl` (as `04 52 …`); W7 both `setState` |
| `0x54` | `02 54 03` | `03` fixed, meaning unknown | Close the full window / dismiss a popup | W4 ("Close full window / popup"); W7 `hidePopup`, `hideFullscreenText` |
| `0x56` | `05 56 <h> <h> <m> <m> 00 00` | four ASCII digits `HHMM` | Set the clock | W7 `CarminatDisplay::setTime` (written as `'V'` — `'V'` is `0x56`) |

`0x56` deserves a note: the legacy source writes `answer.data.uint8[1] = 'V'; // likely
constant`. It is not a letter. It is command `0x56` in a family whose other members are
`0x52`, `0x54`, `0x29` — the "looks like ASCII" reading is an accident of the encoding and
it is exactly the kind of thing that stops a maintainer from recognising the pattern.
The library names it `kCmdSetClock`.

### 2.2 Multi-frame (first-frame) commands

| Cmd | First frame | Declared len | Content layout | Effect | Witness |
|---|---|---|---|---|---|
| `0x21` mode `0x01` | `10 5A 21 01 7E 80 00 00` | `0x5A` | **payload** offsets: `[8]=82 [9]=FF [10]=scroll [11..36]=header(26) [37]=00 [38]=7E [39..63]=item0 [64]=01 [65]=7F [66..95]=item1` (§1.2 — subtract 2 for content offsets) | Windowed menu / now-playing box | W4; W7 `showMenu` + `ScreenDecode::menu` offsets; W1 |
| `0x21` mode `0x05` | `10 60 21 05 FF 00 00 40` | `0x60` | `[6..31]=00`, `[32..33]=20 20`, `[34..95]` text, lines separated by `0x0D` | Fullscreen big text ("Please insert navigation CD") | W4 (captured payload quoted verbatim); W7 `showFullscreenText` |
| `0x21` mode `0x05` (confirm box) | `10 6F 21 05 00 00 01 49` | `0x6F` | 6-byte header + 105 bytes; **tail** offsets: `[0x1A..0x20]` caption (7), `[0x20..0x36]` row1 `0D` row2 `0D` (§1.2 — add 6 for content offsets) | Caption-button confirm box | W7 `showConfirmBoxWithOffsets` only |
| `0x74` | `10 <6+n> 74 <icon> 55 <src> <fmt> 01` + text | `6+n` | 6-byte header + `n` text cells | Full-window text / popup overlay | W4 (`74 09 55 FF 60 01` "VOL 9"); W5; W7 `showPopupText`; W1 |
| `0x76` | `10 0B 76 <prefix> <row> <c0> <c1> <c2>` then `21 <c3..c7> 00 00` | `0x0B` | prefix + rowcode + 8 chars | One row of the info/settings list | W4 (`76 60 41` "AUX ON", `76 60 44` "AF ON", `76 60 48` "SPEED 0"); W7 `showInfoMenu` |
| `0x77` | `10 0E 77 55 55 FF 60 01` + text | `0x0E` | same 6-byte header as `0x74` | Not-full (windowed) radio text | W4; W7 `CarminatDisplay::setText`; W1 |
| `0x76` (AFFA2 form) | `10 19 76 <fmt> <loc>` + `old(8)` + `10` + `new(12)` + `00` | `0x19` = 25 | text-only variant | UpdateList segment/LCD text | W6 `affa3_do_set_text` "Sam tekst"; W3 (`[do_set_text] ID: 0x121, len=27`); W7 `UpdateListBase::setText` |
| `0x7F` (AFFA2 form) | `10 1C 7F <icons> 55 <iconmode> <fmt> <loc>` + `old(8)` + `10` + `new(12)` + `00` | `0x1C` = 28 | text **plus** icons | UpdateList text with icon update | W6 `affa3_do_set_text` "Tekst + ikony" |
| `0x7E` | `10 19 7E <fmt> <loc>` + … | `0x19` | as `0x76` | Observed variant, effect not catalogued | W3 (`mtx 7E 69 01`, `10 19 7E 70 01 …`) |

The `0x21` command has two distinct roles depending on context and this trips people up:
as the **second byte of a first frame** it is the screen command; as the **first byte of a
frame** it is ISO-TP consecutive-frame #1. `showInfoMenu` emits both in two consecutive
frames (`10 0B 76 …` then `21 …`), which reads like a coincidence and is not one.

**Mode byte = window vs fullscreen** (W4, and it is the single best insight in the notes):
`0x21`'s second operand selects the layout. `0x01` = windowed (the panel keeps the radio
context and later overlays replace the box); `0x05` = fullscreen (the panel takes the whole
screen, and the radio then renders volume/settings as `0x74` popups *over* it, dismissing
them with `54 03`). That is the same distinction the `setText` docblock records as
"`0x74` = full window, `0x77` = not full" — one mechanism, two commands, described twice
in two places with no cross-reference. Recorded here once.

**`0x76` versus `0x7F` is a differential encoding, and it is stateful.** This is not
recorded anywhere in the notes and it is the reason two commands exist for one screen.
W6 `affa3_do_set_text`:

```c
static uint8_t old_icons = 0xFF;
static uint8_t old_mode  = 0x00;
...
if ((icons != old_icons) || (mode != old_mode)) {
    data[len++] = 0x1C; data[len++] = 0x7F;                       /* "Tekst + ikony" */
    data[len++] = icons; data[len++] = 0x55; data[len++] = mode;
} else {
    data[len++] = 0x19; data[len++] = 0x76;                       /* "Sam tekst"     */
}
```

So `0x7F` (declared length `0x1C` = 28) is *text plus a three-byte icon header*, and `0x76`
(declared `0x19` = 25) is *text only, icons unchanged* — the panel latches the icon state
and the sender omits it when it has not moved. Three consequences the library must respect:

* **The panel holds icon state across messages.** A `0x76` does not clear the icons; it
  leaves whatever the last `0x7F` set.
* **The cached `old_icons`/`old_mode` are function-local `static`s**, i.e. process-global
  and never invalidated. After a resync the panel's actual icon state is unknown while the
  cache still claims to know it, so the first text after a resync can be a `0x76` that
  leaves stale icons on the glass. The library must force a `0x7F` on every
  `SyncChanged -> synced` transition and keep the cache per-instance, never `static`.
  (Note the archive's initialiser is `old_icons = 0xFF`, a value no caller ever passes,
  which is what forces the *first* message to be a `0x7F`. Reproduce that intent
  explicitly rather than by picking an impossible sentinel.)
* `UpdateListBase::setText` (W7) hardcodes `textType = 0x76` and never emits `0x7F` at all,
  so **our UpdateList driver cannot set icons** — not a bug we introduced, a capability we
  never ported. Recorded in §6 rather than silently added.

### 2.3 Function-channel commands (not screen commands)

These share the `0x7x` range with the text commands but live in a different namespace —
they are addressed to the *function registration* machinery, not to the screen.

| Byte | Direction | Meaning | Witness |
|---|---|---|---|
| `0x70` | either | Register this function id / announce | W6 `regdata[1] = {0x70}`; W2 (`121 8 70 FF …`, `1B1 8 70 FF …`, `1C1 8 70 84 …`); W1 (`RX 1C1 70 A3 …`) |
| `0x74` | reply on `id \| 0x400` | **DONE** — message complete | W6; W2 (`521 74 84 …`, `5B1 74 84 …`); W1 (`TX 5C1 74 00 …`) |
| `0x30 0x01 0x00` | reply on `id \| 0x400` | **PARTIAL** — send the next frame | W6; W7 both `recv()` |
| anything else | reply on `id \| 0x400` | **ERROR** — abort the message | W6; W7 |

**Nobody has ever implemented the sending side of PARTIAL.** W6's `affa3_recv` builds its
auto-ACK from a local `uint8_t last = 1;` that is assigned once at declaration and never
touched, so the `30 01 00` branch below it is unreachable dead code:

```c
uint8_t i, last = 1;
...
if (last) { reply.data[i++] = 0x74; }
else      { reply.data[i++] = 0x30; reply.data[i++] = 0x01; reply.data[i++] = 0x00; }
```

The reference therefore ACKs **every** frame it receives — including consecutive frames
mid-message, and including frames on ids it does not own — with an unconditional DONE. We
know PARTIAL exists only because a real panel *sends* it to us. This matters twice:

* **Anything emulating a panel** has no reference behaviour to copy for deciding when to
  answer PARTIAL versus DONE. It must derive it from the transport: PARTIAL while the
  declared content length has not been reached, DONE on the frame that completes it. That
  rule is inferred from our own receiver, not observed from a sender. The library's own
  `setSelfAck()` implements exactly it; the deleted `vpanel/` twins called the same rule
  `AckMode::Declared`.
* An auto-ACK that answers everything with DONE would tell a peer "message complete" halfway
  through its multi-frame message. The library's ACK responder must be scoped to the ids it
  actually owns, which the reference's was not.

### 2.4 The declared-length audit — and what it says about provenance

Every builder in W7 was measured against its own PCI. The result is not random, and the
pattern is the useful part:

| Family | Builder | Declares | Actually supplies | Consistent? |
|---|---|---|---|---|
| Carminat | `highlightItem` | SF `0x07` | 7 (`29 01 <row> 80 00 00 00`) | yes |
| Carminat | `setTime` | SF `0x05` | 5 (`56 <h><h><m><m>`), padded to DLC 8 with `00 00` | yes |
| Carminat | `setState` | SF `0x03` | **4** (`52 <state> FF FF`) | **no — one short** |
| Carminat | `showInfoMenu` | FF `0x0B` = 11 | 11 (`76 <prefix> <row>` + 8 chars) | yes |
| Carminat | `showFullscreenText` | FF `0x60` = 96 | 96 (6 header + 90) | yes |
| Carminat | `showConfirmBoxWithOffsets` | FF `0x6F` = 111 | 111 (6 header + 105) | yes |
| Carminat | `showPopupText` | FF `6 + textLen` | `6 + textLen` | yes (computed) |
| Carminat | `showMenu` | FF `0x5A` = 90 | **94** | **no — four over** |
| Carminat | `setText` | FF `0x0E` = 14 | **20** (6 header + 14 text) | **no — six over** |
| UpdateList | `setState` | SF `0x04` | 4 (`52 <state> FF FF`) | yes |
| UpdateList | `setText` | FF `0x19` = 25 | 25 (`76 <chan> <loc>` + old 8 + `10` + new 12 + `00`) | yes |
| archive (W6) | `affa3_do_set_text` | FF `0x19` / `0x1C` | 25 / 28 | yes |

**Every inconsistency in the entire corpus is Carminat, and only in the three builders that
were written by hand against a panel until the glass looked right.** Everything transcribed
from an OEM capture (`showFullscreenText`, `showInfoMenu`, `showPopupText`), everything
carried over from the archive's own arithmetic (`highlightItem`, `setTime`,
`showConfirmBoxWithOffsets`), and the *entire* UpdateList family are self-consistent. That
correlation is the strongest available evidence that the declared length *does* mean
"content byte count" as W4 states, and that our three outliers are tolerated by a permissive
panel rather than being an undocumented second convention.

It also predicts what is actually being lost, which is testable on the glass rather than in
a decoder: `showMenu` would truncate item 1 at 26 of its 30 characters, and `setText` would
show 8 of its 14 — the latter matching the source's own "max 7 characters shown" docblock
almost exactly. See §6 items 8 and 9 before changing any of these three bytes.

`setState` is the interesting one, because here the OEM disagrees with us *and* the archive
disagrees with both — and because **our own two families disagree with each other over a
byte they both copied from the same source.** `UpdateListBase::setState` emits
`04 52 <state> FF FF`, character-for-character the archive's array;
`CarminatDisplay::setState` emits `03 52 <state> FF FF`, the same array with the first byte
changed to `0x3` and a comment recording the serial command it was tuned with
(`// sc 151 3 52 9 0 0 0 0 0`). One family was ported, the other was hand-tuned on the
glass; the divergence is a transcription event, not a protocol difference between panels.
So: W6 sends `04 52 <state> FF FF` (4 content bytes, self-consistent);
we send both forms depending on family; the OEM radio sends
`1B1 8 03 52 00 00 FF FF FF FF` (W2) — declaring 3 and supplying exactly 3, i.e.
**`52 <state> 00`, with `FF` as pure filler rather than as operands**. The OEM form is the
only one that is both self-consistent and observed on a factory bus. Our trailing `FF FF`
is probably filler that got mistaken for payload and then had the length byte trimmed to
compensate. Reproduce **each family's own bytes verbatim** — `0x03` for Carminat, `0x04`
for UpdateList — because those are what each panel has been accepting; record the OEM form
here as the likely correct one. Do not unify them into one constant "for consistency": that
would change the Carminat wire on the only panel we can test against, to fix nothing.

### 2.5 Which builders go through the transport — and which quietly do not

Four Carminat builders never touch `affa3_send`. They call `CanUtils::sendCan` /
`sendFrame` directly, which means **no sync gate, no function registration, no per-frame
ACK, no retry, and no error return** — the frame is pushed at the driver and forgotten:

| Builder | Path | What it loses |
|---|---|---|
| `highlightItem` | `CanUtils::sendFrame` | fire-and-forget; the highlight can be emitted while unsynced |
| `showInfoMenu` (free function) | two `CanUtils::sendCan` calls with `delay(5)` between them | ACK; the inter-frame gap is a blocking sleep instead of the peer's PARTIAL |
| `hideFullscreenText` | `CanUtils::sendCan` | ACK |
| `hidePopup` | `CanUtils::sendCan` | ACK |

In the library all four go through the same `enqueue()` state machine as everything else.
That is the right design and it is also a **behaviour change**, which is why it is recorded
here rather than in a commit message:

* They become **sync-gated**. A `highlightItem` issued before FUNCSREG latches now returns
  `Result::NoSync` instead of silently going out. Legacy code that called it optimistically
  will start seeing a failure result it never saw before.
* `showInfoMenu`'s `delay(5)` disappears. The second frame is now sent when the panel
  acknowledges the first, which is both faster and correct — but it means the two frames of
  one info row are no longer guaranteed to be 5 ms apart, and they are no longer guaranteed
  to be adjacent on the bus at all if an Urgent message is queued between rows. Info rows
  must therefore be enqueued as **one message per row**, never as three independent sends
  that assume ordering (§7).
* `showInfoMenu` also has a padding defect worth fixing in the port rather than
  reproducing: `char padded[8] = {' '};` initialises only element 0 to a space and the
  remaining seven to `0x00`, and `strncpy(padded, text, 8)` then NUL-pads anything shorter
  than 8. So short info rows go out **NUL-padded**, where the OEM capture (W4) shows them
  **space-padded** (`"AUX  ON "`). The panel evidently tolerates both; the OEM form is the
  one to emit.

**The popup is a non-destructive overlay** — observed directly on the real panel and
recorded in the `showPopupText` docblock (W7): while a `0x74` popup is up, the screen
underneath can keep being redrawn (it visibly blinks *under* the popup) and other commands
still apply; the popup stays until it auto-reverts or `02 54 03` closes it. This is a panel
fact with a direct design consequence: `RenderSlot::Popup` and `RenderSlot::Text` are
**independent** slots. A popup must not coalesce with, invalidate or be invalidated by the
screen behind it, and closing a popup must not trigger a redraw of that screen.

---

## 3. Icon and format bytes, reconciled

This is where the notes and the code were furthest apart. `AFFA3_ICONS.md` (W5) catalogues
a byte by trial; `affa3.h` (W6) *names its bits*; our encoders (W7) hardcode particular
values with comments like `// unknown/fixed`. They are the same byte.

### 3.1 The icon bitmap — `0x55` and `0x45` are not magic

`affa3.h` defines:

```c
#define AFFA3_ICON_NO_NEWS        (1 << 0)   /* 0x01 */
#define AFFA3_ICON_NEWS_ARROW     (1 << 1)   /* 0x02 */
#define AFFA3_ICON_NO_TRAFFIC     (1 << 2)   /* 0x04 */
#define AFFA3_ICON_TRAFFIC_ARROW  (1 << 3)   /* 0x08 */
#define AFFA3_ICON_NO_AFRDS       (1 << 4)   /* 0x10 */
#define AFFA3_ICON_AFRDS_ARROW    (1 << 5)   /* 0x20 */
#define AFFA3_ICON_NO_MODE        (1 << 6)   /* 0x40 */
```

`CarminatDisplay::setText` hardcodes `rdsIcon = 0x55` and documents `0x45 = AF-RDS icon,
0x55 = no icon`. Evaluate:

```
0x55 = 0x01 | 0x04 | 0x10 | 0x40 = NO_NEWS | NO_TRAFFIC | NO_AFRDS | NO_MODE   -> everything suppressed
0x45 = 0x01 | 0x04 |        0x40 = NO_NEWS | NO_TRAFFIC |            NO_MODE   -> AF-RDS *not* suppressed
```

The reverse-engineered "0x45 shows the AF-RDS icon" and the third-party header agree
exactly. **`0x55` is not a magic number; it is `all NO_* flags set`.** The library must
emit `kIconsNone` (defined as that OR-expression) rather than `0x55`, and the `0x45` case
becomes `kIconsNone & ~ICON_NO_AFRDS`. This also retires the "0x55 // unknown/fixed"
comment at header byte 2 — see §3.4.

The popup sweep values from W5 do **not** decode as cleanly, and this is recorded as an
open question rather than smoothed over:

| Sweep value | Bit decode | W5 observation | Verdict |
|---|---|---|---|
| `0x09` | `NO_NEWS \| TRAFFIC_ARROW` | used by the OEM "VOL nn" popup | plausible: news suppressed, traffic arrow live |
| `0x9B` | `NO_NEWS \| NEWS_ARROW \| TRAFFIC_ARROW \| NO_AFRDS \| 0x80` | "shows a traffic icon; the list blinked" | traffic arrow set — consistent; bit 7 undefined in W6 |
| `0x94` | `NO_TRAFFIC \| NO_AFRDS \| 0x80` | "no icon shown" | **inconsistent**: NO_NEWS and NO_MODE are clear yet nothing renders |

Bit 7 has no name in W6 and appears set in both high sweep values. W5's own note that "the
icon codes repeat cyclically across `0x00–0xFF`" is consistent with the high bits being
ignored or aliased. Do not claim the bitmap is fully decoded (§6).

### 3.2 The format byte — it is an ASCII code with a mode bit

Three descriptions existed, none cross-referenced:

* W6: `data[len++] = 0x60 | (chan & 7);`
* W7 `CarminatDisplay::setText`: `textFormat = 0x60`, documented `0x19–0x3F = radio-style
  (5 digits + '.' + 1 char)`, `0x59–0x7F = plain ASCII (up to 7 chars visible)`.
* W7 `showInfoMenu` comment: *"it also shows some channel symbol based on ascii code, for
  example to show 9 you need send 39 or 79, for # send 23 or 63"*.
* W7 `UpdateListBase::setText`: `chan = (digit <= 9) ? (0x70 + digit) : 0x7A;`

Put together, the byte resolves cleanly:

```
  bit 6 (0x40) : 0 = radio rendering (digits + decimal point)   1 = plain ASCII rendering
  bits 5..0    : the ASCII code of the channel glyph, masked to 6 bits
```

Check it: `'9'` is `0x39`; radio form `0x39`, plain form `0x39 | 0x40 = 0x79`. Exactly what
the comment says. `'#'` is `0x23`; `0x23` / `0x63`. Exactly what the comment says.
`0x70 + digit` = `0x40 | (0x30 + digit)` = plain rendering of `'0'`..`'9'`. `0x60` =
`0x40 | 0x20` = plain rendering of a space, i.e. no channel glyph — which is precisely why
`0x60` is the correct default and why the archive's `0x60 | (chan & 7)` is *wrong* for our
panel (it yields glyphs `0x20`..`0x27`, i.e. space through apostrophe, not digits).

Independent corroboration of the field *names* comes from the serial console's own usage
banner (W3), which predates all of the above and was written by whoever first drove the
panel by hand:

```
Usage: mtx <textType hex> <chan hex> <loc hex> <8char_text>
 mtx 7E 69 01 aaaf1234
 mtx 76 60 01 asdf1234
 mtx 7E 61 01 aaaf1234
```

Three fields, named `textType` / `chan` / `loc` — the same three the archive calls the
command, `0x60 | chan`, and `loc`. The values exercised are `0x69`, `0x60` and `0x61`,
i.e. plain rendering of `')'`, `' '` and `'!'` under the §3.2 rule, with `loc` held at
`0x01` (item 0, selected) throughout. Nothing here contradicts the rule, and the naming is
a third independent arrival at the same decomposition.

Consequence for the library: the format byte is computed, not tabulated —
`affa::formatByte(char glyph, bool plain)`. The stated ranges `0x19–0x3F` / `0x59–0x7F`
follow from the rule and are documentation, not a lookup table. `0x19` sits below the
printable range and is annotated in the legacy source as "for [tick glyph]" — treat as a special case,
not as evidence against the rule.

### 3.3 Row / location byte — `AFFA3_LOCATION` is the thing we open-code

W6 defines the location byte as a packed field:

```c
#define AFFA3_LOCATION(max,idx)   ((((max) & 7) << 5) | (((idx) & 7) << 2))
#define AFFA3_LOCATION_SELECTED   0x01
#define AFFA3_LOCATION_FULLSCREEN 0x02
```

`UpdateListBase::setText` hardcodes `loc = 0x01` — that is `AFFA3_LOCATION(0,0) |
SELECTED`: a one-item list, item 0, selected. `affa3_display_full_screen` (W6) builds
`AFFA3_LOCATION(packets-1, i) | SELECTED | FULLSCREEN` to page long text across up to 7
slots. **We have never used that mechanism**; the Carminat family reaches fullscreen a
different way (`0x21` mode `0x05`). The macro is worth porting anyway because it is the
only documented meaning of a byte we currently emit as a constant, and because a future
UpdateList menu needs it.

Note the disagreement between families: Carminat marks its two menu rows with `0x7E` /
`0x7F` *inside* the `0x21` payload (W7 `showMenu`, `ScreenDecode::OFF_ITEM0_MARK`), while
AFFA2/UpdateList uses the packed location byte. They are not the same field and must not
be unified.

### 3.4 The header bytes our encoders emit, versus what the tables name

`0x74`/`0x77` header, as emitted by `CarminatDisplay::setText` and `showPopupText`:

| Offset | Our code | Archive equivalent (`7F` form) | Reading |
|---|---|---|---|
| 0 | `mode` `0x74` / `0x77` | command `0x7F` / `0x76` | screen command |
| 1 | `rdsIcon` `0x55` / `0x45`, popup `icon` | `icons` | **the `AFFA3_ICON_*` bitmap** (§3.1) |
| 2 | `0x55` "unknown/fixed" | `0x55` fixed | second icon bank, fixed at "all NO_*" — see below |
| 3 | `sourceIcon` `0xFF`/`0xDF`/`0xFD` | `mode` | **`AFFA3_ICON_MODE_NONE` is `0xFF`** — same field |
| 4 | `textFormat` `0x60` | `0x60 \| (chan & 7)` | format byte (§3.2) |
| 5 | `0x01` "control byte" | `loc` | **the location byte** (§3.3) |

Two findings fall out of laying the two side by side:

* `sourceIcon` and the archive's `mode` argument are one field, and `0xFF` — which our code
  emits as "none" and the archive names `AFFA3_ICON_MODE_NONE` — is the same value with the
  same meaning. The documented `0xDF` = "MANU", `0xFD` = "PRESET", "others show icons like
  LIST" (W7 docblock) are values of that field with no names in W6.
* The "unknown/fixed `0x55`" at offset 2 is a second occurrence of the *icon bitmap value
  for "everything suppressed"*, in a slot the archive also holds at `0x55`. That is
  suggestive but not proven — two banks of icons, second bank always off. Listed as an
  open question, and the library keeps emitting `0x55` there under the name
  `kIconBank2None` so that the guess is at least labelled.
* Our `0x01` "control byte — always required by display protocol" is the archive's `loc`
  with `AFFA3_LOCATION_SELECTED` set. That is a much better explanation than "always
  0x01", and it predicts that changing it changes row placement rather than breaking the
  protocol.

### 3.5 Scroll-arrow indicator

Already named in W7 (`Carminat::ScrollLockIndicator`) and computed in `Menu::getScrollIndicator`:

| Value | Meaning | Emitted when |
|---|---|---|
| `0x00` | no arrows | never emitted by the menu |
| `0x07` | up arrow only | selection is at the last item, or second-to-last with the cursor on row 0 |
| `0x0B` | down arrow only | selection is at item 0, or item 1 with the cursor on row 1 |
| `0x0C` | both arrows | otherwise |

It sits at **content offset 8** of the `0x21` mode `0x01` message — which is *payload*
offset 10, the number `ScreenDecode::OFF_SCROLL` actually holds, because that decoder
indexes from the `0x10` PCI. See §1.2 before copying any offset out of either source.
This is a *protocol* computation driven by the sliding window, which is why the menu is the
library's and not the application's (W8 and the library brief agree on this).

---

## 4. `affa3.h` symbol -> our magic number -> proposed `affa::` constant

The table the extraction exists to produce. "Ours today" is what MeganeCAN/MegaOpen emit;
where a cell says *open-coded*, the value appears as a bare literal in an encoder.

### 4.1 Transport and identifiers

| `affa3.h` symbol | Value | Ours today | Proposed `affa::` name | Home |
|---|---|---|---|---|
| `AFFA3_PACKET_LEN` | `0x08` | `AffaCommon::PACKET_LENGTH` | `kFrameLen` | `core/AffaConstants.h` |
| `AFFA3_PACKET_FILLER` | `0x81` | `UpdateList::PACKET_FILLER` `0x81`, `Carminat::PACKET_FILLER` `0x00` | `SyncProfile::filler` | `core/AffaSyncProfile.h` |
| `AFFA3_PACKET_REPLY_FLAG` | `0x400` | `PACKET_REPLY_FLAG` (both) | `kReplyFlag`, `SyncProfile::replyFlag` | `core/AffaConstants.h` |
| `AFFA3_PACKET_ID_SYNC` | `0x3DF` | `UpdateList` `0x3DF`, `Carminat` `0x3AF` | `SyncProfile::syncId` | profile |
| `AFFA3_PACKET_ID_SYNC_REPLY` | `0x3CF` | both `0x3CF` | `SyncProfile::syncReplyId` | profile |
| `AFFA3_PACKET_ID_SETTEXT` | `0x121` | `UpdateList` `0x121`, `Carminat` `0x151` | `kIdSetText` (per family) | family constants |
| `AFFA3_PACKET_ID_DISPLAY_CTRL` | `0x1B1` | `UpdateList` `0x1B1`, `Carminat` `0x151` | `kIdDisplayCtrl` | family constants |
| `AFFA3_PACKET_ID_KEYPRESSED` | `0x0A9` | `UpdateList` `0x0A9`, `Carminat` `0x1C1` | `kIdKeyPressed` | family constants |
| — (no archive name) | `0x1F1` | `Carminat::PACKET_ID_NAV` | `kIdNav` | `carminat/CarminatConstants.h` |
| — | `0x10` | *open-coded* in every builder | `kPciFirstFrame` | `proto/IsoTp.h` |
| — | `0x20` | *open-coded* `0x20 + num` | `kPciConsecutiveBase` | `proto/IsoTp.h` |

### 4.2 Sync state machine

| `affa3.h` symbol | Value | Ours today | Proposed `affa::` name |
|---|---|---|---|
| `AFFA3_SYNC_STAT_FAILED` | `0x01` | `SyncStatus::FAILED` | `SyncStatus::Failed` |
| `AFFA3_SYNC_STAT_PEER_ALIVE` | `0x02` | `SyncStatus::PEER_ALIVE` | `SyncStatus::PeerAlive` |
| `AFFA3_SYNC_STAT_START` | `0x04` | `SyncStatus::START` | `SyncStatus::Start` |
| `AFFA3_SYNC_STAT_FUNCSREG` | `0x08` | `SyncStatus::FUNCSREG` | `SyncStatus::FuncsReg` |
| `AFFA3_PING_TIMEOUT` | `5` (**tick counts**) | `SYNC_TIMEOUT = 5` (**tick counts**) | `AFFA_PEER_TIMEOUT_MS = 5000` (**milliseconds**) |
| — | `0xB9` / `0x79` | *open-coded* in `tick()` | `SyncProfile::aliveByte` |
| — | `0xBA` / `0x7A` | *open-coded* in `tick()` | `SyncProfile::requestByte` |
| — | `0x00` / `0x01` (`data[1]` of the request) | *open-coded* | `SyncProfile::requestArg` |
| — | `0x61 0x11` | *open-coded* in `recv()` | `kPeerSyncRequest` |
| — | `0x69` | *open-coded* in `recv()` | `kPeerAlive` |
| — | `0x01` at `data[2]` of `61 11` | *open-coded* | `kPeerSyncStartFlag` |
| — | `70 1A 11 00 00 00 00 01` | *open-coded*, sent once | `SyncProfile::hello[0]` |
| — | `B0 14 11 00 1F 00 00 00` | *open-coded*, sent **twice** | `SyncProfile::hello[1]`, `hello[2]` |

`AFFA3_PING_TIMEOUT` is the defect. It is a **count of `affa3_tick()` calls**, and it means
"five seconds" only if the caller ticks at exactly 1 Hz — which the archive did, from a
timer interrupt. Both our ports copied the constant *and the counter*, then called `tick()`
from a free-running loop, at which point it expired in milliseconds. The unit change from
"5" to "5000 ms" is not cosmetic; it is the fix.

### 4.3 Function / ACK state

| `affa3.h` symbol | Value | Ours today | Proposed `affa::` name |
|---|---|---|---|
| `AFFA3_FUNC_STAT_IDLE` | `0x00` | `FuncStatus::IDLE` | `TxState::Idle` |
| `AFFA3_FUNC_STAT_WAIT` | `0x01` | `FuncStatus::WAIT` | `TxState::WaitAck` |
| `AFFA3_FUNC_STAT_PARTIAL` | `0x02` | `FuncStatus::PARTIAL` | (ACK classification, not a state) |
| `AFFA3_FUNC_STAT_DONE` | `0x03` | `FuncStatus::DONE` | (ACK classification) |
| `AFFA3_FUNC_STAT_ERROR` | `0x04` | `FuncStatus::ERROR` | (ACK classification) |
| — | `0x70` | *open-coded* `regdata[1] = {0x70}` | `kFuncRegister` |
| — | `0x74` | *open-coded* in both `recv()` and in the auto-ACK builder | `kAckDone` |
| — | `0x30 0x01 0x00` | *open-coded* | `kAckPartial[3]` |
| — | `2000` (ms) | *open-coded* `timeout = 2000` | `AFFA_ACK_TIMEOUT_MS` |
| `AFFA3_ENOTSYNC` | `0x01` | `AffaError::NoSync` | `Result::NoSync` |
| `AFFA3_EUNKNOWNFUNC` | `0x02` | `AffaError::UnknownFunc` | `Result::UnknownFunc` |
| `AFFA3_ESENDFAILED` | `0x03` | `AffaError::SendFailed` | `Result::Failed` |
| `AFFA3_ETIMEOUT` | `0x04` | `AffaError::Timeout` | `Result::Timeout` |
| `AFFA3_ESTRTOLONG` | `0x05` | `AffaError::StrTooLong` | `Result::TooLong` |

### 4.4 Display control, icons, location

| `affa3.h` symbol | Value | Ours today | Proposed `affa::` name |
|---|---|---|---|
| `AFFA3_DISPLAY_CTRL_DISABLE` | `0x00` | `DisplayCtrl::Disable` `0x00` (both) | `kDisplayCtrlOff` |
| `AFFA3_DISPLAY_CTRL_ENABLE` | `0x02` | `UpdateList` `0x02`, **`Carminat` `0x09`** | `kDisplayCtrlOn` (per family) |
| `AFFA3_ICON_NO_NEWS` … `AFFA3_ICON_NO_MODE` | bits 0..6 | `AffaCommon::IconFlags` (already ported, unused) | `IconFlags` — and **now used**: see §3.1 |
| — | `0x55` (`setText` `rdsIcon`) | *open-coded* | `kIconsNone` = `NoNews\|NoTraffic\|NoAfRds\|NoMode` |
| — | `0x45` (documented "AF-RDS") | *open-coded* in a comment only | `kIconsAfRds` = `kIconsNone & ~NoAfRds` |
| — | `0x55` (`setText` header byte 2) | *open-coded* "unknown/fixed" | `kIconBank2None` (name records the guess) |
| `AFFA3_ICON_MODE_NONE` | `0xFF` | *open-coded* `sourceIcon = 0xFF` | `kSrcIconNone` |
| — | `0xDF` / `0xFD` | documented in a comment, never emitted | `kSrcIconManu` / `kSrcIconPreset` |
| — | `0x76` / `0x7F` | *open-coded* `textType = 0x76`; `0x7F` never emitted | `kCmdTextOnly` / `kCmdTextWithIcons` (§2.2) |
| — | `0x19` / `0x1C` | *open-coded* alongside the command | `kLenTextOnly` / `kLenTextWithIcons` — derived, not tabulated |
| — | `0x10` (separator between old and new text) | *open-coded* | `kTextFieldSeparator` — **same value as `kPciFirstFrame`, different layer**; see the note below |
| `AFFA3_LOCATION(max,idx)` | macro | *open-coded* `loc = 0x01` | `constexpr uint8_t location(uint8_t max, uint8_t idx)` |
| `AFFA3_LOCATION_SELECTED` | `0x01` | *open-coded* (`loc = 0x01`, "control byte 0x01") | `kLocSelected` |
| `AFFA3_LOCATION_FULLSCREEN` | `0x02` | never emitted | `kLocFullscreen` |
| — | `0x60` (`textFormat`) | *open-coded* | `formatByte(' ', /*plain=*/true)` |

`0x10` earns the warning. In `10 19 7E 70 01 "asdf1234" 10 "asdf1234    " 00` (W3) the byte
appears twice: at offset 0 it is the ISO-TP first-frame PCI, and at offset 13 it is the
separator between the old and the new text field. They are unrelated, they live in different
layers (§1), and giving them one shared constant — or worse, letting a receiver scan for
`0x10` to find a frame boundary — is a bug waiting to happen. Two names, one value, and a
comment at each saying why the other exists.

### 4.5 Keys

| `affa3.h` symbol | Value | Ours today | Proposed `affa::` name |
|---|---|---|---|
| `AFFA3_KEY_LOAD` | `0x0000` | `AffaKey::Load` | `Key::Load` |
| `AFFA3_KEY_SRC_RIGHT` | `0x0001` | `AffaKey::SrcRight` | `Key::SrcRight` |
| `AFFA3_KEY_SRC_LEFT` | `0x0002` | `AffaKey::SrcLeft` | `Key::SrcLeft` |
| `AFFA3_KEY_VOLUME_UP` | `0x0003` | `AffaKey::VolumeUp` | `Key::VolumeUp` |
| `AFFA3_KEY_VOLUME_DOWN` | `0x0004` | `AffaKey::VolumeDown` | `Key::VolumeDown` |
| `AFFA3_KEY_PAUSE` | `0x0005` | `AffaKey::Pause` | `Key::Pause` |
| `AFFA3_KEY_ROLL_UP` | `0x0101` | `AffaKey::RollUp` | `Key::RollUp` |
| `AFFA3_KEY_ROLL_DOWN` | `0x0141` | `AffaKey::RollDown` | `Key::RollDown` |
| `AFFA3_KEY_HOLD_MASK` | `0xC0` | `AffaCommon::KEY_HOLD_MASK` | `kKeyHoldMask` |
| `AFFA3_KEY_QUEUE_SIZE` | `8` | `std::queue` (unbounded, heap) | `AFFA_KEY_QUEUE_DEPTH` (static) |
| — | `0x03 0x89` | *open-coded* in both `recv()` and `emulateKey` | `kKeyFramePrefix[2]` |

Note the archive's key-frame validity test is `if ((data[0] == 0x03) && (data[1] != 0x89))
return;` — it rejects only frames that start `0x03` and then fail to be `0x89`, letting a
frame with a different `data[0]` through to the key extractor. Both our ports quietly
tightened this to require `03 89` (Carminat) or kept the loose form and fell through to the
auto-ACK (UpdateList). The library requires `03 89` and documents the divergence; the loose
form has no defensible reading.

---

## 5. What the captures actually show

### 5.1 The sync channel has structure: high nibble = speaker, low nibble = opcode

Laying every sync-channel byte we have witnesses for side by side:

| Byte | Speaker | Channel | Opcode | Witness |
|---|---|---|---|---|
| `0x50 29 …` | OEM radio | `0x3AF` | 0 = announce | W2 |
| `0x59 00` | OEM radio | `0x3AF` | 9 = alive | W2 |
| `0x5A 01` | OEM radio | `0x3AF` | A = sync request | W2 |
| `0x49 00` | third node | `0x3BF` | 9 = alive | W2 |
| `0x61 11` | panel/cluster | `0x3CF` | 1 = "tell me who you are" | W1, W2 (`3CF 2 61 23`), W3, W6 |
| `0x69` | panel/cluster | `0x3CF` | 9 = alive | W1, W2, W3, W6 |
| `0x70 1A 11 …` | us / AFFA2 | `0x3AF` / `0x3DF` | 0 = announce | W1, W6 |
| `0x79 00` | AFFA2 | `0x3DF` | 9 = alive | W6 |
| `0x7A 01` | AFFA2 | `0x3DF` | A = sync request | W6 |
| `0xB0 14 11 …` | us (Carminat) | `0x3AF` | 0 = announce | W1, W7 |
| `0xB9 00` | us (Carminat) | `0x3AF` | 9 = alive | W1, W7 |
| `0xBA 00` | us (Carminat) | `0x3AF` | A = sync request | W1, W7 |

The low nibble is the opcode and it is invariant across every speaker: **0 = announce,
1 = request identity, 9 = alive, A = request sync**. The high nibble tags the speaker or
the protocol generation (`5x` OEM radio, `6x` panel, `7x` AFFA2, `Bx` our Carminat
profile). Nothing in the legacy code says this; the `SyncProfile` struct encodes it
implicitly by carrying `aliveByte` and `requestByte` per family, and this section is why
that struct is shaped the way it is rather than being a free-form byte table.

This also explains the `hello` array: our Carminat announce is `70 1A 11 …` (a `7x`-family
byte) followed by `B0 14 11 …` twice (a `Bx`-family byte). We answer in *two* generations.
Whether the panel needs both, or the `70` frame alone, has never been tested — §6.

**The `0x11` in our hello frames is not a constant. It is an echo — and we hardcoded it.**

Lay the identity exchange out in time order and the structure is hard to miss:

| Who | Frame | Note |
|---|---|---|
| our panel (W1) | `3CF  61 **11** 00 …` | identity request, identity token `0x11` |
| us (W1, W7) | `3AF  70 1A **11** 00 00 00 00 01` | our announce carries `0x11` back |
| us (W1, W7) | `3AF  B0 14 **11** 00 1F 00 00 00` | so does the second announce |
| OEM cluster (W2) | `3CF 2 61 **23**` | identity request, identity token `0x23` |
| OEM radio (W2) | `3AF 8 50 29 00 **23** 00 00 00 69` | announce carries `0x23` back, 3 ms later |

Both sides of an exchange we never observed together nonetheless agree: the announce
repeats the token the peer used in its `61 <token>`. Our code does not compute that — it
emits `0x11` as a literal, and it *also* gates the whole branch on `data[1] == 0x11`:

```cpp
if ((packet->data.uint8[0] == 0x61) && (packet->data.uint8[1] == 0x11))   // W7, both families
```

So a panel that identifies itself the way the **factory instrument cluster in W2 does**
(`61 23`) falls straight through to the `unknown sync packet` branch, is never answered,
and the link never comes up. This has never bitten us because the bench panel says `0x11`
every time.

This is a **hypothesis, not a decoded field** — one observation per side, and the token
sits at `data[2]` in our `70 1A <t>` form but at `data[3]` in the OEM's `50 29 00 <t>`
form, so it is positional within a frame shape rather than at a fixed offset. It is
recorded because it is the only reading that explains `0x23` at all, and because the cost
of being wrong is asymmetric: the library should match on `data[0] == 0x61` alone, keep
emitting `0x11` (byte-identical to today for our panel), and expose the observed token so
an application on different hardware has something to work with. Do **not** silently start
echoing the received token — that would change the wire on the one panel we can actually
test against. §6 item 3.

### 5.2 A complete OEM registration, end to end

`notes/notes1` "Dashboard log" (W2), the only witness we have of the *factory* radio
bringing up the *factory* cluster. Verbatim, with the original annotations translated:

```
04,339 3AF 2 5A 01                          radio: sync request
04,340 3AF 2 5A 01                          radio: sync request  (again, +1 ms)
04,345 3AF 2 5A 01                          radio: sync request  (again, +5 ms)
04,405 3AF 2 5A 01                          radio: sync request  (again, +60 ms)
04,409 3CF 2 61 23                          cluster: identity request
04,410 3AF 2 5A 01                          radio: sync request  (fifth and last)
04,412 3AF 8 50 29 00 23 00 00 00 69        radio: initialisation request
04,419 1C1 8 70 84 84 84 84 84 84 84        cluster: register function 0x1C1
04,419 5C1 8 74 FF FF FF FF FF FF FF        radio: ACK (DONE)
04,442 3AF 8 50 29 00 23 00 00 00 69        radio: initialisation request (2nd)
04,472 3AF 8 50 29 00 23 00 00 00 69        radio: initialisation request (3rd)
04,473 121 8 70 FF FF FF FF FF FF FF        radio: register function 0x121
04,473 1B1 8 70 FF FF FF FF FF FF FF        radio: register function 0x1B1
04,475 521 8 74 84 84 84 84 84 84 84        cluster: ACK 0x121
04,475 5B1 8 74 84 84 84 84 84 84 84        cluster: ACK 0x1B1
04,818 3AF 2 59 00                          radio: alive
04,835 3BF 2 49 00                          third node: alive
04,836 3CF 1 69                             cluster: alive
04,873 1B1 8 03 52 00 00 FF FF FF FF        radio: display control (SF len 3, cmd 0x52, state 0x00)
04,877 5B1 8 74 84 84 84 84 84 84 84        cluster: ACK
04,893 1C1 8 02 64 0F 84 84 84 84 84        cluster: SF len 2, cmd 0x64 (unknown)
04,893 5C1 8 74 FF FF FF FF FF FF FF        radio: ACK
```

Seven things this settles:

1. **Registration is bidirectional and symmetric.** Each side sends `0x70` on the ids *it*
   owns and the other side ACKs with `0x74` on `id | 0x400`. The cluster registers `0x1C1`
   (its key channel); the radio registers `0x121` and `0x1B1` (its text and display-control
   channels). Our implementation does exactly this half of it — we register `0x151` and
   `0x1F1`, and we ACK the panel's `0x1C1` registration. W1 shows both halves in one trace.
   The original annotator's Russian labels are worth preserving here because they show how
   the exchange was *read* at the time: the cluster's `1C1 70 …` is
   "Ответ Приборки на запрос инициализации" — *the cluster's **reply** to the init request*,
   7 ms after `50 29 …`, not a spontaneous announcement. The panel registers when invited.
   W1 matches: our panel's `1C1 70 A3 …` follows our hello frames.
   **But do not inherit the rest of the annotator's labelling.** They call every `74` ACK a
   *"Запрос Магнитолы N(1)"* — "radio request N" — and number them 1, 2, 3, then label the
   cluster's `521`/`5B1` ACKs "reply to request 2 / 3". That reads the ACK as the request.
   The filler tell (point 5 below) settles it independently of any annotation: `5C1 74 FF…`
   is padded `0xFF`, so the radio sent it; `521`/`5B1 74 84…` are padded `0x84`, so the
   cluster sent them. Each is an ACK of the `70` on the corresponding base id. Where the
   annotations and the filler disagree, the filler wins — it is evidence, the labels are
   someone's reading.
2. **Registration happens once, on link-up, before any content.** The lazy scheme in the
   archive (register on first send) produces the same wire order because the first send
   follows link-up immediately. Note the OEM ordering is *ours inverted*: the radio's own
   `70` registrations (`121`, `1B1`) come 54 ms **after** the cluster's, i.e. after the
   init handshake has completed. Our lazy scheme happens to reproduce that ordering, but
   for an unrelated reason. Do not treat the coincidence as a constraint if the queue ever
   reorders — it is the *content* of the registration burst that must stay byte-identical,
   and the rule that FUNCSREG latches only after the last ACK.
3. **The steady state is ~2 Hz here** (`04,818` -> `05,319` -> `05,820` for the radio's
   `59 00`; the cluster's `69` at `04,836` -> `05,340` -> `05,848`), i.e. roughly 500 ms.
   W1 measures the panel's `69` at ~1 Hz. The rate is not tightly specified; what matters
   is that the peer-alive watchdog is several multiples of it. `AFFA_PEER_TIMEOUT_MS = 5000`
   against a 500–1000 ms ping is a 5–10x margin.
4. **Frames are not padded to 8 bytes on a real bus.** `3AF 2 5A 01`, `3CF 1 69`,
   `3FF 2 92 01` — DLC 1 and 2. Our implementation always sends DLC 8 with filler, and the
   panel accepts it (W1). The library keeps DLC 8 for byte-identical reproduction, but a
   receiver must never assume `len == 8`.
5. **The filler byte is a don't-care.** Across the witnesses it takes the values `0x84`
   (cluster, W2), `0xFF` (radio, W2), `0xA2` (panel, W3), `0xA3` (panel, W1), `0x81`
   (archive, W6), `0x00` (our Carminat, W7). Six values, all accepted. `SyncProfile::filler`
   exists to reproduce our historical choice exactly, not because the protocol demands one.
   Note the OEM pair uses filler as an **identity tell**: everything the cluster sends is
   padded `0x84`, everything the radio sends is padded `0xFF`, consistently, in both
   directions and on every id. Useful when reading a capture with no direction column.
6. **Repeating an unanswered sync frame is OEM-normal.** The radio sends `5A 01` five times
   in 71 ms and stops the moment the cluster answers with `61 23`; it then sends its
   `50 29 …` announce three times at ~30 ms spacing. This is the first real evidence
   bearing on open question 4 (*is our duplicated `B0 14 11` load-bearing?*): duplication
   of announce frames is what the factory radio does too, so the duplicate is more likely
   deliberate mimicry of OEM behaviour than a copy-paste slip. It does **not** settle
   whether the panel requires it. It does mean the library must keep `helloCount` as a
   count with repeats expressible, which the `SyncProfile::hello[]` array already does.
7. **Our sync-request cadence is far slower than the OEM's.** We emit one `0xBA` per 1 Hz
   tick while `FAILED|START`; the radio bursts. This is the honest reading of the archive's
   `_delay_ms(100)` in that branch — it was trying to retry fast, and blocking was the only
   tool it had. Deleting the delay (as the brief requires) does not restore the burst; it
   removes the block. If bring-up ever proves slow on a cold bus, the fix is a separate
   `AFFA_SYNC_RETRY_MS` faster than `AFFA_SYNC_INTERVAL_MS` while unsynced — a state in the
   FSM — never a delay.

### 5.3 Our implementation against the real panel — and what the capture's own labels hide

W1, taken minutes before the library was specified, on the 2-node bench (ESP32-C3 +
Carminat panel, 500 kbit/s):

```
RX  3CF  61 11 00 A3 A3 A3 A3 A3     panel: identity request  (filler 0xA3)
RX  3CF  69 00 A3 A3 A3 A3 A3 A3     panel: alive, ~1 Hz
RX  1C1  70 A3 A3 A3 A3 A3 A3 A3     panel: register function 0x1C1
TX  5C1  74 00 00 00 00 00 00 00     us: ACK DONE (0x1C1 | 0x400)
TX  3AF  70 1A 11 00 00 00 00 01     us: announce, in reply to 61 11
TX  3AF  B0 14 11 00 1F 00 00 00     us: sent TWICE, in reply to 61 11
TX  3AF  B9 00 00 00 00 00 00 00     us: alive, 1 Hz
TX  3AF  BA 00 00 00 00 00 00 00     us: sync request (only while FAILED or START)
```

Note `data[2] == 0x00` in the panel's `61 11 00`: the `START` flag is **not** set on this
link-up (W6/W7 set `START` only when `data[2] == 0x01`). W2's cluster sends a 2-byte
`61 23`, in which `data[2]` does not exist at all (§6 item 3).

**The `1 Hz` annotation on that capture is our intent, not the wire rate.** `tick()` is
called from two places, and only one of them is paced:

```cpp
// W7 main.cpp:494 (and disp/disp_main.cpp:156) — the intended cadence
if (now - last_sync > SYNC_INTERVAL_MS) { last_sync = now; display->tick(); }

// W7 CarminatDisplay.cpp:350 and UpdateListBase.cpp:93 — inside recv(), unpaced
else if (packet->data.uint8[0] == 0x69) { _sync_status |= PEER_ALIVE; tick(); }
```

`tick()` transmits the alive byte unconditionally at its top. So **every `0x69` the panel
sends provokes an extra `0xB9` from us**, and the real heartbeat rate is the 1 Hz timer
*plus* the panel's ping rate — about 2 Hz on the bench, and whatever the peer chooses in
general. Three consequences, in increasing order of importance:

1. The library's paced heartbeat is a **deliberate divergence from legacy wire behaviour**,
   not a reproduction of it. "Reproduce the bytes exactly" applies to frame *content*; the
   *rate* is being fixed on purpose. The brief's host test — a million `poll()` calls in a
   simulated second must emit exactly one `0xB9` — is precisely the assertion that this
   re-entrancy is gone. See the warning in §7.
2. **The two defects were masking each other.** The tick-counter watchdog (§4.2) is reset
   inside `tick()`'s `else` branch when `PEER_ALIVE` is set. Because `recv()` sets that flag
   and *immediately* calls `tick()`, the counter was in practice being re-armed by the
   peer's ping rather than by the loop's cadence — which is why a watchdog that counts calls
   survived for months in a free-running loop. Remove the re-entrancy while keeping the
   counter and you get spurious peer-loss; keep the re-entrancy while fixing the counter and
   you keep the double heartbeat. **Both must change together**, which is exactly what
   lifting the FSM into the base and giving it a millisecond deadline does.
3. MegaOpen fixed the deadline and **left the re-entrant `tick()` in place**
   (`MegaOpen/src/display/Carminat/CarminatDisplay.cpp`, peer-alive branch of `recv()`).
   So no fix for this exists in either parent to be carried across: it is a third defect,
   distinct from the counter and from the blocking ACK wait, that neither project noticed.
   It is recorded here because a faithful port of *either* parent reintroduces it, and
   because point 2 means reintroducing it silently changes what the watchdog measures.

A related trap for the same code: Carminat's sync request is emitted as
`0xBA` followed by seven filler bytes, and Carminat's filler happens to be `0x00`, which is
why the wire shows `BA 00 00 …` and why `SyncProfile::requestArg` is `0x00` for Carminat.
UpdateList emits `0x7A, 0x01` explicitly with `0x81` filler. The two families look
symmetrical on the wire but only one of them actually has a request argument. Do not
"restore the symmetry" by setting Carminat's `requestArg` to `0x01`.

W3, our firmware's own serial trace, is the only witness for the AFFA2 segment text on the
wire:

```
[RX] ID: 0x3CF Len: 8 Data: { 69 00 A2 A2 A2 A2 A2 A2 }
[do_set_text] ID: 0x121, len=27
10 19 7E 70 01 61 73 64 66 31 32 33 34 10 61 73 64 66 31 32 33 34 20 20 20 20 00
```

Decoding it with §1 and §3: PCI `0x10` first frame, declared length `0x19` = 25, command
`0x7E`, format `0x70` (= plain rendering of `'0'`), location `0x01` (item 0 selected),
`"asdf1234"` as the *old* text, separator `0x10`, `"asdf1234    "` as the *new* text,
terminator `0x00`. 25 content bytes exactly as declared. This is the cleanest single
witness we have that the length byte, the format byte and the location byte all mean what
§3 says they mean.

### 5.4 Screens from OEM captures

W4's decodes of the factory radio driving the factory panel are the origin of every
Carminat screen builder:

* `21 05 FF 00 00 40` + 26 zero bytes + `20 20` + `"Please insert    \r  navigation CD    \r …"`
  — the fullscreen text screen, captured intact.
* `76 60 41` + `"AUX  ON "`, `76 60 44` + `"AF   ON "`, `76 60 48` + `"SPEED 0 "` — the
  settings list, one ISO-TP message per row. The row codes `0x41`/`0x44`/`0x48` and the
  prefix `0x60` are OEM values; our `showInfoMenu` defaulted to prefix `0x70` and W4
  explicitly flags that as a divergence to correct.
* `77 55 55 FF 60 01` + `"  CODE  "` then `"  \xB0 000 "` — the radio code entry screen.
  `0xB0` is a **blinking cursor glyph on digit 0**, advancing `B0 -> B1 -> …` as digits are
  accepted. That is a charset fact, not a protocol fact, and it is the only glyph outside
  printable ASCII we have identified.
* `77 09 55 FF 31 01 "   1056"` — normal radio text after code entry, rendering as
  "105.6". Format byte `0x31`: bit 6 clear = radio rendering, low bits `0x31` = `'1'`.
  The decimal point is inserted by the radio rendering mode, exactly as §3.2 predicts.

### 5.5 The device-monitor logs (W9) — one finding, and it is not a protocol finding

All ten `notes/device-monitor-250826-*.log` were grepped for the AFFA identifiers and for
`affa`. The result, exactly:

| Files | Content |
|---|---|
| 1 (`-024317`) | empty, 0 bytes |
| 6 | ELM/UDS diagnostic chatter and WiFi bring-up only; zero AFFA hits |
| 3 (`-022515`, `-023600`, `-024332`) | `affa3_do_send called` × 38 / 40 / 40, `scrolled part:` × 38 in all three, and a single bare ` ID 0x121` line |

Not one wire byte in any of them: the traces were taken with the verbose ISO-TP dump off
(`vb 0`, which `AFFA3_SCREENS.md` recommends precisely so that continuous renders do not
saturate serial and drop frames). **They are not protocol witnesses** and nothing in §1–§4
rests on them.

They do witness one thing, and it happens to be the empirical origin of a library
requirement. Once the scroll effect starts, every scroll step is followed by exactly one
send, in all three files, for the whole run — verbatim from `-022515` (the ` ID 0x121`
line is the only identifier printed anywhere in the ten logs, and it confirms the sends are
AFFA text sends):

```
affa3_do_send called
scrolled part: 
affa3_do_send called
scrolled part: 
 ID 0x121
affa3_do_send called
scrolled part: 
affa3_do_send called
scrolled part: 
```

`scrolled part:` prints with an empty payload — the text being scrolled is not logged, only
the fact that a step occurred. The counts are 38 steps against 38, 40 and 40 sends; the two
extra sends in the longer files are boot-time renders that precede the first scroll step
(`-024332` opens with two bare `affa3_do_send called` lines directly after
`Auto restore getted and is true.`, before any scrolling begins). From the first scroll step
onward the pairing is 1:1.

Every step of the marquee scroll effect issues a **complete multi-frame ISO-TP screen
send**, each of which then blocks up to 2 s per frame waiting for its ACK (§1). Nothing
throttles it and nothing supersedes it: a scroll running while the user turns the wheel
puts a dozen stale frames ahead of the reaction to the keypress. That is the "counter keeps
ticking for a second after Pause was pressed" symptom in the brief, observed in the wild
eight months before it was described as a requirement, and it is why
**latest-value-wins coalescing on a `RenderSlot` is a correctness feature and not an
optimisation**. The scroll effect is the worst offender because it is the only renderer
that fires on a timer rather than on a state change.

The useful captures remain `notes/notes1` (W2, W3), the `logs affa3 new` set distilled into
`AFFA3_SCREENS.md` (W4), and the live bench capture (W1).

### 5.6 `ARCHITECTURE-V2.md` (W10) — what it does and does not contain

**Correction to the extraction brief, made here so nobody wastes an afternoon on it:** the
brief cites "`ARCHITECTURE-V2.md` sections 6 and 8.5 (the display protocol)". Neither
section is about the display protocol. §6 ("C3 refactors (P0 — done) → DISP firmware") is a
firmware refactor plan, and §8.5 ("Engineering patterns worth copying") is a checklist
distilled from two unrelated third-party CAN-box projects. **Not one wire byte of AFFA
appears in either.** The AFFA content of that document is in §8.2 and §8.6, and those two
sections are genuinely valuable.

**§8.2 — an independently-sourced key table.** It states the key encoding without reference
to our code, and agrees with `affa3.h` (W6) and with both our ports on every value:

> UpdateList `0x0A9` / Carminat `0x1C1`; frame `03 89 <hi> <lo>`, hold = bit 0x80/0x40;
> codes: 0=LOAD, 1=SRC→, 2=SRC←, 3=VOL+, 4=VOL−, 5=PAUSE, 0x0101 SEEK+, 0x0141 SEEK−.
> (Repo's `AffaKey` is ground truth.)

Two things to take from it. First, it names `0x0101`/`0x0141` **SEEK+/SEEK−**, where W6
calls them `ROLL_UP`/`ROLL_DOWN` and our code calls them `RollUp`/`RollDown`. Same codes,
two vocabularies for one control — the library keeps `RollUp`/`RollDown` (the older name,
and the one the menu's navigation mapping is written against) and documents "seek" as the
radio-role synonym. Second, it spells the hold mask as *"bit 0x80/0x40"*, i.e. two
independent bits, where every implementation ORs them together as one `0xC0` constant.
Whether the panel ever sets one without the other is untested (§6 item 19).

**§8.2 also carries a trap, verbatim:**

> Note: Clio III uses a different scheme (BIC module, ID `0x58F`, `89 …`/`80 01`
> press/release) — do NOT confuse mikescotland's SWC captures with ours.

A third-party Renault key capture that *looks* like ours (`89` appears in both) and is a
different protocol on a different id from a different module. Any future contributor
importing "Renault steering-wheel key codes" from a forum needs to see this warning.

**§8.6 — third-party corroboration of the UpdateList ids.** `manu-t/autoradio-interface`
(a Clio 2 UpdateList project) is recorded as confirming `0x121` / `0x3CF` / `0x3DF` /
`0x1B1`, and `megane.com.pl/topic/47797` as carrying an UpdateList + `0x0A9` key table.
These are the only witnesses outside this project and the mhroczny archive that attest the
AFFA2 identifiers, and they are what lets §4.1 assert those ids are the *protocol's*, not
one implementation's.

**§8.0 — bus context**, one line of which belongs in the library README: the panel lives on
the **multimedia CAN** (OBD-II pins 12/13 on this platform), which is a different bus from
the 500 kbit/s powertrain CAN on pins 6/14. Everything in §8.1 of that document — `0x0C2`
steering angle, `0x354` speed, `0x60D` door/light bits and so on — is **vehicle** CAN and
will never appear on the bus this library talks to. Recorded so those ids are not later
mistaken for AFFA traffic, which is the same reason §6 item 7 exists.

**§8.5 — the origin of two scheduling rules.** No protocol content, but two of its bullets
are the direct ancestors of requirements in this library's brief, and knowing they came
from field-proven third-party firmware rather than from a design meeting is worth
recording:

> Key forwarding pre-empts telemetry; whole frames only, never byte-interleaved.

That is `Priority::Urgent` plus "a message already in flight is abandoned only at a frame
boundary", arrived at independently by canbox-nissan.

> Send-on-change-or-interval scheduler per message.

That is latest-value-wins coalescing on a `RenderSlot`, minus the interval half (which the
library leaves to the application, since redraw cadence is application policy).

Its watchdog bullet is the counter-example worth heeding: *"CAN-silence 30 s → reboot only
when battery >11 V says the bus should be alive"*. Note what that pattern does **not** do —
it never reinstalls the driver, it reboots. It is consistent with the
`ESP32CAN-CONTRACT.md` prohibition on touching the controller after `begin()`.

---

## 6. Open questions and known-unknowns

Listed so the README cannot overclaim. Each is phrased as the experiment that would close
it. None blocks the port; each blocks a *confident* change to the bytes involved.

**Protocol structure**

1. **Is `0x30 0x01 0x00` really a flow-control frame, or an ACK that happens to look like
   one?** If it is genuine ISO-TP FC (CTS, BS=1, STmin=0) then a peer could legitimately
   send BS>1 and stop acknowledging every frame, and our sender would stall. No capture
   contains any other flow-control shape. *Experiment: none available — requires a panel
   that sends a different BS.*
2. **Consecutive-frame counter roll-over.** The archive increments `num` without bound and
   emits `0x20 + num`; at `num == 16` that becomes `0x30`, which collides with the PARTIAL
   ACK pattern. No message in our repertoire is long enough to reach it (the longest is
   111 content bytes = 16 frames). `AFFA_MAX_PAYLOAD` stays clamped below the collision
   until someone tests it.
3. **The peer's identity token, and two live bugs around it.** `61 <token> [<start>]`. Our
   panel says `61 11 00`; the OEM cluster says `61 23` in a **2-byte frame**. §5.1 argues
   the token is echoed back in the announce. Two defects follow, and both must be fixed
   regardless of whether the echo hypothesis holds:
   *(a)* our code matches `data[1] == 0x11` exactly, so it would never answer the OEM
   cluster at all; match on `data[0] == 0x61` and treat `data[1]` as data.
   *(b)* our code reads `data[2]` unconditionally to test the START flag, which on a DLC-2
   frame reads whatever the driver left in the buffer — **check `len >= 3` first**, and
   treat a missing byte as "not START".
   *Experiment for the echo itself: none available with one panel — it needs a second panel
   with a different token.*
4. **Are both `hello` frames necessary, and is the duplicate `B0 14 11` load-bearing?**
   We send `70 1A 11 …` once and `B0 14 11 …` twice. §5.2 item 6 shows the factory radio
   also repeats its announce (3×), which makes the duplicate look deliberate, but nothing
   shows the panel *requires* it. *Experiment: drop each in turn on the bench and watch
   whether `0x69` keeps arriving.*
5. **The `00 1F` in `B0 14 11 00 1F 00 00 00`.** Plausibly a capability bitmask.
   Unexplained. Note the OEM announce `50 29 00 23 00 00 00 69` carries a trailing `0x69`
   in the slot where ours carries `0x00` — and `0x69` is the alive opcode. Coincidence or
   an embedded "and I am alive" is unknown.
6. **`0x64` on `0x1C1`** (`02 64 0F`, W2) — a cluster-to-radio single-frame command we have
   never sent or decoded.
7. **Other nodes on the sync-channel family** (W2), out of scope, recorded so they are not
   later mistaken for noise or for AFFA traffic: `0x3BF` (`49 00`, ~500 ms, alive opcode 9),
   `0x3FF` (`92 01`, ~500 ms), `0x2E8` (`91 00 00`, ~100 ms). The `9x` opcodes on `3FF`/`2E8`
   do not fit the speaker/opcode split of §5.1 and may belong to an unrelated protocol.
8. **`setState`: three encodings, one of them ours.** §2.4. The OEM sends `03 52 <state> 00`
   with `FF` filler; the archive sends `04 52 <state> FF FF`; we send `03 52 <state> FF FF`,
   which is self-inconsistent. All three are accepted. *Experiment: send the OEM form and
   confirm the panel still switches off.* Until then, ours is reproduced verbatim.

**Screens and formatting**

9. **`showMenu` declares length `0x5A` (90) but transmits 94 content bytes.** W4 asserts
   "len = total content byte count", which is contradicted by our own encoder — and our
   encoder is what the panel has been accepting for months. Either the panel ignores the
   declared length, or the last 4 bytes of item 1 are silently dropped (which would cap
   item 1 at 26 useful characters, not 30). *Experiment: put 30 distinguishable characters
   in item 1 and read the glass.* **Do not "fix" the length byte** before doing so.
10. **`setText` declares length `0x0E` (14) but transmits 20 content bytes** (6-byte header
   + 14 text bytes). `showPopupText`, built later from the OEM capture, computes
   `6 + textLen` correctly. If the panel honours the declared length it sees 8 text bytes,
   which would explain the docblock's "max 7 characters shown". Same experiment as 8.
11. **The exact popup / info-popup close command.** `02 54 03` closes the full window and
    the popup (W4, W7). `hideInfoPopup()` has no known command and currently sends
    `setText("RENAULT")` as a workaround. The `0x03` operand of `0x54` is unexplained;
    other values have never been swept.
12. **The icon bitmap is not fully decoded.** §3.1 reconciles `0x55` and `0x45` exactly and
    fails to reconcile `0x94`. Bit 7 has no name. W5's note that the codes "repeat
    cyclically across `0x00–0xFF`" means the sweep was never systematic. *Experiment: sweep
    `0x00`–`0x7F` with the `/preview` popup card and record the glyph set per bit.*
13. **The second `0x55` at header offset 2** is guessed to be a second icon bank (§3.4).
    Never varied.
14. **`sourceIcon` values beyond `0xFF`.** `0xDF` = "MANU" and `0xFD` = "PRESET" are
    recorded in a source comment with no capture behind them; "others show icons like LIST"
    is not a specification. Never swept.
15. **`showConfirmBoxWithOffsets` has no OEM witness at all.** Caption at tail offset `0x1A`
    (7 chars) and rows from tail `0x20` — six bytes apart for a seven-character field, so
    the caption's last byte and the row region's first byte are the same byte (§1.2). Either
    the caption is really 6 characters, or the row region really starts at `0x21`. It is
    reproduced because it worked, not because it is understood; a caption of exactly 7
    characters is the case nobody has tried. *Experiment: send a 7-character caption and a
    non-empty row 1 and read the glass.*
16. **Info-list row codes.** `0x41`/`0x44`/`0x48` are the three OEM values captured. The
    spacing (3, then 4) suggests a row-height or field-width encoding. The `showInfoMenu`
    source calls them "maybe itr constants". Unknown.
17. **`0x7E` as a text command** (W3) versus `0x76` (W6) — both accepted by a panel, effect
    difference not catalogued.

**Channels**

18. **`0x1F1` (Carminat NAV).** Registered on every cold link, never written to. W4
    reinterprets it as structured navigation state (turn direction + distance + street
    name), *not* a streamed bitmap, on the grounds that the idle "globe" glyph already
    lives in the panel. Its first frame is `11 2E` — a 302-byte message. The capture is
    frame-lossy because `esp32_can` hardcodes `rx_queue_len = 6` and the ~43-frame burst
    overflows it. Decoding the fields is future work; the library registers the id because
    the OEM does and stops there.
19. **Held rotary detents, and whether the hold mask is one bit or two.** `RollUp`/`RollDown`
    (`0x0101`/`0x0141`) are excluded from hold-masking by both ports, which implies the
    panel never sets the hold bits on them. No capture contains a held detent. Separately,
    W6 and both ports treat `0xC0` as a single mask while W10 §8.2 describes it as
    *"hold = bit 0x80/0x40"* — two bits. No capture shows one set without the other, so
    whether they encode two distinct states (long-press versus repeat, say) is unknown.
    *Experiment: hold each key on the bench and log the raw `<lo>` byte rather than the
    masked key code.*

**Encoding state**

20. **Icon updates on UpdateList are not implemented.** §2.2: the `0x7F` "text + icons"
    command exists in W6 and is never emitted by `UpdateListBase::setText`, which hardcodes
    `0x76`. So our AFFA2 driver can write text and cannot change icons. Nothing is broken —
    the capability was never ported. *Experiment: emit `10 1C 7F <icons> 55 <mode> <fmt>
    <loc> …` on `0x121` and watch the segment panel's icon row.* Until then the library
    must not advertise icon control on UpdateList.
21. **What resets the panel's latched icon state.** The `0x76`/`0x7F` differential encoding
    means the panel holds icons across messages, and the sender caches what it last sent.
    Nothing observed shows what invalidates the panel's copy: a resync, a display-control
    `0x52 00`/`0x52 <on>` cycle, and a power cycle are all plausible and untested. The
    library forces a `0x7F` after every resync because that is the cheap conservative
    choice, not because the panel is known to need it. *Experiment: set a non-default icon
    set, force a resync, send a `0x76`, and read the glass.*

---

## 7. Consequences for the library

Recorded here because each is a design decision that only makes sense once you have read
the sections above.

* **`SyncProfile` carries bytes, not behaviour.** §5.1 shows the opcode is invariant and
  only the speaker tag differs, so one state machine parameterised by four bytes covers
  both families. The two duplicated `tick()` implementations were duplicating the *same*
  algorithm with the *same* two defects.
* **`AFFA_PEER_TIMEOUT_MS`, not `SYNC_TIMEOUT`.** §4.2. A call counter is only a timeout
  when the caller's cadence is part of the contract, and it must not be. §5.3 adds the part
  that neither parent project spotted: the counter appeared to work only because `recv()`
  re-entered `tick()` on every peer ping, so the peer was re-arming the watchdog directly.
  The counter and the re-entrancy have to be removed in the same change.

**One divergence from legacy wire behaviour is deliberate, and it must not be "fixed" back.**
Everything else in this library reproduces the legacy bytes exactly. The heartbeat *rate*
does not. Legacy emits `0xB9`/`0x79` once per 1 Hz timer tick **plus** once per received
peer ping (§5.3), so a capture of MeganeCAN or MegaOpen shows roughly two heartbeats per
second, phase-locked to the panel. The library emits exactly one per
`AFFA_SYNC_INTERVAL_MS`, paced against `IClock::millis()` and independent of both the
caller's `poll()` frequency and the peer's ping rate — which is precisely what the
million-polls-in-a-simulated-second test asserts. If a future capture diff against legacy
flags "missing `B9` frames", this is why. The frame content is byte-identical; only the
cadence changed, and it changed on purpose.
* **The transmit path is a state machine.** §1: every frame needs an ACK, and the legacy
  code waited for it inside the only routine that could have delivered it. This is
  structural, not a tuning problem.
* **`kIconsNone` instead of `0x55`, `formatByte()` instead of a range table.** §3.1, §3.2.
  Both values are *computed* from named parts; shipping the hex is shipping the loss of
  that knowledge, which is exactly how this material came to need mining in the first
  place.
* **The menu belongs to the library.** §3.5: the scroll-arrow byte is computed from the
  sliding-window position, the row markers `0x7E`/`0x7F` are wire constants, and the
  highlight is a separate frame. None of that is separable from the protocol. W8 reached
  the same conclusion from the opposite direction — it lists `showMenu`/`highlightItem`
  under "panel driver: stays", and menu *content* and *navigation source* under the
  application.
* **Receivers must not assume DLC 8.** §5.2 item 4, and §6 item 3 is the live bug that
  assumption already caused.
* **One offset origin, stated at every declaration.** §1.2. Three origins are in circulation
  across the source material and two of them appear in one file. Every offset constant in
  `proto/` and `carminat/` is content-relative and says so in its comment; the porting rule
  is "convert once, at the point of transcription, never at the point of use".
* **Four builders change from fire-and-forget to acknowledged.** §2.5. `highlightItem`,
  `showInfoMenu`, `hidePopup` and `hideFullscreenText` bypassed the transport entirely in
  MeganeCAN. Routing them through `enqueue()` is correct and it makes them sync-gated and
  ACK-gated for the first time — the frames are byte-identical, the failure modes are new.
* **`RenderSlot::Popup` is independent of the slot behind it.** §2.5: the popup is a
  non-destructive overlay on the real panel. Coalescing a popup against the screen it covers
  would discard a frame the panel expects.
* **The icon header is differential and therefore stateful.** §2.2. The one piece of panel
  state the sender is required to track. It belongs to the display instance, must be
  invalidated on every resync, and must never be a function-local `static` the way the
  reference implementation had it.

---

## 8. AUX-source detection — an application writes this, not the library

**An application that wants to know which source the radio is playing implements the table
below itself, against a `subscribe()` on the panel's text id.** The library ships no such
feature and no decoded-text event: this is a heuristic about *someone else's product*, and
a library that asserts it as fact is lying on behalf of a radio it has never met.

This was `carminat/AuxModeTracker`, gated off by default behind `AFFA_ENABLE_AUX_TRACKER`.
The class is deleted — no test covered it, no example used it, nothing in the library
depended on it, and a default-off class that nobody switches on is dead weight with a
maintenance cost. The **patterns** are kept here because they cost real bench time to find
and throwing away working reverse-engineering would be the worse mistake.

### 8.1 Scope, and why it is narrow

These patterns describe **one Renault radio family** — the head unit that happened to be on
the bench bus (W1). They are not the panel, not the protocol, and quite possibly not your
car. Every verdict is a **guess**; none of it is a protocol fact.

### 8.2 The mechanism

Watch **`0x151` frames sent by the radio** (Carminat text channel; the UpdateList
equivalent is `0x121`, §9.6). Pair a `0x10` **first frame** with the `0x21` **continuation**
that follows it within **200 ms**, keep both, and classify.

Three things make this work on raw frames rather than on a reassembled string:

* The discriminator includes **header byte 6** — the `setText` format byte (§3.2), where
  `0x59`-`0x7F` is plain ASCII and `0x19`-`0x3F` is the radio-digit style. A reassembled
  string does not carry it. Two of the seven tests below need it.
* `text[0]` of the `0x21` frame is the **last byte of the header region, not text**. Every
  index below therefore starts at 1. Getting this wrong shifts every pattern by one and
  the classifier silently never matches.
* The 200 ms pairing window must be tracked with a **flag, not a zero timestamp**: at boot
  `millis()` is under 200, so a stale `0 + 200` window looks open.

### 8.3 The table

`text[]` is the `0x21` continuation frame's 8 bytes; `header[]` is the `0x10` frame's.

| # | Test | Verdict |
|---|---|---|
| 1 | `text[1..3] == "AUX"` | **AUX** |
| 2 | `text[1..7] == "RENAULT"` | radio (the idle banner) |
| 3 | `text[1..3] == "TR "`, `text[4]` space-or-digit, `text[5]` digit, `text[6] == ' '`, `text[7] == 'C'` | CD |
| 4 | `text[1..2] == "> "`, `text[3] != ' '`, `header[6] >= 0x59` | radio (short form) |
| 5 | `text[1] == 'M'`, `text[2] == ' '`, `text[3]` space-or-digit, `text[4..6]` digits, `text[7] == ' '` | radio (manual preset) |
| 6 | `text[1] == 'L'`, `text[2..3] == "  "`, `text[4..6]` digits, `text[7] == ' '` | radio (list preset) |
| 7 | `text[1..3] == "   "`, `text[4..7]` digits, `header[6] < 0x59` | radio (bare frequency) |
| — | none of the above | **retain the previous verdict** |

Tests are applied in order; the first match wins. Only #1 yields AUX — everything else is
evidence *against* it, which is why the fallback is "retain" and not "not AUX". A classifier
that flipped to `false` on an unrecognised screen would toggle the application's source
state every time the radio drew something this table has never seen.

`digit` means `c >= '0' && c <= '9'`, spelled out rather than taken from `<cctype>`:
`isdigit()` takes an `int` and is locale-dependent, and Arduino's `isDigit()` is neither of
those and does not exist off-target.

### 8.4 Worked example

`examples/08_radio_mitm` implements tests #1 and #2 this way in about twenty lines,
including the header pairing — enough to show the shape without pretending the remaining
five are universal. The full seven is a copy of this table into a `classify()` of your own.

---

## 9. The instrument-cluster variant — a THIRD sync profile  [CAP, single sample]

A capture supplied 2026-07-28 of an **OEM radio talking to a dashboard cluster** — not to
either panel this library drives. One sample only, direction annotations are the capture
owner's, and nothing here is bench-verified by us. It matters because it is a third point
on the family curve and it settles the shape of `SyncProfile`.

### 9.1 The sync bytes are a THIRD pair on an id we already know

    3AF 2  59 00        radio alive
    3AF 2  5A 01        radio sync request
    3CF 1  69           cluster peer-alive — DLC 1
    3BF 2  49 00        a fourth sync id, unmodelled

`0x3AF` is **Carminat's** sync id, but the bytes are neither Carminat's `B9`/`BA` nor
UpdateList's `79`/`7A`. The pattern holds though — `X9` alive, `XA` request — so this is a
third `(aliveByte, requestByte)` pair, `0x59`/`0x5A`, on a familiar id.

**This is the strongest evidence yet that `SyncProfile` was cut in the right place.** The
ids, the two sync bytes and the filler are all data; a third family needs a new profile
instance and not a line of new code. Note also `3CF` at **DLC 1** — the short-DLC case
`handleSyncFrame()` guards against, observed in the wild.

### 9.2 Registration is the same lazy 0x70 walk

    3AF 8  50 29 00 23 00 00 00 69     radio: init request (cf. our 70 1A 11 00 00 00 00 01)
    1C1 8  70 84 84 84 84 84 84 84     probe -> 5C1 74 ...
    121 8  70 FF FF FF FF FF FF FF     probe -> 521 74 ...
    1B1 8  70 FF FF FF FF FF FF FF     probe -> 5B1 74 ...

Three functions rather than two, probed with `0x70` and acknowledged on `id | 0x400` —
exactly the mechanism in §5. **The filler differs by SPEAKER, not by bus**: the cluster
pads `0x84`, the radio pads `0xFF`. That corroborates the note in `proto/ScreenDecode.h`
that filler is `0xA3` on our bench unit, `0x84` on an OEM cluster and `0xFF` on the OEM
radio — and is why nothing in this library ever matches on a received filler byte.

### 9.3 Display control carries a THIRD declared length

    1B1 8  03 52 00 00 FF FF FF FF

Command `0x52` again, but `SF_DL = 0x03` where UpdateList declares `0x04` for the same
shape (§9.3 of WIRE-SPEC) and Carminat declares `0x03` for its own. Three families, three
length bytes, all accepted by their own hardware. Do not unify them.

### 9.4 The clock command — `0x3EF`, and it is NOT an AFFA message

    3EF 3  A6 0C 03        annotated "Time", 12 hours 03 minutes

Three bytes at DLC 3: `A6 <hours> <minutes>`, both plain binary. **There is no PCI byte and
no SF_DL** — this is a raw frame, not the ISO-TP-ish transport everything else on this bus
uses. Anything sending it through a normal `enqueue()` will frame it and corrupt it.

That is interesting beyond the clock: `0x3EF` is evidence that the bus carries ordinary
signalling alongside the AFFA transport, and that `ICanLink::send()` — raw, unframed — is a
necessary escape hatch rather than an implementation detail.

**Why anyone cares:** UpdateList has no `setTime` of its own (`supports(Feature::Time)` is
false, §9 of WIRE-SPEC), and a head unit without a clock button has no other way to set it.
`examples/15_updatelist_modes` exposes this as `/api/time?h=&m=`, built on a raw
`ICanLink::send()`. **UNVERIFIED, and as of 2026-07-28 it did NOT set the clock** on the bench panel — the
frame is accepted by the bus and the clock does not change. `examples/15_updatelist_modes`
exposes it as `/api/time?h=&m=` and a `/api/sweep?n=1..8` of eight candidate encodings.

**THE ORACLE, which makes brute force practical.** These panels BLINK the clock while it is
unset and count up from power-on; a clock that has been set stops blinking. So you do not
have to watch the moment a command lands — a steady display means something worked, and a
blinking one means nothing has. The sweep exploits this by setting **hour == candidate
number**, so whatever a stopped clock reads names the encoding that won.

### 9.5 Two periodic ids we do not model

    2E8 3  91 00 00        ~10 Hz
    3FF 2  92 01           ~2 Hz

Unidentified. Listed so that a future capture has something to match against, and so nobody
assumes a quiet AFFA bus is an idle one.
