# DEVELOPING WITHOUT HARDWARE

Written for the specific situation this library came out of: **a Renault head unit on a
bench, no car, and often not even the panel yet.** Everything below runs without a vehicle,
and the first tier runs without any hardware at all.

* [Tier 1 — laptop only](#tier-1--laptop-only)
* [Tier 2 — a bare ESP32 devkit, no transceiver, no panel](#tier-2--a-bare-esp32-devkit-no-transceiver-no-panel)
* [Tier 3 — a real panel on a bench](#tier-3--a-real-panel-on-a-bench)
* [Capturing your own traffic](#capturing-your-own-traffic)
* [Comparing a capture against WIRE-SPEC.md](#comparing-a-capture-against-wire-specmd)
* [Adding a fourth panel](#adding-a-fourth-panel)

Prerequisite for all three tiers: PlatformIO. Every command below is run from the
repository root. On Windows the launcher is
`C:/Users/<you>/.platformio/penv/Scripts/pio.exe`; on Linux/macOS it is `pio` or
`~/.platformio/penv/bin/pio`. The examples use plain `pio`.

---

## Tier 1 — laptop only

**No board, no transceiver, no panel. This is where 90 % of the work happens.**

### 1.1 Run the test suite

```
pio test -e native
```

Expect **206 test cases across 13 suites in about fourteen seconds**. The suites are worth
knowing by name, because when you break something one of them tells you *what*:

| Suite | What it pins |
| --- | --- |
| `test_core` | ring, transliteration, heartbeat pacing, peer watchdog, registration, coalescing, key decode |
| `test_carminat_wire` | every Carminat builder, byte for byte, against the golden vectors |
| `test_updatelist_wire` | both UpdateList encodings, the `0x4A9` ACK, the AMS banner |
| `test_sync_profiles` | both handshakes through the real panel classes, including the short-DLC trap |
| `test_isotp_edges` | the 113-byte ceiling, "DONE while bytes remain", `fragment()` vs the transmit FSM |
| `test_keys` | the `03 89` guard, the hold mask, the wheel collision, unknown codes |
| `test_keysource` | `Local` / `Wire` / `Both`, and the three `fromSelf` drop points |
| `test_nav` | a full menu script through `CarminatDisplay`, decoded back with the independent screen decoder, asserting the FRAME COUNT of every step |
| `test_menu_widget` | `widget::MenuModel` alone, at `rows` = 2, 3 and 6, against a recording renderer — no panel, no CAN (`docs/MENU-WIDGET.md` §6) |
| `test_seam` | subscriptions, the tap, and every event kind |
| `test_latency` | key delivery in exactly one `poll()`, coalescing, preemption |
| `test_bench_surface` | the console's acceptance list, and a local wire decoder standing in for the deleted twin |
| `test_marquee` | `widget::Marquee` alone — the window as a function of the clock, at widths and step rates that are not this panel's |

Run one suite while you iterate:

```
pio test -e native -f test_carminat_wire
```

### 1.2 Run the whole library against a decoder

`src/vpanel/` used to hold **panel twins** — models of the panel that reassembled what the
library transmitted, answered the sync handshake and acknowledged frame by frame — and
`examples/07_virtual_panel` drove one on the host. Both are **deleted**: they were
application-shaped code shipped as library surface, and the two halves they welded together
are separately available and smaller.

```cpp
display.setSelfAck(true);                 // the ACK half
link.inject(affatest::panelSyncRequest()); // the handshake half
```

That is enough for the whole library to run to `registered()` on `LoopbackLink` with no
panel anywhere. To see what was *drawn* rather than what was sent, decode the transmitted
frames back — `AFFA_ENABLE_ISOTP_RX` buys exactly the two pieces you need:

```cpp
isotp::Reassembler asmb;                 // feed it every TX frame on the text id
if (asmb.onFrame(f) && asmb.len() >= 4)
  screen::windowText(asmb.buffer(), asmb.len(), model);   // or screen::menu(), infoRow()
```

`test_bench_surface` carries a thirty-line `decodeTx()` built from exactly that, and
`examples/90_bench_ota` carries the same thing as `BenchScreen` to drive `/api/screen`.
Copy whichever is closer to what you need.

> ### Self-ACK is the `Declared` rule, and that is the one that models a panel
>
> There are three imaginable ACK models and only one of them is a panel:
>
> | Model | Behaviour | Result |
> | --- | --- | --- |
> | DONE to everything | answers `0x74` to every frame | The transmit FSM correctly treats "DONE while bytes remain" as **success**, so a 96-byte screen terminates after frame 0 and a decoder has seen 8 bytes of it. |
> | PARTIAL to everything | answers `30 01 00` to every frame | The last frame is answered "keep going" with nothing left to send, and the sender reports `SendFailed`. |
> | **Declared** | `30 01 00` while the declared FF_DL is unsatisfied, `0x74` at it | **What the hardware does**, and what `setSelfAck(true)` implements. |
>
> The deleted twins made this a selectable `AckMode` and defaulted to the wrong one, which
> is why this box used to be a warning. There is nothing to select now.

The remaining trap is the opposite one and it is real: `setSelfAck()` terminates at the
length you *built*, not the length you *declared*, so `showMenu` is **14** frames under it
and **13** on hardware. Any golden vector that carries a bare `14` is wrong for one of the
two worlds. The suites parameterise it (`_HW` / `_EMU`) and so should yours.

### 1.3 Write a host test for your own change

The rig is `test/affa_test_support.h`, header-only, included as `"../affa_test_support.h"`.
It gives you `FakeClock`, `mk()` for building frames, `panelSyncRequest()` /
`panelPeerAlive()`, `drain()`, `expectFrame()` / `expectFrames()`, `pump()` /
`pumpUntilIdle()` and `ASSERT_RESULT`.

`expectFrames(link, vec, n)` asserts **exactly** `n` frames and then that the link is empty.
A prefix-only check would pass a 14-frame emulator transfer against a 13-frame hardware
vector, which is precisely the bug you are trying not to ship.

```cpp
#include <unity.h>
#include "../affa_test_support.h"

void test_my_thing(void) {
  affa::LoopbackLink<256> link;
  affatest::FakeClock     clk;
  affa::CarminatDisplay   d(link, clk);
  // ... bring the link up, drain, then assert the frames your change emits
}
```

Note the clock is normally left **frozen** in a wire test: nothing wants a heartbeat in the
middle of a golden vector.

---

## Tier 2 — a bare ESP32 devkit, no transceiver, no panel

**One ESP32-C3 (or ESP32) on a USB cable. Nothing else.** You get a browser console with a
live frame ring, the decoded panel screen, key injection, a working menu and latency
counters — with the board standing in for the panel it does not have.

### 2.1 Flash the bench console

`examples/90_bench_ota` already has `AFFA_ENABLE_ISOTP_RX=1` in its environment, which is
what `/api/screen` decodes with.

```
pio run -e ex90_bench_ota                 # build (≈898 kB, 1.4 MB OTA slot)
pio run -e ex90_bench_ota -t upload       # first flash, over USB
pio device monitor -e ex90_bench_ota      # watch it come up
```

After the first flash, later updates go over the network at `http://<ip>/update`
(ElegantOTA) — no cable needed.

### 2.2 Get on the network

The console reads WiFi credentials from NVS (**read-only**: namespace `megaopen`, keys
`ssid` / `pass`). If the station join fails within 15 s it raises a SoftAP:

* SSID **`AffaBench`**, password **`affabench`**
* then `http://192.168.4.1/`, or `http://affabench.local/` via mDNS

Network and HTTP come up **before** CAN and before the library, deliberately: everything
after that point may fail and still leave you a way in.

### 2.3 Switch to the virtual panel

```
curl "http://affabench.local/api/mode?panel=virtual"
```

or the button on the page. `real` and `virtual` are a **runtime flag on one `ICanLink`**
(`BenchLink`), not a second display and not a driver mode change:

* **real** — `send()`/`recv()` go to `Esp32CanLink`;
* **virtual** — `send()` accepts the frame and puts it nowhere; `setSelfAck(true)` supplies
  the per-frame ACK and `loop()` injects the two sync frames a panel would send.

In *both* modes the decoder is fed from the **Layer-0 tap**, not from `send()`. That is why
the same wiring serves two jobs: emulation with no transceiver, and **passive decode next to
a real panel** — so `/api/screen` answers with live hardware attached, and the decoder never
transmits a byte in either mode.

### 2.4 Drive it

```
curl "http://affabench.local/api/text?s=HELLO"
curl "http://affabench.local/api/menu"              # build/render the demo menu
curl "http://affabench.local/api/nav?c=next"        # next | prev | select | back | open
curl "http://affabench.local/api/key?k=0&hold=1&src=local"   # hold Load, locally
curl "http://affabench.local/api/key?k=257&src=wire"         # RollUp, onto the bus
curl "http://affabench.local/api/screen"            # the decoded glass
curl "http://affabench.local/api/status"            # sync, counters, latency
curl "http://affabench.local/api/frames?n=50"       # the live frame ring
curl "http://affabench.local/api/log?n=100"
```

`src=local` drives our menu and puts nothing on the bus; `src=wire` transmits
`03 89 <hi> <lo> 00 00 00 00` and has no local effect. Seeing those two side by side against
the frame ring is the clearest demonstration of the input seam there is.

`/api/status.lat` carries `keyToCbUs`, `keyToWireUs`, `pollMaxUs`, `staleDropped`
(completions with `Result::Aborted` — coalescing made visible) and
`ackN/ackMinUs/ackMeanUs/ackMaxUs`, the **panel's ACK turnaround**. Counters reset on a mode
switch so a virtual figure can never be mistaken for a hardware one.

### 2.5 What a virtual run does **not** prove

* Not that a real panel accepts your bytes. The decoder reads what our own encoder produced;
  agreement between two halves of one repository is not evidence about hardware.
* Not the pad bytes. We pad with the profile filler (`0x00` / `0x81`); a **real panel pads
  `0xA3`**. A golden vector recorded in virtual mode will differ from a capture there, and
  that is the emulation being an emulation. (Nothing in the library ever matches on a
  received filler — and nothing you write should either.)
* Not timing. Self-ACK answers instantly; a panel does not.

---

## Tier 3 — a real panel on a bench

### 3.1 Wiring

| Signal | ESP32-C3 pin | Notes |
| --- | --- | --- |
| CAN **RX** | `GPIO_NUM_3` | transceiver `CRX` / `RXD` / `R` |
| CAN **TX** | `GPIO_NUM_4` | transceiver `CTX` / `TXD` / `D` |
| `CANH` / `CANL` | — | to the panel |
| Bit rate | **500 000** | fixed by the car |
| Termination | 120 Ω | one at each physical end; on a short bench harness one resistor is usually right |

Use a **3.3 V** transceiver (SN65HVD230, TJA1051T-3). Give the panel its proper supply — it
is not a 3.3 V device — and share the ground.

### 3.2 The (rx, tx) trap

```cpp
g_link.begin(affa::CanPins{.rx = GPIO_NUM_3, .tx = GPIO_NUM_4}, 500000);   // this board
```

Swap those two and there is **no error at all**: no TX failure, no RX frame, no log line,
just silence. `CanPins` is a named struct for exactly this reason. The reference project
Earlier bench wiring used the mirrored assignment (`rx = GPIO_NUM_4, tx = GPIO_NUM_3`).
Those older logs are historical evidence only; the current bench rig uses the assignment above.

### 3.3 The panel opens the conversation

**Nothing happens until the panel pings.** Powering up your board first and seeing only a
1 Hz heartbeat is correct behaviour, not a fault. The order is:

1. panel announces itself: `3CF  61 11 …`
2. we answer with the hello burst on `0x3AF` (Carminat: **three** frames, the second and
   third byte-identical) or `0x3DF` (UpdateList: one)
3. we heartbeat at 1 Hz and answer the panel's `0x69` ping
4. only now does a payload trigger the lazy `70` registration probes, and only then does
   anything appear on the glass

So: power the panel, *then* look at your log. If you see heartbeats and nothing else, the
panel is not talking — check power, ground and the harness before you touch any code.

### 3.4 Flash `01_link_check` first

```
pio run -e ex01_link_check -t upload
pio device monitor -e ex01_link_check
```

It classifies and **names** the six known frames (`3CF 61 11`, `3CF 69`, `1C1 03 89`,
`1C1 70`, `|0x400 74`, `|0x400 30 01 00`), keeps a count and the age of the last one of each,
and reports every 2 s with our TX attempts and the controller's `txErr / txFailed / rxErr /
ringOverflow`:

```
RX 3CF 61 11 A3 A3 A3 A3 A3 A3  <panel hello>
---- sync=0x03 synced=1 registered=0 peerAcked=1 | ourTx=7 txErr=0 ...
```

**On a two-node bus, `txErr == 0` is the proof that the panel is acknowledging you** — a CAN
frame with no other node to ACK it fails at the controller. That single number tells you
whether you have a bus at all.

| Symptom | Most likely cause |
| --- | --- |
| nothing at all, `txErr` climbing | `rx`/`tx` swapped, or nothing else on the bus to ACK |
| `txErr == 0` but no RX | your TX is fine, the panel is not powered or not wired |
| RX frames but `synced=0` | wrong panel family selected (`AFFA_PANEL_*`) — you are answering the wrong sync id |
| `synced=1`, `registered=0` | expected: registration is lazy, and `01_link_check` never sends a payload |
| everything works, then `PeerLost` after a flash write | the TWAI ISR is not in IRAM; an OTA or NVS write stops reception. See `AFFA_PEER_TIMEOUT_MS`. |

### 3.5 Only now, a payload

`examples/02_carminat_text` is the smallest thing that puts text on the glass, and it prints
both verdicts side by side — acceptance from `setText()` and delivery from `onComplete()`.

---

## Capturing your own traffic

You want captures for two reasons: to add a panel, and to settle an argument about a byte.

**With this repository, no extra hardware.** `01_link_check` already prints every frame in
both directions in wire order:

```
pio device monitor -e ex01_link_check | tee capture.log
```

The format is `RX|TX <id> <bytes…>` plus a `<name>` for the frames it recognises. For a
denser trace of what the library itself is doing, build any example with
`-D AFFA_LOG_LEVEL=5`, or use the bench console's `/api/frames?n=200`, which returns the ring
as JSON with direction and timestamps.

**With a USB-CAN adapter** (candleLight / SocketCAN, on Linux):

```
sudo ip link set can0 up type can bitrate 500000
candump -tz -x can0 | tee capture.log            # -x shows TX vs RX
```

Two rules that will save you a day each:

1. **Capture the panel talking to its OEM radio, if you can get both.** A capture of *our*
   library talking to a panel proves only that the panel tolerated us. A capture of the OEM
   pair is the oracle — it is where every `[CAP]` tag in `WIRE-SPEC.md` comes from.
2. **Record what was on the bus, not just what you sent.** Filler bytes are per-node (our
   bench panel pads `0xA3`, an OEM cluster `0x84`, the OEM radio `0xFF`), so a capture
   without a note of which node emitted which frame is much less useful than it looks.

---

## Comparing a capture against WIRE-SPEC.md

`docs/WIRE-SPEC.md` is the byte-level oracle, and every claim in it carries the strongest
witness that attests it:

| Tag | Means | Trust |
| --- | --- | --- |
| `[CAP]` / `[CAP-VERBATIM]` | seen on a real bus, in the corpus | fact |
| `[REF]` | third-party implementation (`affa3.c/h`) agrees | strong |
| `[TWIN]` | our decoder round-trips it | consistency only |
| `[CODE]` | the extracted source did it this way | **a claim, not a fact** |

Procedure:

1. **Find the command.** `WIRE-SPEC.md` §8 is Carminat, §9 is UpdateList; each subsection
   gives the payload table, the frame arithmetic and the last PCI.
2. **Check the arithmetic before the bytes.** Frame 0 carries 8 payload bytes, each
   continuation 7, PCI `0x20 | (n & 0x0F)` — *wrapping*, so frame 16 is `0x20` and not
   `0x30`. `frames = 1 + ceil((L - 8) / 7)`.
3. **Expect a hardware transfer to be SHORTER than the builder's length.** A panel stops at
   the declared FF_DL (`payload[1]`), so `showMenu` declares `0x5A` = 90 content bytes and
   ends after 13 frames at PCI `0x2C`, even though 96 bytes were built. If your capture is
   13 and the document says 14, check which ACK model the document's vector was taken under
   before assuming anyone is wrong.
4. **Ignore the pad bytes.** Never match on, validate or assert a received filler.
5. **Do not "fix" an inconsistent declared length.** `showMenu` declares `0x5A` for 94 bytes
   built; `setText` declares `0x0E` for 20 transmitted; Carminat `setPower` is
   `03 52 <state> FF FF` while UpdateList's is `04 52 <state> FF FF`. These have rendered
   correctly for months. **Changing a length byte changes the glass.**
6. **If your capture disagrees with the document about a byte, the document loses only if
   your capture is `[CAP]`-grade** — i.e. taken from real hardware, with the emitting node
   known. Then: update `WIRE-SPEC.md`, add or amend the golden vector, and make a test fail
   before you make the code change.

Turn every settled disagreement into a vector in `test/`. The vectors are ordinary
`affa::Frame` arrays and paste straight in.

---

## Adding a fourth panel

The library is built so that a new panel family is **data plus one class**. Work in this
order — each step is testable before the next exists.

### Step 1 — the constants header

`src/<family>/<Family>Constants.h`, modelled on `src/carminat/CarminatConstants.h`. Tag
every value with its witness (`[CAP]` / `[OEM]` / `[REF]` / `[DERIVED]`). It must carry:

* the ids: sync, sync-reply, the function table **in registration order**, the key id;
* the packet filler byte for **our** transmissions;
* the hello frames, verbatim;
* command / header / row-tag bytes and field capacities.

### Step 2 — the `SyncProfile`

This is what lets one FSM in `core/` serve every family. Fill in:

`affa::SyncProfile` (`src/core/AffaSyncProfile.h`) is a plain aggregate, and it is the whole
of what differs between the two families' handshakes:

| Field | Carminat | UpdateList | What it is |
| --- | :---: | :---: | --- |
| `syncId` | `0x3AF` | `0x3DF` | the id **we** transmit sync on |
| `syncReplyId` | `0x3CF` | `0x3CF` | the id the panel answers on |
| `replyFlag` | `0x400` | `0x400` | ACK id = `funcId \| replyFlag`, always **computed**, never tabulated |
| `aliveByte` | `0xB9` | `0x79` | `data[0]` of our 1 Hz heartbeat |
| `requestByte` | `0xBA` | `0x7A` | `data[0]` of our sync request |
| `requestArg` | **`0x00`** | `0x01` | `data[1]` of the request. Carminat's `BA 00 00 …` is `0xBA` plus seven filler bytes that merely happen to be zero; UpdateList's `7A 01` is a genuine argument. **The symmetry is an illusion — do not unify them.** |
| `filler` | `0x00` | `0x81` | pads every frame we build. Note `data[1]` of the **heartbeat** is a literal `0x00` in both families and is *not* the filler. |
| `hello` / `helloCount` | 3 frames | 1 frame | the burst that answers the panel's `61 11`. Carminat's second and third frames are **byte-identical** — that is in the capture, not a typo. |

The function table is **not** in the profile: it is a constructor argument, because its
**order is on the wire** — it is the order the lazy `0x70` registration probes go out in.

```cpp
AffaDisplayBase(ICanLink&, IClock&, const SyncProfile&,
                const uint16_t* funcIds, uint8_t funcCount);
// Carminat   {0x151, 0x1F1}
// UpdateList {0x121, 0x1B1}
```

### Step 3 — the panel class

Derive from `AffaDisplayBase` and implement what `src/core/AffaDisplayBase.h` requires:

```cpp
uint8_t  packetFiller() const override;      // your filler
uint16_t keyTxId() const override;           // 0 if the family has none
bool     supports(Feature) const override;   // answer honestly; NotSupported is fine
bool     onFrame(const Frame&) override;     // frames the base did not consume
void     onPoll() override;                  // deadline-driven, never a call counter
```

Rules that are not style preferences:

* **Every render goes through `enqueue(funcId, data, len, TxOptions{slot, priority,
  coalesce})`** with the right `RenderSlot`. Never build a frame and call `_link.send()`
  directly for a render — that bypasses coalescing, the ACK FSM and the sync gate.
* **Gate the entire body of every `.cpp` on your `AFFA_PANEL_*` flag**, so the translation
  unit is empty when the panel is deselected. That is what makes an unused panel cost zero
  (verify with `nm` on a `size_min`-style build: zero defined symbols).
* **No `delay()`, no heap after `begin()`, no `static` state.** Every counter and deadline is
  a member.
* Add your flag to `src/AffaConfig.h`, your include to `src/AffaDisplay.h` (behind the gate
  and a `__has_include`), and an env to `platformio.ini`.

### Step 4 — the tests, copied not invented

Copy the shape, change the bytes:

| Copy | For |
| --- | --- |
| `test/test_updatelist_wire/` | your builders, byte for byte, against vectors from *your* capture |
| `test/test_sync_profiles/` | your handshake, the heartbeat pacing, the peer deadline — **and the short-DLC case**: inject `data[2] = 0x01` with `len = 2` and assert the start flag does *not* latch |
| `test/test_keys/` | only if your family has a key channel; assert the guard byte, the hold mask and that an unknown code arrives with its raw value |
| `test/test_isotp_edges/` | if your payloads reach the 113-byte ceiling |

Then, if you want the no-hardware loop for your family, write a `decode()` that reads *your*
payload layout on top of `isotp::Reassembler`, and drive it from the Layer-0 tap as
`test_bench_surface` and `examples/90_bench_ota` do — thirty lines, in your own test or
application, not in the library. With `setSelfAck(true)` your frame counts should fall out of
the arithmetic on their own; if they do not, either your decoder or the spec is wrong, and
finding out which is exactly what the exercise is for.

You would also implement `textRxId()` and `decodeText()` for your panel if you want
`onText()` to work — the base owns the reassembly and the completion rule, and your panel
owns nothing but the command byte and the offsets of the text within the payload.

### Step 5 — the documentation, in the same commit

A panel added without a `WIRE-SPEC.md` section is a panel nobody else can maintain. Write
the payload table, the frame arithmetic, the last PCI, and at least one golden vector per
command, each tagged with its witness. If a byte is `[DERIVED]`, say so — the next person
needs to know which claims to re-verify on their bench.
