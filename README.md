# AffaDisplay

> A non-blocking ESP32 driver for Renault AFFA2 / AFFA3 OEM dash panels over CAN /
> Неблокуючий драйвер штатних панелей Renault AFFA2 / AFFA3 для ESP32 через CAN

**[English](#english) · [Українська](#українська)**

MIT · ESP32 / ESP32-C3 · Arduino + PlatformIO · no heap after `begin()` · no `delay()` anywhere ·
259 host tests, no hardware required

---

## English

* [What it is and what it is not](#what-it-is-and-what-it-is-not)
* [Quick start](#quick-start)
* [Wiring](#wiring)
* [Supported panels](#supported-panels)
* [Capability matrix](#capability-matrix)
* [The menu is a widget, not the protocol](#the-menu-is-a-widget-not-the-protocol)
* [Configuration knobs](#configuration-knobs)
* [Footprint](#footprint)
* [Threading and the non blocking contract](#threading-and-the-non-blocking-contract)
* [Latency and preemption](#latency-and-preemption)
* [Key codes](#key-codes)
* [Developing without a car](#developing-without-a-car)
* [Keep ownership of the controller](#keep-ownership-of-the-controller)
* [Documents and tests](#documents-and-tests)

### What it is and what it is not

**It is** a complete, self-contained implementation of the *panel side* of the Renault
AFFA display protocol: the sync handshake, `0x70` function registration, ISO-TP framing,
the flow-controlled ACK state machine, key decoding and encoding, and every screen the panel
knows how to draw — text, clock, menu, popup, fullscreen, confirm box, info list.

It talks to two panel families:

* **Carminat / AFFA3** — the 3-row graphical display with the scroll wheel;
* **UpdateList / AFFA2** — the 8-segment display, and its mono-LCD variant.

Nothing in it sleeps, waits or allocates after `begin()`. The CAN seam is a **pull** port
(`recv(Frame&)`), every transmission is a **state machine advanced by `poll()`**, and every
periodic behaviour is a **wall-clock deadline** against an injected `IClock`. Those three
are structural answers to three defects that cost real bench time in the project this code
was extracted from: a watchdog that counted `poll()` *calls* instead of milliseconds, a
2000 ms blocking ACK wait sitting inside the only code path that could have delivered the
ACK, and a render queue in which a stale value could not be superseded.

**It is not:**

* **a radio emulator.** It drives a panel. Which text means which audio source, what a
  password prompt means, what a key should *do* — that is your application's business.
  See the boundary principle in `docs/API.md` §7b.
* **a CAN sniffer framework.** It exposes every frame it sees (Layer 0 tap, Layer 1
  filtered subscriptions), but it owns one controller under a strict contract and will not
  reconfigure it behind your back.
* **car-aware.** It knows nothing about your vehicle bus, your radio's model, or what else
  is listening on `0x151`. It will happily transmit into all of it if you let it.
* **a persistence layer.** No NVS, no preferences, no filesystem. What the user edits in a
  menu is yours to store.
* **thread-safe.** It is per-instance and unlocked, by design. Exactly one task calls
  `poll()`; see [Threading](#threading-and-the-non-blocking-contract).

### Quick start

```cpp
#include <AffaDisplay.h>

struct ArduinoClock final : affa::IClock {            // the whole IClock implementation
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

static void onKey(affa::Key k, affa::KeyEdge e, void*) {
  if (k == affa::Key::Pause && e == affa::KeyEdge::Click) g_display.setText("PAUSED", 0);
}

void setup() {
  // Named struct: the two pins cannot be swapped at the call site, and they have been.
  g_link.begin(affa::CanPins{.rx = GPIO_NUM_3, .tx = GPIO_NUM_4}, 500000);
  g_display.onKey(&onKey, nullptr);
  g_display.begin();                                  // we announce `BA` first; the panel answers
}

void loop() {
  g_display.poll();                                   // that is the whole integration
}
```

`setText()`, `showMenu()` and friends **enqueue and return**. Their `Result` says whether
the message was *accepted*, never whether the panel *displayed* it — that verdict arrives
later through `onComplete(cb, ctx)`, carrying the same `TxTicket` the call issued.

> ### ⚠️ Vehicle-bus session ownership
>
> Carminat/AFFA3 NAV opens with a bounded bare `3AF BA` announce from us; the display
> then answers on `0x3CF: 61 11 xx` and the session proceeds. Other panel profiles have their
> own session cadence. The library answers registration with `0x74` on `id | 0x400`; on a live
> vehicle bus, make sure no factory node owns the same role. Prefer a bench harness — see
> [Developing without a car](#developing-without-a-car).

Installation, `platformio.ini`:

```ini
lib_deps =
  https://github.com/andruxa/AffaDisplay.git

build_flags =
  -std=gnu++17
  -D AFFA_PANEL_CARMINAT=1
build_unflags =
  -std=gnu++11        ; the ESP32-C3 Arduino core still defaults to gnu++11
```

**The transport is `can_common` / `esp32_can`**, the stack most existing Renault/ESP32 code
already uses and the one proven end to end on the bench. `CanCommonLink` owns its RX ring and
driver lifecycle. A raw-TWAI seam existed once and was deleted for having no consumers: two
implementations of one interface, one of them untested by anything, is a place for the two to
disagree on a bus rather than in CI.

#### Building without a CAN driver at all

`-D AFFA_ENABLE_CANCOMMON_LINK=0` plus your own `ICanLink` is a supported configuration —
the interface is four methods and `link/LoopbackLink.h` is a worked example in ninety lines.
The gate removes the direct-TWAI link from the build; no `lib_ignore` or manifest surgery
is needed because the package has no external CAN dependency.

> ### Carminat/AFFA3 NAV session rule
>
> **Settled 2026-08-04 against four passive captures of a real OEM Renault radio driving a
> real Carminat panel.** Derivation: [`docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md`](docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md).
>
> **We speak first.** Into a silent bus the library announces one bounded bare `3AF BA` —
> no `B9` in front of it; that is the heartbeat, and it does not start until registration
> completes. The panel then requests on `0x3CF: 61 11 xx` (DLC ≥ 3) on its own ~104 ms timer. The
> first request only *arms* the announce; the panel's **next** request draws the three-frame
> burst `B0 14 11 00 1F 00 00 00` ×3, **30.75 ms** after it, 31 ms apart.
>
> **`61 11 00` and `61 11 01` are the same request.** The low bit reports the panel's own
> state, not an authorization grade. The panel's `1C1 70` lands between the first and second
> `B0`, and **we must answer `5C1 74` within ~0.5 ms, unconditionally, before any
> authorization has completed.** Registration (`151 70`, `1F1 70`) follows 0.1–0.3 ms after
> the third `B0` — it is part of the opening, not of the first render. Then 400 ms, then
> `151 03 52 09 …` (display ON) — always the first application payload, never a screen and
> never a clock. `3AF B9` is a **free-running 500 ms heartbeat, not a reply to the panel's
> `69`**, and it does not start until registration completes. `BA` is never periodic.
>
> **A registered panel never sends `61 11` at all**, so any complete `61 11 xx` arriving while
> the library holds registrations means the panel has voided the session: tear down and
> re-open, whatever the third byte says.
>
> <details><summary>What this box used to say, and what disproved it</summary>
>
> It read: *"The display, not the ESP32, starts the session … transmits **nothing** after
> `begin()` … Every such request receives the proven three-frame Carminat hello burst:
> `70 1A 11`, then `B0 14 11`, then the identical `B0 14 11` again. `61 11 01` is the
> **bootstrap** request … It does **not** authorize function registration, rendering, display
> power, text, or the clock. Only a later `61 11 00` authorizes those operations; the library
> then registers functions **sequentially**."*
>
> * **We are not silent** — in the co-boot capture the panel's first `61 11 00` arrives
>   **7.24 ms after our `BA`**, answering it.
> * **`70 1A 11` appears in zero of the 579 OEM frames.** The default burst is three
>   *identical* `B0 14 11 00 1F 00 00 00` frames; `70/B0/B0` is the opt-in legacy profile.
> * **`01` authorizes exactly as much as `00` does.**
>   `docs/captures/aknowledge offed display cONNECT OT POWER.csv` holds sixteen `61 11 01`
>   frames and zero `61 11 00`, and completes a full session on them. Waiting for a `00`
>   cost a bench session: the panel repeated `01` for fifteen seconds while the library
>   refused to answer.
> * **Registration is pipelined, not sequential** — `1F1 70` goes out 0.29 ms after
>   `151 70`, before either ACK returns.
> </details>

### Wiring

The bench board is an **ESP32-C3 SuperMini** plus a 3.3 V CAN transceiver
(SN65HVD230 / TJA1051T-3, *not* a 5 V TJA1050 without level shifting).

| Signal | ESP32-C3 pin | Notes |
| --- | --- | --- |
| CAN **RX** | `GPIO_NUM_3` | transceiver `CRX` / `RXD` / `R` |
| CAN **TX** | `GPIO_NUM_4` | transceiver `CTX` / `TXD` / `D` |
| `CANH` / `CANL` | — | to the panel harness |
| Bit rate | **500 000** | fixed by the car; not negotiable |
| Termination | 120 Ω | one at each physical end of the bus — with a panel plus your board on a short bench harness, one 120 Ω resistor is usually right; two if the harness is long |

> #### The (rx, tx) trap
>
> `CanPins` is a named struct precisely because these two get swapped, and the symptom is
> not an error — it is **silence**. No TX error, no RX frame, no log line, nothing:
>
> ```cpp
> g_link.begin(affa::CanPins{.rx = GPIO_NUM_3, .tx = GPIO_NUM_4}, 500000);   // this board
> ```
>
> The current rig now uses the same assignment as **MeganeCAN**. Older bench logs and images
> used the mirrored `rx = GPIO_NUM_4, tx = GPIO_NUM_3` wiring; they are historical evidence,
> not a wiring recipe. `examples/01_bringup` carries an explicit legacy override only for
> an older, differently soldered rig.

**Carminat/AFFA3 NAV opening is a two-sided exchange, and we move first.** The library
announces one bounded bare `3AF BA`, the panel replies on `0x3CF: 61 11 xx`, and
its *next* request draws the `B0` announce burst 31 ms later. A powered bench with no panel
therefore shows a lone `BA`, repeated every ~30 s, and then nothing further — that is correct behaviour, not
a fault. (This paragraph used to say the library transmits nothing at all until the panel
speaks; the OEM captures show our `BA` first, with the panel's request arriving 7.24 ms
after it.)

### Supported panels

| Family | Class | Sync id | Reply id | Function ids | Key id | Key ACK |
| --- | --- | --- | --- | --- | --- | --- |
| Carminat / AFFA3 | `affa::CarminatDisplay` | `0x3AF` | `0x3CF` | `0x151`, `0x1F1` | `0x1C1` | `0x5C1` |
| UpdateList / AFFA2 — **every glass** | `affa::UpdateListDisplay` | `0x3DF` | `0x3CF` | `0x121`, `0x1B1` | `0x0A9` | `0x4A9` |
| Instrument cluster — **unverified** | `affa::ClusterDisplay` | `0x3AF` | `0x3CF` | `0x151`, `0x1F1` | — | — |

The ACK id is always **computed** as `funcId | 0x400`, never tabulated. `0x0A9 | 0x400` is
`0x4A9` and not `0x5A9`, because bit 8 is already clear in `0x0A9` — uniquely in this
table. A hard-coded ACK id is a bug waiting for the UpdateList family.

Each family used to ship a **twin** — a model of the panel that reassembled what you
transmitted and ACKed the way hardware does. The twins are **deleted**: they were
application-shaped code living in the library. What they were used for is still there, in
two smaller pieces — `setSelfAck()` supplies the ACKs when no panel is attached, and
`test/affa_decode.h` holds the reassembler that reads the wire back, where it belongs.
See [Developing without a car](#developing-without-a-car).

### Capability matrix

Ask `display.supports(affa::Feature::X)` before you call; every unsupported call returns
`Result::NotSupported` rather than silently succeeding.

| Feature | Carminat | UpdateList | Cluster |
| --- | :---: | :---: | :---: |
| `Text` | yes | yes | **no** — the one capture has no text frame, so the encoding is unknown |
| `Time` | yes | no | no |
| `Power` | yes | yes | yes |
| `Menu` | **yes, unconditionally** | no | no |
| `Popup` | if `AFFA_ENABLE_POPUP` | no | no |
| `Fullscreen` | if `AFFA_ENABLE_FULLSCREEN` | no | no |
| `ConfirmBox` | if `AFFA_ENABLE_CONFIRMBOX` | no | no |
| `InfoPopup` | if `AFFA_ENABLE_INFOPOPUP` | no | no |
| `NavBitmap` | if `AFFA_ENABLE_NAV` | no | no |
| `KeyTx` | yes (`0x1C1`) | yes (`0x0A9`) | no |

**And ask how big it is, separately.** `panelGeometry()` reports rows, characters per row,
list capacity and image size, with **every field zero unless that surface exists**:

| | main chars | menu | info rows | list | image |
| --- | :---: | :---: | :---: | :---: | :---: |
| Carminat | 8 | 2 × 26 | 3 × 8 | 10 | 48 × 48 |
| UpdateList | 8 | — | — | — | — |
| Cluster | — | — | — | — | — |

UpdateList's 8 is a **promise, not a measurement**: the frame always carries a 12-cell field,
so a wider glass in that family shows more for free, but the radio cannot tell which glass
answered and eight is what every panel in it is known to render.

> **`Feature::RadioText` was removed.** It reported a *compile gate* — that a reassembler was
> built — and a capability query that answers a question about your own build tells you
> nothing about the glass. Inbound `0x121` from the radio is still decoded and reported
> through the protected `UpdateListBase::onRadioText(bool isAux)` hook; see
> `docs/PROTOCOL-NOTES.md` §8 for the pattern table.

### The library is a transport, not a UI

**There are no widgets in here.** Not optional ones, not off-by-default ones — none.

There used to be. `src/widget/` held a sliding-window menu state machine, a scrolling text
window and a three-row live screen, plus a Carminat adapter and a page/key controller,
behind `AFFA_ENABLE_MENU` and `AFFA_ENABLE_MARQUEE`. All of it is deleted, and the gates
with it.

The rule, owner's, 2026-08-08:

> Which item is selected, what a hold-`Load` gesture means, how fast a title scrolls and
> when to repaint are decisions about a **product**. A CAN driver that makes them is a CAN
> driver you cannot use for a different product.

What the panel actually defines is render calls, and they are **unconditional**:

```cpp
panel.showMenu(header, row0, row1, scrollByte);   // the 96-byte 0x21/0x01 screen
panel.showMenuN(buf, sizeof buf, header, items, n);
panel.highlightItem(rowTag);
panel.selectMenuItem(i);
panel.showInfoMenu(header, a, b, c);
panel.setText("HELLO");
```

Header, rows, which one is lit, which arrows. That is the whole of what is on the wire, and
none of it is behind a widget gate.

Everything above it is yours, and it is smaller than it sounds: an application that wants a
scrolling title calls `setText` with a different window every 400 ms — which is exactly what
`Marquee` did, except on the library's task, where it did not belong. `examples/17_mediascreen`
and `examples/18_aiscreen` each build their own screens on these calls, differently, which is
the point.

Ask the panel what will fit rather than assuming: `panelGeometry()` reports rows, characters
per row and image size, and **every field is zero unless that surface exists**.

### Configuration knobs

`src/AffaConfig.h` is the single knob header; every gate is documented there with what it
costs and what breaks. Set them in your own `build_flags` — the header only ever supplies
defaults.

| Macro | Default | What it controls |
| --- | :---: | --- |
| `AFFA_PANEL_CARMINAT` | `0`¹ | Carminat / AFFA3 panel |
| `AFFA_PANEL_UPDATELIST` | `0`¹ | UpdateList / AFFA2. **One encoding for every glass in the family** — there is no LCD variant flag any more, because there was never an LCD variant. |
| `AFFA_PANEL_CLUSTER` | `0` | the instrument cluster. **Never on by default, not even via `DEFAULT_ALL`**: everything it claims is inference from a single capture, and its opening cannot complete. |
| `AFFA_PANEL_DEFAULT_ALL` | `0`¹ | opt in to "compile Carminat + UpdateList". For a first look and the footprint reference builds. |
| `AFFA_ENABLE_POPUP` | `1` | `showPopupText` / `hidePopup` |
| `AFFA_ENABLE_FULLSCREEN` | `1` | `showFullscreenText` |
| `AFFA_ENABLE_CONFIRMBOX` | `1` | `showConfirmBox` (sits at exactly the 113-byte ceiling) |
| `AFFA_ENABLE_INFOPOPUP` | `1` | `showInfoPopup` (three messages) |
| `AFFA_ENABLE_BIGMENU` | `1` | `showMenuN` — the N-item list screen. Costs nothing at rest: the buffer belongs to the caller. |
| `AFFA_ENABLE_NAV` | `1` | `showNavBitmap` / `navTick` — the 48 × 48 pane. The image stays in the caller's flash. |
| `AFFA_ENABLE_TRANSLITERATION` | `1` | `toAscii` + its table (~1.2 kB). **0 is dangerous**: UTF-8 then reaches the wire unchanged and renders as garbage — a visual failure, not a compile error. |
| `AFFA_ENABLE_LOG` | `1` | the `AFFA_LOG*` macros. 0: no format strings enter flash at all, so never put a side effect in a log argument. |
| `AFFA_LOG_LEVEL` | `3` | 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace. Compile-time. |
| `AFFA_ENABLE_CANCOMMON_LINK` | `1` on Arduino, `0` on host | `CanCommonLink`, the transport over `can_common` / `esp32_can` |
| `AFFA_ENABLE_TASK` | `0` | the library owns the poll task. With it on, **every render is callable from any task** — see below. `#error` off ESP-IDF / Arduino-ESP32. |
| `AFFA_DISPATCH_DEPTH` | `8` | cross-task dispatch slots. Power of two; `0` removes the ring. Deeper than `AFFA_TX_QUEUE_DEPTH` only defers `QueueFull` to a worse place. |
| `AFFA_TX_COALESCE` | `1` | latest-value-wins per `RenderSlot`. 0 reproduces the "panel keeps counting after Pause" defect. |
| `AFFA_TX_QUEUE_DEPTH` | `6` | queue slots, `~AFFA_MAX_PAYLOAD + 12` B each. 6 and not 4 because `showInfoPopup` is three messages and the first call after a resync also carries two registration probes. |
| `AFFA_MAX_PAYLOAD` | `113` | **a wire limit, not a budget**: `8 + 15×7 = 113`, the point at which the ISO-TP counter would wrap. Below 96 the Carminat menu returns `TooLong`. |
| `AFFA_RX_RING_DEPTH` | `32` | power of two. 32 × `sizeof(Frame)` = 448 B; tolerates a ~7 ms gap between `poll()` calls on a saturated bus. |
| `AFFA_ACK_TIMEOUT_MS` | `2000` | per-frame ACK deadline; matches the legacy blocking wait exactly |
| `AFFA_PEER_TIMEOUT_MS` | `5000` | silence before sync is torn down. **Effective window is up to this + `AFFA_SYNC_INTERVAL_MS`**, because the watchdog is evaluated on a heartbeat tick. Never lower it below your longest flash write — the TWAI ISR is not in IRAM, so an OTA or NVS write looks exactly like a panel that went quiet. |
| `AFFA_SYNC_INTERVAL_MS` | `1000` | heartbeat cadence. Treat as fixed: it is what the capture shows. |
| `AFFA_TEXT_MAX` | `64` | text buffer |

¹ **Naming no panel is a compile error, not a default.** The panel flags default to `0`, and
`AffaConfig.h` `#error`s when every one of them is `0` — which is also the state a misspelled
`-D AFFA_PANEL_CARMINET=1` leaves behind, and the only way that typo can be caught
(`-Wundef` cannot see it: the misspelled macro *is* defined, merely never read).

**Gates that used to be here and are gone.** `AFFA_PANEL_UPDATELIST_MENU`,
`AFFA_ENABLE_MENU`, `AFFA_ENABLE_MARQUEE`, `AFFA_ENABLE_ISOTP_RX`,
`AFFA_ENABLE_ESP32CAN_LINK`, `AFFA_MAX_SUBSCRIPTIONS`, `AFFA_TASK_QUEUE_DEPTH`,
`AFFA_MENU_MAX_ITEMS`, `AFFA_MENU_MAX_FIELDS`, `AFFA_MENU_ROW_MAX`. Each one is still
named in `AffaConfig.h` with a paragraph on what it did and why it went — a knob that sizes
a structure the library no longer has is worse than no knob, because someone tunes it,
nothing changes, and they go looking for the bug somewhere real. `docs/API.md` §7 is the
full account.

**Every `Result`-returning call is `[[nodiscard]]`.** A render whose `Result` you drop is a
screen that silently never appears — `NoSync`, `QueueFull`, `TooLong` and `NotSupported` all
look identical to success from the call site. Ignore one deliberately and say so:
`(void)display.setText("RENAULT", 0);`.

### Footprint

ESP32-C3 (`board = esp32-c3-devkitm-1`, Arduino core 2.0.17), release build. **Measured
2026-08-08 with `pio run -c platformio_footprint.ini`** — every number below came out of
that run, not out of the last one.

That distinction is the point. The table this replaces quoted a menu widget, an ISO-TP
gate, a subscription table and an `Esp32CanLink` that had all been deleted; the harness that
produced it no longer compiled, so nothing was ever going to correct it. It compiles now,
and re-running it is one command.

**Baselines, same toolchain:**

| Baseline | Flash | RAM |
| --- | ---: | ---: |
| empty `setup()`/`loop()` sketch | 218 912 B | 13 476 B |
| …plus `can_common` + `esp32_can`, no AffaDisplay | 257 724 B | 14 564 B |

**Reference build** — `g_base`: Carminat + UpdateList, `CanCommonLink`, owned task, every
feature gate on, and a probe that calls **every** optional render so `--gc-sections` cannot
remove what a gate is supposed to remove:

| | Flash | RAM |
| --- | ---: | ---: |
| `g_base` | 284 036 B | 22 140 B |
| Δ vs the CAN baseline | **+26 312 B** | **+7 576 B** |

#### What each gate is actually worth

Measured by flipping exactly one flag against `g_base`.

| Change | Flash | RAM | |
| --- | ---: | ---: | --- |
| `AFFA_ENABLE_CANCOMMON_LINK=0` | **−11 256 B** | −1 888 B | by far the largest — it is an external library, not our code |
| `AFFA_PANEL_CARMINAT=0` | −4 980 B | −2 704 B | the big family: menus, popups, confirm boxes, the nav pane |
| `AFFA_ENABLE_LOG=0` | −2 788 B | −16 B | mostly format strings |
| `AFFA_ENABLE_TRANSLITERATION=0` | −2 178 B | 0 | the mapping table. **Do not**, unless every string you ever pass is already ASCII |
| `AFFA_PANEL_UPDATELIST=0` | −1 430 B | −2 528 B | |
| `AFFA_ENABLE_BIGMENU=0` | −818 B | −192 B | `showMenuN` + `selectMenuItem` |
| `AFFA_ENABLE_NAV=0` | −806 B | 0 | `showNavBitmap` + `navTick` |
| `AFFA_ENABLE_CONFIRMBOX=0` | −558 B | 0 | |
| `AFFA_ENABLE_TASK=0` | −308 B | −144 B | see the note below — this is not the cost of the task |
| `AFFA_ENABLE_INFOPOPUP=0` | −278 B | 0 | |
| `AFFA_ENABLE_FULLSCREEN=0` | −256 B | 0 | |
| `AFFA_ENABLE_POPUP=0` | −204 B | 0 | |
| `AFFA_PANEL_CLUSTER=1` | **+414 B** | **+2 528 B** | the third family, added rather than removed |

**Two of these numbers were bugs before they were numbers**, and the harness is how they
surfaced:

* **`-D AFFA_ENABLE_FULLSCREEN=0` did not link.** The `#if AFFA_ENABLE_FULLSCREEN` block in
  `CarminatDisplay.cpp` had grown to enclose the `BIGMENU` and `NAV` blocks, so turning
  fullscreen off silently removed the *definitions* of `selectMenuItem`, `showMenuN`,
  `showNavBitmap`, `showNavBitmapWithHeader` and `navTick` while the header went on
  declaring them. Five undefined references, from a gate that was asked to remove one
  function.
* **The library only compiled because every env in this repository defined a macro the
  library had deleted.** `CarminatDisplay::supports()` still read `AFFA_ENABLE_MENU != 0` —
  a *C++ expression*, not a preprocessor test, so an undefined macro is a hard error rather
  than a warning — and `platformio.ini` was still passing `-D AFFA_ENABLE_MENU=0` to all
  four example envs. **Any consumer who did not pass that flag got a compile error on
  `supports()`.** `Feature::Menu` returns `true` unconditionally now, because `showMenu` and
  `highlightItem` are protocol and no gate removes them.

**Reading the small numbers honestly.**

* **`AFFA_ENABLE_TASK=0` measuring −308 B does not mean the owned task is free.** The probe
  declares an `AffaTask` but never `start()`s it, so most of `rtos/AffaTask.cpp` is collected
  away. −308 B is what a build pays for *having the type available*. A build that actually
  starts the task also pays `AFFA_TASK_STACK` (4 096 B by default) out of heap, which no
  static measurement can see.
* **An unselected panel costs zero, and that is a mechanism rather than a hope.** Each
  optional `.cpp` gates its whole body, so it compiles to an empty object file — the
  preprocessor is the only thing that can remove a translation unit under PlatformIO's
  Library Dependency Finder, because a consumer's `build_src_filter` cannot reach into a
  `lib_deps` library.
* **A feature gate's price is only visible once something calls it.** In a build that never
  touches `showNavBitmap`, `--gc-sections` has already removed it and
  `AFFA_ENABLE_NAV=0` measures nothing. The probe exists precisely to defeat that, which is
  why these numbers are larger than what you will see in your own application.
* `g_neg_typo` still fails to compile, which is the point of it: a misspelled
  `-D AFFA_PANEL_CARMINET=1` leaves every real panel macro at `0` and the `#error` is the
  only thing that can catch it.

### Threading and the non blocking contract

* **No `delay()`, no busy-wait, and no `vTaskDelay()` in `core/`, `util/`, `link/`
  or any panel.** `IClock` exposes `millis()` and deliberately nothing
  else. If something on a data path in this library wanted to sleep, its state machine would
  be wrong. **The single exception, added in 0.3.0, is `src/rtos/AffaTask.cpp`**: the owned
  task's own `vTaskDelayUntil` between iterations, which is what a task period *is*. It is
  one call, in the one directory that requires FreeRTOS, and it sleeps the library's task —
  never yours, and never a data path.
* **No heap after `begin()`.** Every buffer is static and sized by a macro in
  `AffaConfig.h`. No `String`, no `std::vector`, no `std::function` in the core.
* **No file-scope or function-local state.** Every counter, deadline and buffer is a member,
  so two instances on two buses cannot interfere. (The extracted code had a file-scope event
  queue, a static log timestamp and a `static int8_t timeout`; in a library those are shared
  state between instances.)
* **Exactly one task calls `poll()`.** The library is per-instance and **unlocked** — that
  is a deliberate choice, not an omission, and it is what keeps `poll()` free of critical
  sections. **That is the rule for `AFFA_ENABLE_TASK=0` only.** With the owned task on — the default on
  ESP32 since 2.0 — any context may render directly and no mailbox is needed; see below.
* **Callbacks fire from the `poll()` context**, never from the CAN driver task. State is
  committed *before* the callback that reports it, so a callback may call back into the
  library — render calls, `abortPending()`, `pressKey()` — but never `poll()`
  itself.
* **Pointers handed to a callback are valid only for its duration.** They point at
  library-internal storage. Copy what you keep.
* **The frames we emit are frequency-independent; whether a transfer completes is not.**
  Calling `poll()` once per second and a million times per second produce the same frames in
  the same order with the same timing — nothing counts calls. But `AFFA_ACK_TIMEOUT_MS` and
  `AFFA_PEER_TIMEOUT_MS` are wall-clock deadlines evaluated *inside* `poll()`, so a late
  `poll()` does not delay a result, it **changes** it: `Ok` becomes `Timeout`, and an expired
  peer deadline tears down registration. Earlier revisions said "no minimum rate for
  correctness" without that second half, and it was read as licence to share the poll task.
  (docs/API.md §4.4.)
* **Since 2.0 the library owns the task by default on ESP32, and that reversal is the point
  of the refactor.** `AFFA_ENABLE_TASK` defaults to `1` there: `src/rtos/` compiles, a 2 ms
  task at priority 2 is created, and **every render is callable from any task — all of them,
  including ones written after this sentence.** No mutex, no mailbox, no hand-off, and
  nothing to transcribe.

  It used to default to `0`, on an argument that was correct at the time: turning it on
  changes which task a consumer's callbacks run on. What that produced is countable —
  **thirteen of nineteen shipped examples turned it off** and pumped `poll()` from `loop()`,
  including the one whose HTTP handlers then raced the queue.

  The reason they turned it off is gone. Until 2.0 the task published its own render surface
  covering ten of twenty-two calls, so anything richer forced the application back onto the
  raw display — at which point the task was an extra object with a second vocabulary. **The
  display is the thread-safe surface now**, whichever task calls it, and the task costs a
  consumer nothing but the two lines that start it.

  `KeyCb` still fires synchronously inside `poll()`, so key latency is unchanged: bounded by
  the task period and nothing else. `poll()` refuses a caller that is not the owning task and
  counts it. `examples/17_mediascreen` is the reference — forty concurrent HTTP renders
  against a real panel, every counter zero; `docs/API.md` §4.7 is the mechanism and §4b is
  the contract. `core/` and `util/` are untouched by it and still compile on the host against
  nothing but C++17.
* **There is no exception.** Earlier revisions offered one, `sendBlocking(ticket,
  timeoutMs)`, which spun on `poll()` until a ticket completed. It is gone: nothing in
  `src/`, `examples/` or `test/` ever called it, and a library whose headline promise is
  that it never blocks should not ship the one call that does. Wait on `onComplete()` from
  your own loop.

### Latency and preemption

The guarantee, pinned by `test_latency` as a **poll count** rather than a wall-clock claim:

> **A key reaches your callback in exactly one `poll()`** — with an empty queue, and equally
> with a 96-byte `showMenu` in flight, `WaitAck` holding 1900 ms of its 2000 ms deadline, and
> every transmit slot occupied.

That falls out of the ordering inside `poll()`: drain RX and deliver keys **strictly
before** pumping the transmit FSM. The TX FSM checks a deadline and returns; it never waits.

* **Latest value wins, per `RenderSlot`.** A repeated render occupies exactly one queue slot
  no matter the render rate and always holds the newest value; superseded tickets complete
  `Result::Aborted`. Three `setText`s queued behind an in-flight menu become one message
  carrying the third string.
* **Different slots never coalesce against each other** — a clock update cannot eat a popup.
* **`abortPending()`** drops everything queued but *not yet started*, reporting `Aborted`
  once per ticket, in order. **`Priority::Urgent`** jumps the queue but never the
  registration probes.
* **A message on the wire is never split.** `Urgent` and `abortAll()` take effect at a frame
  boundary only, and the ISO-TP continuation counter resets when a job is abandoned, so it
  cannot corrupt the next message.
* **Self-sent frames are inert.** Every transmitted frame is tagged `Frame::fromSelf` and
  dropped before the auto-ACK, before the ACK matcher **and** before the key decoder. A real
  controller does not echo its own frames; `LoopbackLink` can. Behaviour is identical on
  both, which is what makes the host tests worth anything.

Without this, a 10 Hz counter rendered in front of a 13-frame menu transfer leaves a backlog
of stale values: the panel visibly keeps counting for a second *after* the user pressed
Pause and after the library correctly received the key. It reads as a key-handling bug and
it is a queueing bug. `examples/06_counter_preempt` measures it.

### Key codes

The joystick is physically part of the **panel**: pressing it makes the panel encode and
transmit a key frame, which the radio receives. **This library's normal role is the radio**,
so keys only ever come *in*, and `pressKey()` / `nav()` default to `KeySource::Local`.

Wire frame, on `0x1C1` (Carminat) or `0x0A9` (UpdateList):

```
03 89 <code>>8> <code&0xFF | (hold ? 0xC0 : 0)> <filler × 4>
```

| `affa::Key` | Code | Notes |
| --- | :---: | --- |
| `Load` | `0x0000` | the button at the bottom of the stalk; hold-`Load` is the default menu gesture |
| `SrcNext` | `0x0001` | |
| `SrcPrev` | `0x0002` | |
| `VolUp` | `0x0003` | |
| `VolDown` | `0x0004` | |
| `Pause` | `0x0005` | |
| `RollUp` | `0x0101` | wheel, one detent up |
| `RollDown` | `0x0141` | wheel, one detent down |

Four things about this table are load-bearing:

1. **The `03 89` guard is not optional.** The same key id also carries `70 A3..`,
   `02 64 0F A3..` and `05 63 "0037"`. A decoder without the guard invents keys `0x640F`
   and `0x3030` out of ordinary traffic.
2. **Held wheel detents are unrecoverable by design.** `0x0101 | 0xC0` and `0x0141 | 0xC0`
   are *both* `0x01C1`, because `0x40` is simultaneously RollDown's direction bit and half
   the hold mask. They decode as `RollUp` + hold, and the encoder refuses to transmit either
   — a hold edge on the wheel has no wire representation at all. This is why
   `NavCommand::Increase` / `Decrease` are reachable only with `KeySource::Local`.
3. **The enum is open.** These eight names are `[REF]`-attested, but nothing establishes the
   list is *complete*. An unrecognised code is delivered as `static_cast<Key>(raw & 0xFF3F)`
   — a `Key` carrying the raw wire code — and never dropped. Always write a `default:` in a
   switch over `Key`.
4. **`KeySource::Wire` puts phantom presses on the bus.** Harmless on a bench; input other
   modules may act on in a car.

### Developing without a car

Three tiers, none of which needs a vehicle. The full walkthrough with copy-pasteable
commands is **[`docs/DEVELOPING-WITHOUT-HARDWARE.md`](docs/DEVELOPING-WITHOUT-HARDWARE.md)**.

1. **Laptop only — no board at all.**
   ```
   pio test -e native            # 259 cases
   ```
   The whole library runs on the host over `LoopbackLink`, with `setSelfAck(true)` supplying
   the per-frame ACK a panel would and one injected `61 11` completing the handshake. To
   assert on what was *drawn* rather than what was sent, decode the transmitted frames back
   through `isotp::Reassembler` + `affa::screen` — `test_bench_surface` carries thirty lines
   that do exactly that, and it is the pattern to copy.

   Self-ACK is the **Declared** rule — PARTIAL while the declared FF_DL is unsatisfied, DONE
   at it — which is what the hardware does and what reproduces every frame count in the wire
   spec (`showMenu` = 13 frames, last PCI `0x2C`) without being told them.
2. **A bare ESP32 devkit — no transceiver, no panel.** Flash `examples/90_bench_ota`, open
   the web console, switch it to `panel=virtual`. The decoder is fed from the Layer-0 tap,
   so the same wiring serves both a virtual panel and a passive decode alongside a real one.
   You get the live frame ring, the decoded glass, key
   injection and the latency counters in a browser.
3. **A real panel on a bench.** Wiring as above, 500 kbit/s, mind the `(rx, tx)` trap, and
   remember that **we open the conversation**: the library announces a bare `BA` into a
   silent bus every ~30 s, and the panel answers on `0x3CF: 61 11 xx`. Flash
   `examples/01_bringup` first — it proves the link in the order it has to be proved, and on
   a two-node bus `txErr == 0` is the proof the panel is acknowledging you.

The same document also covers capturing your own traffic, diffing it against
`docs/WIRE-SPEC.md`, and adding a fourth panel family.

### Keep ownership of the controller

`CanCommonLink` owns one CAN controller and its RX ring. Do not drive that controller from
application code — no `esp32_can` `begin()`/`watchFor()`/`sendFrame()` of your own, and no
ESP-IDF `twai_*` calls behind its back. Use `setTxEnabled()` for
quiet periods and `setListenOnly()` only through the link; recovery is driven by the
library poll backoff.

`send()` is non-blocking: accepted means queued, not that the display processed the frame.
The display's protocol `0x74` reply is delivery proof; controller TX counters are
diagnostic only.

### Documents and tests

```
pio test -e native      # 197 host test cases across 15 suites, no hardware
pio run                 # 5 environments: one host, four ESP32 examples
```

| Document | What it is |
| --- | --- |
| [`docs/API.md`](docs/API.md) | The contracts the implementation is written against — threading, `Result`, latency, capabilities. **It does not copy declarations**: the headers are the declarations, and §7 says where every deleted thing went. |
| [`docs/WIRE-SPEC.md`](docs/WIRE-SPEC.md) | The byte-level oracle: every frame layout, ready-to-paste golden vectors each tagged with the strongest witness that attests it, and the arithmetic for every frame count. **Where the code and this document disagree about a byte, the code is wrong.** |
| [`docs/PROTOCOL-NOTES.md`](docs/PROTOCOL-NOTES.md) | Provenance: every byte traced to a capture, an OEM log or a third-party reference, plus the open questions each phrased as the experiment that closes it. |
| [`docs/REFACTOR-2.0.md`](docs/REFACTOR-2.0.md) | Why the surface has the shape it now has: the evidence, the root cause, and what was deleted to fix it. |
| [`docs/ESP32CAN-CONTRACT.md`](docs/ESP32CAN-CONTRACT.md) | Driver ownership, RX/TX, lifecycle, and recovery. |
| [`docs/PORTING.md`](docs/PORTING.md) | Moving an application off the old classes — and how to drop this library entirely, including which files are panel-specific and which are the reusable transport core. |
| [`docs/DEVELOPING-WITHOUT-HARDWARE.md`](docs/DEVELOPING-WITHOUT-HARDWARE.md) | The three tiers above, in full, plus capturing traffic and adding a panel. |
| [`docs/BENCH-VERIFIED.md`](docs/BENCH-VERIFIED.md) | What has actually been seen on a panel, as opposed to what is believed. |

`core/`, `util/` and `link/LoopbackLink.h` must all compile for `platform = native` with
nothing but the C++17 standard library. If a change breaks that build, the change is wrong,
not the test. `<driver/twai.h>` appears nowhere in the library: the driver is reached
through `can_common`.


## Українська

* [Що це таке і чим воно не є](#що-це-таке-і-чим-воно-не-є)
* [Швидкий старт](#швидкий-старт)
* [Підключення](#підключення)
* [Підтримувані панелі](#підтримувані-панелі)
* [Матриця можливостей](#матриця-можливостей)
* [Меню — це віджет, а не протокол](#меню--це-віджет-а-не-протокол)
* [Перемикачі конфігурації](#перемикачі-конфігурації)
* [Обсяг прошивки](#обсяг-прошивки)
* [Багатозадачність і неблокуючий контракт](#багатозадачність-і-неблокуючий-контракт)
* [Затримка і витіснення](#затримка-і-витіснення)
* [Коди кнопок](#коди-кнопок)
* [Розробка без автомобіля](#розробка-без-автомобіля)
* [Власник контролера](#власник-контролера)
* [Документи і тести](#документи-і-тести)

### Що це таке і чим воно не є

**Це** повна самодостатня реалізація *панельного боку* протоколу Renault AFFA: sync
handshake, реєстрація функцій через `0x70`, ISO-TP фрагментація, автомат станів для ACK із
керуванням потоком, декодування і кодування кнопок, і всі екрани, які панель уміє малювати — текст,
годинник, меню, popup, повноекранний текст, вікно підтвердження, список інформації.

Підтримуються дві родини панелей:

* **Carminat / AFFA3** — трирядковий графічний дисплей із коліщатком;
* **UpdateList / AFFA2** — восьмисегментний дисплей і його моно-LCD різновид.

Ніщо тут не спить, не чекає і не виділяє пам'ять після `begin()`. Шов до CAN — це **pull**
порт (`recv(Frame&)`), кожна передача — це **автомат станів, який рухає `poll()`**, а кожна
періодична дія — це **дедлайн за реальним часом** відносно впровадженого `IClock`. Усі три
рішення структурно закривають три дефекти, які коштували реального часу на столі в проєкті,
звідки цей код видобуто: watchdog, що рахував *виклики* `poll()` замість мілісекунд;
блокуюче очікування ACK на 2000 мс усередині єдиного шляху, який міг би цей ACK доставити; і
черга рендерів, у якій застаріле значення не можна було замінити свіжим.

**Чим воно не є:**

* **не емулятор радіо.** Воно керує панеллю. Який текст означає яке джерело звуку, що робити
  із запитом пароля, що саме має *робити* кнопка — це справа вашого застосунку. Дивіться
  принцип межі в `docs/API.md` §7b.
* **не фреймворк для сніфінгу CAN.** Воно віддає кожен кадр, який бачить (Layer 0 tap,
  Layer 1 підписки з фільтром), але володіє одним контролером за суворим контрактом і не
  переналаштує його у вас за спиною.
* **не знає про автомобіль.** Йому нічого не відомо про вашу шину, модель вашого радіо чи про
  те, хто ще слухає `0x151`. І воно радо передаватиме в усе це, якщо ви дозволите.
* **не шар зберігання.** Ніякого NVS, preferences чи файлової системи. Те, що користувач
  змінив у меню, зберігаєте ви.
* **не потокобезпечне.** Воно на екземпляр і без локів — навмисно. Рівно одна задача викликає
  `poll()`; див. [Багатозадачність](#багатозадачність-і-неблокуючий-контракт).

### Швидкий старт

```cpp
#include <AffaDisplay.h>

struct ArduinoClock final : affa::IClock {            // уся реалізація IClock
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

static void onKey(affa::Key k, affa::KeyEdge e, void*) {
  if (k == affa::Key::Pause && e == affa::KeyEdge::Click) g_display.setText("PAUSED", 0);
}

void setup() {
  // Іменована структура: два піни неможливо переплутати на місці виклику, а їх плутали.
  g_link.begin(affa::CanPins{.rx = GPIO_NUM_3, .tx = GPIO_NUM_4}, 500000);
  g_display.onKey(&onKey, nullptr);
  g_display.begin();                                  // ми першими оголошуємо `BA`; панель відповідає
}

void loop() {
  g_display.poll();                                   // це вся інтеграція
}
```

`setText()`, `showMenu()` та інші **ставлять у чергу і повертаються**. Їхній `Result` каже,
чи повідомлення *прийнято*, і ніколи — чи панель його *показала*: цей вердикт приходить
пізніше через `onComplete(cb, ctx)` із тим самим `TxTicket`, який видав виклик.

> ### ⚠️ Хто починає сесію на шині автомобіля
>
> Carminat/AFFA3 NAV відкриває сесію з нашого боку: після `begin()` бібліотека оголошує один
> обмежений `3AF BA`, і вже на нього дисплей відповідає запитом
> `0x3CF: 61 11 xx`. Інші профілі панелей мають власний ритм сесії.
> Бібліотека відповідає на реєстрацію `0x74` на `id | 0x400`; на живій шині автомобіля
> переконайтеся, що цю саму роль не виконує штатний вузол. Краще починати зі стенда — див.
> [Розробка без автомобіля](#розробка-без-автомобіля).

Встановлення, `platformio.ini`:

```ini
lib_deps =
  https://github.com/andruxa/AffaDisplay.git

build_flags =
  -std=gnu++17
  -D AFFA_PANEL_CARMINAT=1
build_unflags =
  -std=gnu++11        ; ядро Arduino для ESP32-C3 досі стоїть на gnu++11
```

**Транспорт — це `can_common` / `esp32_can`**, той самий стек, який уже використовує більшість
наявного коду для Renault/ESP32, і той, що доведений від краю до краю на стенді.
`CanCommonLink` володіє власним RX-кільцем і життєвим циклом драйвера. Шов до «сирого» TWAI
колись існував і був видалений за відсутністю споживачів: дві реалізації одного інтерфейсу,
одна з яких нічим не перевірена, — це місце, де вони розійдуться на шині, а не в CI.

#### Збірка взагалі без драйвера CAN

`-D AFFA_ENABLE_ESP32CAN_LINK=0` плюс власний `ICanLink` — підтримувана конфігурація.
Перемикач прибирає direct-TWAI link зі збірки; `lib_ignore` чи зміна маніфесту не потрібні,
бо пакет не має зовнішньої CAN-залежності.

### Підключення

Стендова плата — **ESP32-C3 SuperMini** плюс 3.3 В CAN-трансивер (SN65HVD230 /
TJA1051T-3, *не* 5 В TJA1050 без узгодження рівнів).

| Сигнал | Пін ESP32-C3 | Примітки |
| --- | --- | --- |
| CAN **RX** | `GPIO_NUM_3` | `CRX` / `RXD` / `R` трансивера |
| CAN **TX** | `GPIO_NUM_4` | `CTX` / `TXD` / `D` трансивера |
| `CANH` / `CANL` | — | у джгут панелі |
| Швидкість | **500 000** | задана автомобілем, не обговорюється |
| Термінація | 120 Ом | по одному на кожному фізичному кінці шини; на короткому стенді з панеллю і вашою платою зазвичай достатньо одного резистора, двох — якщо джгут довгий |

> #### Пастка (rx, tx)
>
> `CanPins` зроблено іменованою структурою саме тому, що ці два піни плутають, а симптом —
> не помилка, а **тиша**. Ні TX error, ні жодного кадру, ні рядка в логу:
>
> ```cpp
> g_link.begin(affa::CanPins{.rx = GPIO_NUM_3, .tx = GPIO_NUM_4}, 500000);   // ця плата
> ```
>
> Поточний стенд тепер має те саме призначення, що й **MeganeCAN**. Старі стендові логи та
> прошивки використовували дзеркальне `rx = GPIO_NUM_4, tx = GPIO_NUM_3`; це історичні дані,
> а не схема підключення. `examples/01_bringup` має явний legacy-перемикач лише для старого
> стенду з іншим паянням.

**Carminat/AFFA3 NAV: першими говоримо ми.** Після `begin()` бібліотека оголошує в тишу один
обмежений `3AF BA` — без `B9` попереду: це серцебиття, і воно не стартує до завершення
реєстрації. Далі дисплей надсилає `0x3CF: 61 11 xx` за власним
таймером ~104 мс; **перший** запит лише зводить курок, а сплеск із трьох однакових кадрів
`B0 14 11 00 1F 00 00 00` (31 мс один від одного) виходить через **30.75 мс після
наступного** запиту. Між `B0`#1 і `B0`#2 панель надсилає `1C1 70`, і ми **зобов'язані**
відповісти `5C1 74` протягом ~0.5 мс — безумовно, на будь-якій фазі. Реєстрація
(`151 70`, `1F1 70`, конвеєрно, за 0.29 мс одна від одної) іде через 0.1–0.3 мс після
`B0`#3 і є частиною відкриття, а не першого рендера. Потім 400 мс — і `151 03 52 09 …`
(вмикання екрана), завжди першим прикладним кадром. `3AF B9` — вільний 500-мс heartbeat,
**не відповідь** на `69` панелі, і він не стартує до завершення реєстрації. `BA` ніколи не
періодичний.

**`61 11 00` і `61 11 01` — це один і той самий запит.** Молодший біт повідомляє власний
стан панелі, а не рівень авторизації.

> **Виправлено 2026-08-04 за чотирма OEM-захопленнями** (`docs/captures/*.csv`, розбір у
> `docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md`). Тут раніше стояло: *"бібліотека не передає,
> доки дисплей не надішле повний `0x3CF: 61 11 xx`"*, *"три-кадровий Carminat hello:
> `70 1A 11`, `B0 14 11`, `B0 14 11`"*, *"`61 11 01` — лише bootstrap … Лише пізній
> `61 11 00` дозволяє послідовну реєстрацію"*. Спростовано: наш `BA` іде першим (панель
> відповідає на нього через 7.24 мс); `70 1A 11` не трапляється **жодного разу** серед 579
> OEM-кадрів; а `docs/captures/aknowledge offed display cONNECT OT POWER.csv` містить
> шістнадцять `61 11 01` і жодного `61 11 00` — і повністю проходить сесію на них.

Окремий `69` — це liveness, не старт сесії. **Зареєстрована панель узагалі не надсилає
`61 11`**, тож будь-який повний `61 11 xx` під час утримуваної реєстрації означає, що панель
скасувала сесію — незалежно від третього байта.

### Підтримувані панелі

| Родина | Клас | Sync id | Reply id | Function ids | Key id | Key ACK |
| --- | --- | --- | --- | --- | --- | --- |
| Carminat / AFFA3 | `affa::CarminatDisplay` | `0x3AF` | `0x3CF` | `0x151`, `0x1F1` | `0x1C1` | `0x5C1` |
| UpdateList / AFFA2 — **будь-яке скло** | `affa::UpdateListDisplay` | `0x3DF` | `0x3CF` | `0x121`, `0x1B1` | `0x0A9` | `0x4A9` |
| Приборка — **не перевірена** | `affa::ClusterDisplay` | `0x3AF` | `0x3CF` | `0x151`, `0x1F1` | — | — |

ACK id завжди **обчислюється** як `funcId | 0x400`, і ніколи не береться з таблиці.
`0x0A9 | 0x400` — це `0x4A9`, а не `0x5A9`, бо біт 8 у `0x0A9` уже нульовий — унікально в цій
таблиці. Захардкоджений ACK id — це баг, який чекає на родину UpdateList.

Кожна родина колись мала **twin** — модель панелі, яка збирала те, що ви передали, і
відповідала ACK так, як це робить залізо. Twin-и **видалено**: це був код рівня застосунку,
який жив у бібліотеці. Те, для чого їх використовували, лишилося у двох менших частинах —
`setSelfAck()` дає ACK, коли панелі немає, а `test/affa_decode.h` тримає збирач, яким можна
прочитати шину назад — там, де йому й місце.
Див. [Розробка без автомобіля](#розробка-без-автомобіля).

### Матриця можливостей

Питайте `display.supports(affa::Feature::X)` перед викликом; будь-який непідтриманий виклик
повертає `Result::NotSupported`, а не мовчазний успіх.

| Можливість | Carminat | UpdateList | Приборка |
| --- | :---: | :---: | :---: |
| `Text` | так | так | **ні** — у єдиному лозі немає жодного текстового кадру, тож кодування невідоме |
| `Time` | так | ні | ні |
| `Power` | так | так | так |
| `Menu` | **так, безумовно** | ні | ні |
| `Popup` | якщо `AFFA_ENABLE_POPUP` | ні | ні |
| `Fullscreen` | якщо `AFFA_ENABLE_FULLSCREEN` | ні | ні |
| `ConfirmBox` | якщо `AFFA_ENABLE_CONFIRMBOX` | ні | ні |
| `InfoPopup` | якщо `AFFA_ENABLE_INFOPOPUP` | ні | ні |
| `NavBitmap` | якщо `AFFA_ENABLE_NAV` | ні | ні |
| `KeyTx` | так (`0x1C1`) | так (`0x0A9`) | ні |

**І окремо питайте, скільки туди влізе.** `panelGeometry()` повідомляє рядки, символи в
рядку, місткість списку і розмір картинки, причому **кожне поле нульове, якщо такої поверхні
немає**:

| | головних символів | меню | інфо-рядки | список | картинка |
| --- | :---: | :---: | :---: | :---: | :---: |
| Carminat | 8 | 2 × 26 | 3 × 8 | 10 | 48 × 48 |
| UpdateList | 8 | — | — | — | — |
| Приборка | — | — | — | — | — |

Вісімка в UpdateList — це **обіцянка, а не вимір**: кадр завжди несе поле на 12 комірок, тож
ширше скло в цій родині покаже більше задарма, але радіо не може знати, яке скло відповіло, а
вісім — це те, що показує кожна панель родини.

> **`Feature::RadioText` видалено.** Він повідомляв про *прапорець компіляції* — що збирач
> зібрано — а запит можливостей, який відповідає на питання про вашу власну збірку, нічого не
> каже про скло. Вхідний `0x121` від радіо досі декодується і повідомляється через захищений
> гак `UpdateListBase::onRadioText(bool isAux)`; таблиця патернів — у
> `docs/PROTOCOL-NOTES.md` §8.

### Бібліотека — це транспорт, а не UI

**Тут немає жодних віджетів.** Ні опціональних, ні типово вимкнених — жодного.

Колись були. `src/widget/` тримав автомат меню з ковзним вікном, вікно біжучого рядка і
трирядковий живий екран, плюс адаптер для Carminat і контролер сторінок та клавіш, за
прапорцями `AFFA_ENABLE_MENU` і `AFFA_ENABLE_MARQUEE`. Усе це видалено разом із прапорцями.

Правило, від власника, 2026-08-08:

> Який пункт вибрано, що означає утримання `Load`, як швидко їде назва і коли перемальовувати
> — це рішення про **продукт**. CAN-драйвер, який їх ухвалює, — це CAN-драйвер, який ви не
> зможете взяти в інший продукт.

Панель насправді визначає виклики малювання, і вони **безумовні**:

```cpp
panel.showMenu(header, row0, row1, scrollByte);   // 96-байтний екран 0x21/0x01
panel.showMenuN(buf, sizeof buf, header, items, n);
panel.highlightItem(rowTag);
panel.selectMenuItem(i);
panel.showInfoMenu(header, a, b, c);
panel.setText("HELLO");
```

Заголовок, рядки, який із них підсвічено, які стрілки. Це все, що є на шині, і нічого з цього
не сидить за прапорцем віджета.

Усе, що вище, — ваше, і воно менше, ніж звучить: застосунок, якому потрібна назва, що їде,
кличе `setText` з іншим вікном кожні 400 мс — рівно те, що робив `Marquee`, тільки на таску
бібліотеки, де йому не місце. `examples/17_mediascreen` і `examples/18_aiscreen` будують свої
екрани на цих викликах, кожен по-своєму, і в цьому суть.

Питайте панель, що влізе, замість припущень: `panelGeometry()` повідомляє рядки, символи в
рядку і розмір картинки, і **кожне поле нульове, якщо такої поверхні немає**.


### Перемикачі конфігурації

`src/AffaConfig.h` — єдиний заголовок із перемикачами; кожен описано там разом із ціною і
наслідками. Задавайте їх у власних `build_flags` — заголовок лише підставляє значення за
замовчуванням.

| Макрос | Типово | Що вмикає |
| --- | :---: | --- |
| `AFFA_PANEL_CARMINAT` | `0`¹ | панель Carminat / AFFA3 |
| `AFFA_PANEL_UPDATELIST` | `0`¹ | UpdateList / AFFA2. **Одне кодування для будь-якого скла в родині** — прапорця LCD-різновиду більше немає, бо LCD-різновиду ніколи й не було. |
| `AFFA_PANEL_CLUSTER` | `0` | приборка. **Ніколи не вмикається типово, навіть через `DEFAULT_ALL`**: усе, що вона стверджує, — висновок з єдиного лога, і її відкриття не може завершитися. |
| `AFFA_PANEL_DEFAULT_ALL` | `0`¹ | явна згода «зібрати Carminat + UpdateList». Для першого знайомства і для довідкових збірок обсягу. |
| `AFFA_ENABLE_POPUP` | `1` | `showPopupText` / `hidePopup` |
| `AFFA_ENABLE_FULLSCREEN` | `1` | `showFullscreenText` |
| `AFFA_ENABLE_CONFIRMBOX` | `1` | `showConfirmBox` (рівно на стелі в 113 байтів) |
| `AFFA_ENABLE_INFOPOPUP` | `1` | `showInfoPopup` (три повідомлення) |
| `AFFA_ENABLE_BIGMENU` | `1` | `showMenuN` — екран-список на N пунктів. У спокої не коштує нічого: буфер належить тому, хто викликає. |
| `AFFA_ENABLE_NAV` | `1` | `showNavBitmap` / `navTick` — панель 48 × 48. Картинка лишається у флеші того, хто викликає. |
| `AFFA_ENABLE_TRANSLITERATION` | `1` | `toAscii` і його таблиця (~1,2 кБ). **0 — небезпечно**: UTF-8 тоді доходить до шини як є і малюється сміттям — це візуальна поломка, а не помилка компіляції. |
| `AFFA_ENABLE_LOG` | `1` | макроси `AFFA_LOG*`. 0: у флеш не потрапляє жоден формат-рядок, тож ніколи не кладіть побічний ефект в аргумент лога. |
| `AFFA_LOG_LEVEL` | `3` | 0 вимк, 1 error, 2 warn, 3 info, 4 debug, 5 trace. На етапі компіляції. |
| `AFFA_ENABLE_CANCOMMON_LINK` | `1` на Arduino, `0` на хості | `CanCommonLink`, транспорт поверх `can_common` / `esp32_can` |
| `AFFA_ENABLE_TASK` | `1` на ESP32 | бібліотека володіє задачею опитування. З ним **кожен рендер можна кликати з будь-якої задачі** — див. нижче. Поза ESP-IDF / Arduino-ESP32 це `#error`. |
| `AFFA_DISPATCH_DEPTH` | `8` | слоти міжзадачної передачі. Степінь двійки; `0` прибирає кільце. Глибше за `AFFA_TX_QUEUE_DEPTH` — лише відкласти `QueueFull` у гірше місце. |
| `AFFA_TX_COALESCE` | `1` | перемагає останнє значення в межах `RenderSlot`. 0 відтворює дефект «панель рахує далі після Pause». |
| `AFFA_TX_QUEUE_DEPTH` | `6` | слоти черги, `~AFFA_MAX_PAYLOAD + 12` Б кожен. 6, а не 4, бо `showInfoPopup` — це три повідомлення, а перший виклик після ресинку несе ще дві реєстраційні проби. |
| `AFFA_MAX_PAYLOAD` | `113` | **обмеження шини, а не бюджет**: `8 + 15×7 = 113`, точка, де лічильник ISO-TP переповнився б. Нижче 96 меню Carminat повертає `TooLong`. |
| `AFFA_RX_RING_DEPTH` | `32` | степінь двійки. 32 × `sizeof(Frame)` = 448 Б; переживає ~7 мс паузи між викликами `poll()` на завантаженій шині. |
| `AFFA_ACK_TIMEOUT_MS` | `2000` | дедлайн ACK на кадр; точно збігається зі старим блокуючим очікуванням |
| `AFFA_PEER_TIMEOUT_MS` | `5000` | тиша до розриву синхронізації. **Реальне вікно — до цього плюс `AFFA_SYNC_INTERVAL_MS`**, бо сторож оцінюється на такті серцебиття. Ніколи не опускайте нижче за найдовший запис у флеш: ISR TWAI не в IRAM, тож OTA чи запис у NVS виглядає точно як панель, що замовкла. |
| `AFFA_SYNC_INTERVAL_MS` | `1000` | ритм серцебиття. Вважайте фіксованим: так показує лог. |
| `AFFA_TEXT_MAX` | `64` | буфер тексту |

¹ **Не назвати жодної панелі — це помилка компіляції, а не типова поведінка.** Прапорці
панелей типово `0`, і `AffaConfig.h` видає `#error`, коли всі вони `0` — а це саме той стан,
який лишає по собі помилково написаний `-D AFFA_PANEL_CARMINET=1`, і єдиний спосіб цю
одруківку зловити (`-Wundef` її не бачить: помилковий макрос *визначений*, просто ніхто його
не читає).

**Прапорці, які тут були і яких немає.** `AFFA_PANEL_UPDATELIST_MENU`, `AFFA_ENABLE_MENU`,
`AFFA_ENABLE_MARQUEE`, `AFFA_ENABLE_ISOTP_RX`, `AFFA_ENABLE_ESP32CAN_LINK`,
`AFFA_MAX_SUBSCRIPTIONS`, `AFFA_TASK_QUEUE_DEPTH`, `AFFA_MENU_MAX_ITEMS`,
`AFFA_MENU_MAX_FIELDS`, `AFFA_MENU_ROW_MAX`. Кожен досі названий у `AffaConfig.h` з абзацом
про те, що він робив і чому пішов — перемикач, який задає розмір структури, якої в бібліотеці
вже немає, гірший за відсутність перемикача: хтось його покрутить, нічого не зміниться, і він
піде шукати баг у справжньому місці. Повний звіт — `docs/API.md` §7.

**Кожен виклик, що повертає `Result`, позначено `[[nodiscard]]`.** Рендер, чий `Result` ви
відкинули, — це екран, який тихо не з'явився: `NoSync`, `QueueFull`, `TooLong` і
`NotSupported` з місця виклику виглядають так само, як успіх. Якщо ігноруєте свідомо —
скажіть це: `(void)display.setText("RENAULT", 0);`.

### Обсяг прошивки

ESP32-C3 (`board = esp32-c3-devkitm-1`, Arduino core 2.0.17), release-збірка. **Виміряно
2026-08-08 через `pio run -c platformio_footprint.ini`** — усі числа нижче з того запуску, а
не з минулого.

Ця відмінність і є суттю. Таблиця, яку це замінює, цитувала віджет меню, прапорець ISO-TP,
таблицю підписок і `Esp32CanLink`, яких давно немає; гарнесс, що її породив, уже не
компілювався, тож виправити її ніхто б не зміг. Тепер компілюється, і перезапуск — це одна
команда.

**Базові точки, той самий тулчейн:**

| Базова точка | Флеш | RAM |
| --- | ---: | ---: |
| порожній скетч `setup()`/`loop()` | 218 912 Б | 13 476 Б |
| …плюс `can_common` + `esp32_can`, без AffaDisplay | 257 724 Б | 14 564 Б |

**Опорна збірка** — `g_base`: Carminat + UpdateList, `CanCommonLink`, власний таск, усі
прапорці ввімкнені, і зонд, що викликає **кожен** опціональний рендер, щоб `--gc-sections` не
могло прибрати те, що має прибирати прапорець:

| | Флеш | RAM |
| --- | ---: | ---: |
| `g_base` | 284 036 Б | 22 140 Б |
| Δ проти CAN-базової | **+26 312 Б** | **+7 576 Б** |

#### Скільки насправді коштує кожен прапорець

Виміряно перемиканням рівно одного прапорця проти `g_base`.

| Зміна | Флеш | RAM | |
| --- | ---: | ---: | --- |
| `AFFA_ENABLE_CANCOMMON_LINK=0` | **−11 256 Б** | −1 888 Б | найбільше з усього — це зовнішня бібліотека, не наш код |
| `AFFA_PANEL_CARMINAT=0` | −4 980 Б | −2 704 Б | велика родина: меню, попапи, діалоги, нав-панель |
| `AFFA_ENABLE_LOG=0` | −2 788 Б | −16 Б | здебільшого формат-рядки |
| `AFFA_ENABLE_TRANSLITERATION=0` | −2 178 Б | 0 | таблиця відповідностей. **Не робіть цього**, якщо тільки кожен рядок у вас уже не ASCII |
| `AFFA_PANEL_UPDATELIST=0` | −1 430 Б | −2 528 Б | |
| `AFFA_ENABLE_BIGMENU=0` | −818 Б | −192 Б | `showMenuN` + `selectMenuItem` |
| `AFFA_ENABLE_NAV=0` | −806 Б | 0 | `showNavBitmap` + `navTick` |
| `AFFA_ENABLE_CONFIRMBOX=0` | −558 Б | 0 | |
| `AFFA_ENABLE_TASK=0` | −308 Б | −144 Б | див. застереження нижче — це не ціна таска |
| `AFFA_ENABLE_INFOPOPUP=0` | −278 Б | 0 | |
| `AFFA_ENABLE_FULLSCREEN=0` | −256 Б | 0 | |
| `AFFA_ENABLE_POPUP=0` | −204 Б | 0 | |
| `AFFA_PANEL_CLUSTER=1` | **+414 Б** | **+2 528 Б** | третя родина, додається, а не прибирається |

**Два з цих чисел були багами раніше, ніж стали числами**, і гарнесс — це те, як вони
випливли:

* **`-D AFFA_ENABLE_FULLSCREEN=0` не лінкувався.** Блок `#if AFFA_ENABLE_FULLSCREEN` у
  `CarminatDisplay.cpp` розрісся так, що охопив блоки `BIGMENU` і `NAV`, тож вимкнення
  повноекранного режиму мовчки прибирало *визначення* `selectMenuItem`, `showMenuN`,
  `showNavBitmap`, `showNavBitmapWithHeader` і `navTick`, тоді як заголовок і далі їх
  оголошував. П'ять undefined reference від прапорця, якого просили прибрати одну функцію.
* **Бібліотека збиралася тільки тому, що кожне середовище в цьому репозиторії визначало
  макрос, який бібліотека видалила.** `CarminatDisplay::supports()` досі містив
  `AFFA_ENABLE_MENU != 0` — це *вираз C++*, а не перевірка препроцесора, тож невизначений
  макрос тут є жорсткою помилкою, а не попередженням, — а `platformio.ini` і далі передавав
  `-D AFFA_ENABLE_MENU=0` усім чотирьом прикладам. **Будь-хто, хто цього прапорця не
  передавав, отримав би помилку компіляції на `supports()`.** Тепер `Feature::Menu` повертає
  `true` безумовно, бо `showMenu` і `highlightItem` — це протокол, і жоден прапорець їх не
  прибирає.

**Як чесно читати малі числа.**

* **`AFFA_ENABLE_TASK=0` дає −308 Б, і це не означає, що власний таск безкоштовний.** Зонд
  оголошує `AffaTask`, але ніколи не робить `start()`, тож більшість `rtos/AffaTask.cpp`
  збирається сміттярем. −308 Б — це ціна *наявності типу*. Збірка, яка справді стартує таск,
  платить ще `AFFA_TASK_STACK` (типово 4 096 Б) з купи, і жоден статичний вимір цього не
  побачить.
* **Невибрана панель коштує нуль, і це механізм, а не сподівання.** Кожен опціональний
  `.cpp` загороджує все своє тіло, тож компілюється в порожній об'єктний файл — препроцесор
  є єдиним, що може прибрати одиницю трансляції під Library Dependency Finder, бо
  `build_src_filter` споживача не дістає всередину бібліотеки з `lib_deps`.
* **Ціна прапорця видима лише тоді, коли щось його викликає.** У збірці, яка ніколи не чіпає
  `showNavBitmap`, `--gc-sections` уже його прибрав, і `AFFA_ENABLE_NAV=0` не показує нічого.
  Зонд існує саме щоб це перебороти — тому ці числа більші за ті, що ви побачите у власному
  застосунку.
* `g_neg_typo` і далі має не компілюватися, і в цьому його сенс: помилково написаний
  `-D AFFA_PANEL_CARMINET=1` лишає всі справжні макроси панелей на `0`, і тільки `#error`
  може це зловити.

### Багатозадачність і неблокуючий контракт

* **Ніякого `delay()`, активного очікування чи `vTaskDelay()` — ні в `core/`, `util/`,
  `link/`, ні в жодній із панелей.** `IClock` віддає `millis()` і
  навмисно більше нічого. **Єдиний виняток — `src/rtos/AffaTask.cpp`**:
  власний `vTaskDelayUntil` задачі бібліотеки між ітераціями, бо саме це і є період задачі.
  Один виклик, у єдиному каталозі, що потребує FreeRTOS; він присипляє задачу бібліотеки —
  ніколи вашу і ніколи шлях даних.
* **Ніякої купи після `begin()`.** Усі буфери статичні і задані макросами в `AffaConfig.h`.
  Ні `String`, ні `std::vector`, ні `std::function` в ядрі.
* **Ніякого стану на рівні файлу чи статичних локальних змінних.** Кожен лічильник, дедлайн і
  буфер — це поле об'єкта, тож два екземпляри на двох шинах не заважають один одному. (У
  видобутому коді були черга подій на рівні файлу, статична мітка часу логу і
  `static int8_t timeout`; у бібліотеці це спільний стан між екземплярами.)
* **`poll()` викликає рівно одна задача.** Бібліотека на екземпляр і **без локів** — це
  свідомий вибір, а не недогляд, і саме він тримає `poll()` вільним від критичних секцій.
  **Але це правило лише для `AFFA_ENABLE_TASK=0`.** З увімкненим власним таском — а на ESP32
  він типово увімкнений з 2.0 — рендерити може будь-який контекст напряму, і жодна поштова
  скринька не потрібна; див. нижче.
* **Callback-и викликаються з контексту `poll()`**, ніколи із задачі драйвера CAN. Стан
  фіксується *до* того callback-у, який про нього повідомляє, тож із callback-у можна
  викликати бібліотеку далі — рендери, `abortPending()`, `pressKey()` — але ніколи сам
  `poll()`.
* **Вказівники, передані в callback, дійсні лише на час його виконання.** Вони вказують на
  внутрішню пам'ять бібліотеки. Копіюйте те, що зберігаєте.
* **Кадри, які ми надсилаємо, не залежать від частоти виклику; а от чи завершиться передача —
  залежить.** Раз на секунду і мільйон разів на секунду дають ті самі кадри в тому самому
  порядку з тим самим таймінгом — ніщо не рахує виклики. Але `AFFA_ACK_TIMEOUT_MS` і
  `AFFA_PEER_TIMEOUT_MS` — це дедлайни за реальним часом, які перевіряються *всередині*
  `poll()`, тож запізнілий `poll()` не затримує результат, а **змінює** його: `Ok` стає
  `Timeout`, а прострочений дедлайн однолітка зносить реєстрацію. У ранніх редакціях була
  лише перша половина, і це читалося як дозвіл ділити задачу опитування. (docs/API.md §4.4.)
* **З 2.0 бібліотека типово володіє задачею на ESP32, і в цьому розвороті — суть
  рефакторингу.** `AFFA_ENABLE_TASK` там дорівнює `1`: збирається `src/rtos/`, створюється
  задача з періодом 2 мс і пріоритетом 2, і **кожен рендер можна кликати з будь-якої задачі —
  усі, включно з тими, що будуть написані після цього речення.** Без м'ютексів, без поштової
  скриньки, без передавання і без нічого, що треба переписувати вручну.

  Раніше типовим було `0`, і аргумент був слушний на той час: ввімкнення змінює, у якій
  задачі працюють колбеки споживача. Наслідок піддається підрахунку — **тринадцять із
  дев'ятнадцяти прикладів вимкнули це** і крутили `poll()` з `loop()`, включно з тим, чиї
  HTTP-обробники потім змагалися з чергою.

  Причини вимикати більше немає. До 2.0 задача публікувала власну поверхню рендерів, що
  покривала десять викликів із двадцяти двох, тож усе багатше повертало застосунок до
  «сирого» дисплея — і тоді задача була зайвим об'єктом із другим словником. **Тепер
  потокобезпечна поверхня — це сам дисплей**, з якої задачі його не кличуть, і задача коштує
  споживачеві лише двох рядків, що її стартують.

  `KeyCb` і далі спрацьовує синхронно всередині `poll()`, тож затримка клавіші не змінилася:
  її межа — період задачі й нічого більше. `poll()` відмовляє викликачеві, який не є
  задачею-власником, і рахує такі виклики. `examples/17_mediascreen` — взірець: сорок
  одночасних HTTP-рендерів проти справжньої панелі, усі лічильники по нулях; `docs/API.md`
  §4.7 — механізм, §4b — контракт. `core/` і `util/` це не зачіпає, і вони й далі збираються
  на хості з нічим, окрім C++17.
* **Винятку немає.** У ранніх редакціях він був — `sendBlocking(ticket, timeoutMs)`, який
  крутив `poll()`, доки квиток не завершиться. Його видалено: його не викликало ніщо в
  `src/`, `examples/` чи `test/`, а бібліотека, головна обіцянка якої — ніколи не блокувати,
  не має постачати єдиний виклик, що блокує. Чекайте через `onComplete()` у власному циклі.
### Затримка і витіснення

Гарантія, зафіксована в `test_latency` як **кількість викликів `poll()`**, а не як обіцянка в
мілісекундах:

> **Кнопка доходить до вашого callback-у рівно за один `poll()`** — і з порожньою чергою, і
> так само тоді, коли в польоті 96-байтний `showMenu`, `WaitAck` тримає 1900 мс зі своїх
> 2000 мс дедлайну, а всі слоти передачі зайняті.

Це випливає з порядку всередині `poll()`: спершу вичерпати RX і доставити кнопки, і лише
**строго після цього** качати автомат передачі. Автомат TX перевіряє дедлайн і повертається;
він ніколи не чекає.

* **Перемагає найновіше, у межах `RenderSlot`.** Повторний рендер займає рівно один слот
  черги незалежно від частоти і завжди тримає найсвіжіше значення; витіснені квитки
  завершуються з `Result::Aborted`. Три `setText`, поставлені за меню в польоті, стають одним
  повідомленням із третім рядком.
* **Різні слоти ніколи не витісняють один одного** — оновлення годинника не з'їсть popup.
* **`abortPending()`** прибирає все, що в черзі, але *ще не почалося*, повідомляючи `Aborted`
  по одному разу на квиток, у порядку. **`Priority::Urgent`** обганяє чергу, але ніколи не
  обганяє зонди реєстрації.
* **Повідомлення на шині ніколи не розривається.** `Urgent` і `abortAll()` спрацьовують лише
  на межі кадру, а лічильник продовження ISO-TP скидається при відмові від завдання, тож він
  не може зіпсувати наступне повідомлення.
* **Власні передані кадри інертні.** Кожен переданий кадр позначається `Frame::fromSelf` і
  відкидається до auto-ACK, до зіставлення ACK **і** до декодера кнопок. Справжній контролер
  не повертає собі власні кадри; `LoopbackLink` може. Поведінка однакова в обох випадках — і
  саме це робить хостові тести чогось вартими.

Без цього лічильник, що малюється з частотою 10 Гц перед 13-кадровою передачею меню, лишає
хвіст застарілих значень: панель видимо рахує далі ще секунду *після* того, як користувач
натиснув Pause і бібліотека коректно отримала кнопку. Виглядає як баг обробки кнопок, а є
багом черги. `examples/06_counter_preempt` це вимірює.

### Коди кнопок

Джойстик фізично є частиною **панелі**: натискання змушує панель закодувати і передати кадр
кнопки, який приймає радіо. **Штатна роль цієї бібліотеки — радіо**, тож кнопки завжди
приходять *до* нас, і `pressKey()` / `nav()` типово працюють як `KeySource::Local`.

Кадр на шині, на `0x1C1` (Carminat) або `0x0A9` (UpdateList):

```
03 89 <code>>8> <code&0xFF | (hold ? 0xC0 : 0)> <filler × 4>
```

| `affa::Key` | Код | Примітки |
| --- | :---: | --- |
| `Load` | `0x0000` | кнопка знизу підрульового важеля; утримання `Load` — типовий жест відкриття меню |
| `SrcNext` | `0x0001` | |
| `SrcPrev` | `0x0002` | |
| `VolUp` | `0x0003` | |
| `VolDown` | `0x0004` | |
| `Pause` | `0x0005` | |
| `RollUp` | `0x0101` | коліщатко, один клац угору |
| `RollDown` | `0x0141` | коліщатко, один клац униз |

Чотири речі в цій таблиці критичні:

1. **Перевірка `03 89` не є необов'язковою.** Той самий id кнопок несе також `70 A3..`,
   `02 64 0F A3..` і `05 63 "0037"`. Декодер без цієї перевірки вигадує кнопки `0x640F` і
   `0x3030` зі звичайного трафіку.
2. **Утримання клацання коліщатка невідновне за задумом.** `0x0101 | 0xC0` і `0x0141 | 0xC0`
   — це *обидва* `0x01C1`, бо `0x40` одночасно є бітом напрямку RollDown і половиною маски
   утримання. Вони декодуються як `RollUp` + hold, а кодувальник відмовляється передавати
   будь-який із них: утримання коліщатка не має жодного представлення на шині. Саме тому
   `NavCommand::Increase` / `Decrease` доступні лише через `KeySource::Local`.
3. **Перелік відкритий.** Ці вісім імен підтверджені `[REF]`, але ніщо не доводить, що список
   *повний*. Нерозпізнаний код доставляється як `static_cast<Key>(raw & 0xFF3F)` — тобто
   `Key`, що несе сирий код із шини, — і ніколи не відкидається. Завжди пишіть `default:` у
   `switch` по `Key`.
4. **`KeySource::Wire` кладе фантомні натискання на шину.** На стенді це нешкідливо; в
   автомобілі це ввід, на який можуть зреагувати інші блоки.

### Розробка без автомобіля

Три рівні, і жоден не потребує машини. Повний покроковий опис із командами, які можна просто
скопіювати, — **[`docs/DEVELOPING-WITHOUT-HARDWARE.md`](docs/DEVELOPING-WITHOUT-HARDWARE.md)**.

1. **Лише ноутбук — узагалі без плати.**
   ```
   pio test -e native            # 259 випадків
   ```
   Уся бібліотека працює на хості поверх `LoopbackLink`: `setSelfAck(true)` дає покадровий
   ACK замість панелі, а один вкинутий `61 11` завершує handshake. Щоб перевіряти те, що
   *намальовано*, а не те, що надіслано, декодуйте передані кадри назад через
   `isotp::Reassembler` + `affa::screen` — у `test_bench_surface` є рівно тридцять рядків,
   які це роблять, і це зразок для копіювання.

   Self-ACK — це правило **Declared**: PARTIAL, поки оголошений FF_DL не набрано, і DONE на
   ньому. Саме так поводиться залізо, і саме це відтворює всі кількості кадрів із wire spec
   (`showMenu` = 13 кадрів, останній PCI `0x2C`), не знаючи їх наперед.
2. **Гола плата ESP32 — без трансивера і без панелі.** Прошийте `examples/90_bench_ota`,
   відкрийте вебконсоль, переключіть її на `panel=virtual`.
   Декодер годується з Layer-0 tap, тому та сама схема обслуговує і віртуальну панель, і
   пасивне декодування поруч зі справжньою. У браузері ви отримуєте живе кільце кадрів,
   декодоване «скло», ін'єкцію кнопок і лічильники затримок.
3. **Справжня панель на столі.** Підключення як вище, 500 кбіт/с, пам'ятайте про пастку
   `(rx, tx)` і про те, що **розмову починаємо ми**: бібліотека кидає в тишу `BA` кожні
   ~30 с, і панель відповідає на `0x3CF: 61 11 xx`. Спершу прошийте `examples/01_bringup` —
   він доводить лінк у тому порядку, у якому це треба робити, і на шині з двох вузлів
   `txErr == 0` є доказом того, що панель вас підтверджує.

Той самий документ описує, як зняти власний трафік, як звірити його з `docs/WIRE-SPEC.md` і
як додати четверту родину панелей.

### Власник контролера

`CanCommonLink` володіє одним контролером CAN і його RX-кільцем. Не керуйте цим контролером
з коду застосунку — ні власними `begin()`/`watchFor()`/`sendFrame()` з `esp32_can`, ні
викликами ESP-IDF `twai_*` у нього за спиною. Для тихого
періоду використовуйте `setTxEnabled()`, а `setListenOnly()` — лише через link; recovery
веде poll-backoff бібліотеки.

`send()` не блокує: accepted означає «поставлено в чергу», а не «дисплей опрацював кадр».
Відповідь протоколу дисплея `0x74` є доказом доставки; лічильники TX контролера лише
діагностичні.

### Документи і тести

```
pio test -e native      # 197 тестів у 15 наборах, без заліза
pio run                 # 5 середовищ: одне хостове і чотири приклади для ESP32
```

| Документ | Що це |
| --- | --- |
| [`docs/API.md`](docs/API.md) | Контракти, під які написана реалізація: багатозадачність, `Result`, затримки, можливості. **Він не копіює оголошень**: оголошення — це заголовки, а §7 каже, куди поділося кожне видалене. |
| [`docs/WIRE-SPEC.md`](docs/WIRE-SPEC.md) | Побайтовий оракул: кожен формат кадру, готові до вставки золоті вектори з найсильнішим свідком для кожного, і арифметика кількості кадрів. **Якщо код і цей документ розходяться щодо байта — неправий код.** |
| [`docs/PROTOCOL-NOTES.md`](docs/PROTOCOL-NOTES.md) | Походження: кожен байт зведено до лога, OEM-запису чи стороннього джерела, плюс відкриті питання, кожне сформульоване як експеримент, що його закриє. |
| [`docs/REFACTOR-2.0.md`](docs/REFACTOR-2.0.md) | Чому поверхня має теперішній вигляд: докази, першопричина і що було видалено, щоб це полагодити. |
| [`docs/ESP32CAN-CONTRACT.md`](docs/ESP32CAN-CONTRACT.md) | Володіння драйвером, RX/TX, життєвий цикл і відновлення. |
| [`docs/PORTING.md`](docs/PORTING.md) | Як перевести застосунок зі старих класів — і як відмовитися від цієї бібліотеки взагалі, включно з тим, які файли специфічні для панелі, а які є придатним до повторного вжитку ядром транспорту. |
| [`docs/DEVELOPING-WITHOUT-HARDWARE.md`](docs/DEVELOPING-WITHOUT-HARDWARE.md) | Три рівні вище, повністю, плюс захоплення трафіку і додавання панелі. |
| [`docs/BENCH-VERIFIED.md`](docs/BENCH-VERIFIED.md) | Що справді бачили на панелі, на відміну від того, у що віриться. |

`core/`, `util/` і `link/LoopbackLink.h` мають збиратися для `platform = native` з нічим,
окрім стандартної бібліотеки C++17. Якщо зміна ламає цю збірку — неправа зміна, а не тест.
`<driver/twai.h>` не зустрічається в бібліотеці ніде: до драйвера ходять через `can_common`.

Ліцензія: **MIT**, див. [`LICENSE`](LICENSE).

---

## 🇺🇦 Ukraine

This project is developed in Ukraine, under a full-scale invasion.
If it was useful to you, consider supporting Ukraine's defence:
https://savelife.in.ua/ and https://u24.gov.ua/
Slava Ukraini.

## 🇺🇦 Україна

Цей проєкт розробляється в Україні, під час повномасштабного вторгнення.
Якщо він був вам корисний, розгляньте можливість підтримати оборону України:
https://savelife.in.ua/ та https://u24.gov.ua/
Слава Україні.
