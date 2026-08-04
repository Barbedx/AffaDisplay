# AffaDisplay

> A non-blocking ESP32 driver for Renault AFFA2 / AFFA3 OEM dash panels over CAN /
> Неблокуючий драйвер штатних панелей Renault AFFA2 / AFFA3 для ESP32 через CAN

**[English](#english) · [Українська](#українська)**

MIT · ESP32 / ESP32-C3 · Arduino + PlatformIO · no heap after `begin()` · no `delay()` anywhere ·
244 host tests, no hardware required

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

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

static void onKey(affa::Key k, affa::KeyEdge e, void*) {
  if (k == affa::Key::Pause && e == affa::KeyEdge::Click) g_display.setText("PAUSED", 0);
}

void setup() {
  // Named struct: the two pins cannot be swapped at the call site, and they have been.
  g_link.begin(affa::CanPins{.rx = GPIO_NUM_3, .tx = GPIO_NUM_4}, 500000);
  g_display.onKey(&onKey, nullptr);
  g_display.begin();                                  // Carminat stays silent until the panel asks
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
> Carminat/AFFA3 NAV opens with a bounded `3AF B9` + `3AF BA` announce from us; the display
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

**No third-party CAN wrapper is required.** `Esp32CanLink` uses the ESP32 Arduino core's
built-in ESP-IDF TWAI driver directly. The transport owns its RX task and driver lifecycle;
this library does not install `ESP32_CAN` or `can_common`.

#### Building without a CAN driver at all

`-D AFFA_ENABLE_ESP32CAN_LINK=0` plus your own `ICanLink` is a supported configuration.
The gate removes the direct-TWAI link from the build; no `lib_ignore` or manifest surgery
is needed because the package has no external CAN dependency.

> ### Carminat/AFFA3 NAV session rule
>
> **Settled 2026-08-04 against four passive captures of a real OEM Renault radio driving a
> real Carminat panel.** Derivation: [`docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md`](docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md).
>
> **We speak first.** Into a silent bus the library announces one bounded `3AF B9` + `3AF BA`
> pair. The panel then requests on `0x3CF: 61 11 xx` (DLC ≥ 3) on its own ~104 ms timer. The
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
announces one bounded `3AF B9` + `3AF BA` pair, the panel replies on `0x3CF: 61 11 xx`, and
its *next* request draws the `B0` announce burst 31 ms later. A powered bench with no panel
therefore shows the `B9`/`BA` pair and then nothing further — that is correct behaviour, not
a fault. (This paragraph used to say the library transmits nothing at all until the panel
speaks; the OEM captures show our `BA` first, with the panel's request arriving 7.24 ms
after it.)

### Supported panels

| Family | Class | Sync id | Reply id | Function ids | Key id | Key ACK |
| --- | --- | --- | --- | --- | --- | --- |
| Carminat / AFFA3 | `affa::CarminatDisplay` | `0x3AF` | `0x3CF` | `0x151`, `0x1F1` | `0x1C1` | `0x5C1` |
| UpdateList / AFFA2, 8-segment | `affa::UpdateListDisplay` | `0x3DF` | `0x3CF` | `0x121`, `0x1B1` | `0x0A9` | `0x4A9` |
| UpdateList / AFFA2, mono LCD | `affa::UpdateListMenuDisplay` | `0x3DF` | `0x3CF` | `0x121`, `0x1B1` | `0x0A9` | `0x4A9` |

The ACK id is always **computed** as `funcId | 0x400`, never tabulated. `0x0A9 | 0x400` is
`0x4A9` and not `0x5A9`, because bit 8 is already clear in `0x0A9` — uniquely in this
table. A hard-coded ACK id is a bug waiting for the UpdateList family.

Each family used to ship a **twin** — a model of the panel that reassembled what you
transmitted and ACKed the way hardware does. The twins are **deleted**: they were
application-shaped code living in the library. What they were used for is still there, in
two smaller pieces — `setSelfAck()` supplies the ACKs when no panel is attached, and
`AFFA_ENABLE_ISOTP_RX` buys the reassembler and screen decoder you need to read the wire
back. See [Developing without a car](#developing-without-a-car).

### Capability matrix

Ask `display.supports(affa::Feature::X)` before you call; every unsupported call returns
`Result::NotSupported` rather than silently succeeding.

| Feature | Carminat | UpdateList 8-seg | UpdateList LCD |
| --- | :---: | :---: | :---: |
| `Text` | yes | yes | yes |
| `Time` | yes | no | no |
| `Power` | yes | yes | yes |
| `Menu` | if `AFFA_ENABLE_MENU` | no | no |
| `Popup` | if `AFFA_ENABLE_POPUP` | no | no |
| `Fullscreen` | if `AFFA_ENABLE_FULLSCREEN` | no | no |
| `ConfirmBox` | if `AFFA_ENABLE_CONFIRMBOX` | no | no |
| `InfoPopup` | if `AFFA_ENABLE_INFOPOPUP` | no | no |
| `KeyTx` | yes (`0x1C1`) | yes (`0x0A9`) | yes (`0x0A9`) |
| `RadioText` | if `AFFA_ENABLE_ISOTP_RX` | same | same |

One honest caveat, also recorded in `docs/API.md` §6:

* `Feature::RadioText` reports a **compile gate and nothing more** — that the ISO-TP
  reassembler in `proto/` is built, so inbound text *can* be reconstructed. **No panel
  routes reassembled text to the application**, and there is no event for it: the
  never-emitted `EventKind::RadioText` was removed rather than left standing as a
  promise the library could not keep. What UpdateList does with inbound `0x121` is a
  single-frame AUX sniff reported through the protected `UpdateListBase::onRadioText(bool)`
  hook — a subclass seam, unaffected by that removal. An application that wants inbound
  text today subscribes to the raw frames; see `docs/PROTOCOL-NOTES.md` §8.

### The menu is a widget, not the protocol

What the Carminat panel actually defines is two calls, and they are available
unconditionally: `showMenu(header, row0, row1, scrollByte)` — the 96-byte `0x21/0x01` screen
— and `highlightItem(rowTag)`. Header, two rows, which one is lit, which arrows. Everything
above that (which items exist, which is selected, how a window slides over N of them, when
Select advances to the next field) is a UI state machine the panel knows nothing about, which
is why `AFFA_ENABLE_MENU` defaults to `0`.

Say it plainly, because the rest of this section is about the widget and it is easy to lose:
**`showMenu()` and `highlightItem()` are the protocol-level primitives and they are not
optional.** They live in `CarminatDisplay` outside every menu gate. With
`AFFA_ENABLE_MENU=0` — the default — both still compile, still work, and still put the same
bytes on the wire; what you lose is `MenuModel`, `MenuController`, `IPage`, `nav()` and
`getMenu()`, i.e. one *opinion* about how a menu should behave. **The widget is optional; the
two calls it is built on are not.** Drive them yourself and you owe this library nothing.

If you want that state machine rather than your own, `src/widget/` now holds it in a form that
is **not welded to one panel's geometry**: `MenuModel` + `IMenuRenderer` + `MenuGeometry`.
Rows, characters-per-row and wrap are injected, so the same algorithm drives the 2 × 26
Carminat menu screen, the 3 × 8 info-row screen (`showInfoPopup`) and a 6 × 20 OLED. The model
speaks *row index* and hands you already-truncated, already-transliterated text; row tags,
highlight frames and what a redraw costs stay in the adapter you write — usually under thirty
lines. It compiles on the host with no Arduino, no CAN and no panel header.

Read [`docs/MENU-WIDGET.md`](docs/MENU-WIDGET.md); run `examples/09_menu_widget`, which puts
one identical menu on three different displays. **There is only one implementation:**
`src/carminat/Menu/` — the panel-welded original — has been deleted, `CarminatDisplay` drives
`MenuModel` through `affa::CarminatMenuRenderer`, and `getMenu()` keeps its name while
returning `widget::MenuModel&`. `affa::Menu` and `affa::MenuItem` survive as aliases, so
existing item-building code compiles unchanged; the two observable differences are that
`render()` returns `void` (ask `menuRenderer().lastResult()` for the panel's verdict) and that
rows truncate at the injected 26 characters. `MenuController` / `IPage` keep their job — the
page stack and the `(Key, KeyEdge)` → intent map, which is navigation policy, not menu.

### Configuration knobs

`src/AffaConfig.h` is the single knob header; every gate is documented there with what it
costs and what breaks. Set them in your own `build_flags` — the header only ever supplies
defaults.

| Macro | Default | What it controls |
| --- | :---: | --- |
| `AFFA_PANEL_CARMINAT` | `0`¹ | Carminat / AFFA3 panel |
| `AFFA_PANEL_UPDATELIST` | `0`¹ | UpdateList 8-segment panel |
| `AFFA_PANEL_UPDATELIST_MENU` | `0`¹ | UpdateList mono-LCD variant (implies the line above) |
| `AFFA_PANEL_DEFAULT_ALL` | `0`¹ | opt in to "compile all three panels". Only for a first look and for the footprint reference builds. |
| `AFFA_ENABLE_MENU` | **`0`** | `src/widget/`, `CarminatMenuRenderer`, `MenuController`, `IPage`, `nav()`, `getMenu()`. The largest optional block, and **off by default**: the menu is a widget, not protocol. `showMenu` / `highlightItem` stay available with it off. |
| ↳ `src/widget/` | *same gate* | `MenuModel` + `IMenuRenderer` + `MenuGeometry` — the sliding-window algorithm with rows, characters-per-row and wrap as **parameters**, for any display. Panel-free, host-testable, no heap after construction. The **only** menu implementation in the library; `CarminatDisplay` uses it too. See [`docs/MENU-WIDGET.md`](docs/MENU-WIDGET.md). |
| `AFFA_ENABLE_POPUP` | `1` | `showPopupText` / `hidePopup` |
| `AFFA_ENABLE_FULLSCREEN` | `1` | `showFullscreenText` / `hideFullscreenText` |
| `AFFA_ENABLE_CONFIRMBOX` | `1` | `showConfirmBox` (sits at exactly the 113-byte ceiling) |
| `AFFA_ENABLE_INFOPOPUP` | `1` | `showInfoPopup` / `hideInfoPopup` (three messages) |
| `AFFA_ENABLE_TRANSLITERATION` | `1` | `toAscii` + its table (~1.2 kB). **0 is dangerous**: UTF-8 then reaches the wire unchanged and renders as garbage — a visual failure, not a compile error. |
| `AFFA_ENABLE_LOG` | `1` | the `AFFA_LOG*` macros. 0: no format strings enter flash at all, so never put a side effect in a log argument. |
| `AFFA_LOG_LEVEL` | `3` | 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace. Compile-time. |
| `AFFA_ENABLE_ESP32CAN_LINK` | `1` on Arduino, `0` on host | `Esp32CanLink` and the direct `<driver/twai.h>` transport |
| `AFFA_ENABLE_ISOTP_RX` | `0` on target, `1` on host | the ISO-TP reassembler, the screen decoder, and `onText()`. For reading a channel somebody else writes; the radio role never needs it. |
| `AFFA_ENABLE_MARQUEE` | `1` | `widget::Marquee` and `UpdateListDisplay`'s `setScrollText` / `setScrollActive` / `reassert`. A widget, not protocol — but a small one, and eight cells do not hold a track title. |
| `AFFA_TX_COALESCE` | `1` | latest-value-wins per `RenderSlot`. 0 reproduces the "panel keeps counting after Pause" defect. |
| `AFFA_TX_QUEUE_DEPTH` | `6` | queue slots, `~AFFA_MAX_PAYLOAD + 12` B each. 6 and not 4 because `showInfoPopup` is three messages and the first call after a resync also carries two registration probes. |
| `AFFA_MAX_PAYLOAD` | `113` | **a wire limit, not a budget**: `8 + 15×7 = 113`, the point at which the ISO-TP counter would wrap. Below 96 the Carminat menu returns `TooLong`. |
| `AFFA_RX_RING_DEPTH` | `32` | power of two. 32 × `sizeof(Frame)` = 448 B; tolerates a ~7 ms gap between `poll()` calls on a saturated bus. |
| `AFFA_ACK_TIMEOUT_MS` | `2000` | per-frame ACK deadline; matches the legacy blocking wait exactly |
| `AFFA_PEER_TIMEOUT_MS` | `5000` | silence before sync is torn down. **Effective window is up to this + `AFFA_SYNC_INTERVAL_MS`**, because the watchdog is evaluated on a heartbeat tick. Never lower it below your longest flash write — the TWAI ISR is not in IRAM, so an OTA or NVS write looks exactly like a panel that went quiet. |
| `AFFA_SYNC_INTERVAL_MS` | `1000` | heartbeat cadence. Treat as fixed: it is what the capture shows. |
| `AFFA_MAX_SUBSCRIPTIONS` | `8` | Layer 1 filtered subscription slots |
| `AFFA_MENU_MAX_ITEMS` | `12` | menu capacity |
| `AFFA_MENU_MAX_FIELDS` | `3` | fields per item; `MenuItem` embeds all of them, which is most of the menu's RAM |
| `AFFA_MENU_ROW_MAX` | `32` | rendered row buffer |
| `AFFA_TEXT_MAX` | `64` | text/marquee buffer |

¹ **Naming no panel is a compile error, not a default.** All three panel flags default to
`0`, and `AffaConfig.h` `#error`s when every one of them is `0` — which is also the state a
misspelled `-D AFFA_PANEL_CARMINET=1` leaves behind, and the only way that typo can be
caught (`-Wundef` cannot see it: the misspelled macro *is* defined, merely never read). If
you really want all three, say `-D AFFA_PANEL_DEFAULT_ALL=1`; `size_all` is the one
environment in this repository that does. The host test build names all three explicitly.

**Every `Result`-returning call is `[[nodiscard]]`.** A render whose `Result` you drop is a
screen that silently never appears — `NoSync`, `QueueFull`, `TooLong` and `NotSupported` all
look identical to success from the call site. Ignore one deliberately and say so:
`(void)display.setText("RENAULT", 0);`.

### Footprint

ESP32-C3 (`board = esp32-c3-devkitm-1`, Arduino core 2.0.17), release build, straight from
`pio run`. **Baseline measured on the same toolchain**: an empty `setup()`/`loop()` sketch
is **218 912 B** flash / **13 476 B** RAM.

| Build | Flash | Δ vs empty sketch | RAM | Δ vs empty sketch |
| --- | ---: | ---: | ---: | ---: |
| `size_all` — every panel gate on (`AFFA_PANEL_DEFAULT_ALL=1`) | 266 078 B | +47 166 B | 16 380 B | +2 904 B |
| `size_carminat` — Carminat only | 266 078 B | +47 166 B | 16 380 B | +2 904 B |
| `size_min` — Carminat, no menu/popup/fullscreen/confirm/info, no transliteration, no log, no subscriptions | 264 198 B | +45 286 B | 16 052 B | +2 576 B |
| `ex10_callbacks` — Carminat, no menu, **plus `AFFA_ENABLE_ISOTP_RX`** (`onText`) | 271 776 B | +52 864 B | 16 460 B | +2 984 B |
| `ex09_menu_widget` — Carminat **plus the menu widget**, used | 276 826 B | +57 914 B | 19 892 B | +6 416 B |

<sub>Re-measured from a clean `pio run` on 2026-07-28, after `src/vpanel/` was deleted. The
`ex07_virtual_panel_c3` row that used to sit here measured a build that no longer exists;
`ex10_callbacks` replaces it as the "what does reading the wire back cost" row. The two
*baselines* these are compared against (the empty sketch and the CAN-only floor below) are
carried forward from the earlier measurement session, not re-measured; the toolchain is
pinned, so they are expected to hold.</sub>

Read those numbers with three corrections, or they will mislead you:

1. **The CAN transport is part of the image.** These absolute figures predate the
   direct-TWAI migration; remeasure `tools/footprint/` before quoting a transport or gate
   delta.
2. **`size_all` and `size_carminat` are byte-identical, and that is the result, not a
   defect.** Both build `examples/01_link_check`, which instantiates its own minimal
   `AffaDisplayBase` subclass and references no panel class, so `--gc-sections` removes
   every panel that is compiled but unused. **An unused panel costs zero, measurably** —
   which is exactly what the whole-body `#if` discipline in every optional `.cpp` exists to
   buy. The cost of *using* a panel shows up in the per-example table below.
3. The last two rows are *used* features, for the same reason: enabling
   `AFFA_ENABLE_ISOTP_RX` or `AFFA_ENABLE_MENU` in a build that never calls `onText()` or
   `getMenu()` costs nothing either. A gate's price is only visible once something names
   what it buys — which is what `tools/footprint/gate_probe` exists to force.

<sub>The project brief quoted 247 290 B for an empty sketch on this board. That figure does
not reproduce with the toolchain pinned in this repository (Arduino core 2.0.17 / platform
espressif32 6.13.0); 218 912 B is what a clean build measures here. Against 247 290 B the
deltas would read +18 826, +18 826, +16 946 and +35 214 B. The measured baselines above are
the ones this table uses.</sub>

Per-example, same board and core, all from real `pio run` output. **Both columns were
measured on the same day and the same toolchain**, "before" being the tree with
`src/carminat/Menu/` still in it and "after" being the tree immediately following — so the
Δ is the price of collapsing the two menu implementations into one, and nothing else.

<sub>**This is a historical snapshot of that one migration, not the current tree.** Both
columns are frozen at the day they were taken; the absolute figures have moved since
(`src/vpanel/` was deleted, `onText` was added, the marquee moved to `src/widget/`). For
current absolute numbers use the table above, which is re-measured. The `ex07_virtual_panel_c3`
row measures a build that no longer exists and is kept only so the Δ column stays a complete
account of the migration.</sub>

| Env | What it exercises | Flash before | Flash after | Δ | RAM before | RAM after | Δ |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `ex01_link_check` | core only, log level 4 | 266 124 B | 266 124 B | **0** | 16 380 B | 16 380 B | **0** |
| `ex02_carminat_text` | Carminat, no menu | 271 708 B | 271 708 B | **0** | 16 332 B | 16 332 B | **0** |
| `ex03_carminat_menu` | Carminat + `Menu` + pages | 274 672 B | 275 700 B | **+1 028 B** | 17 900 B | 18 028 B | **+128 B** |
| `ex04_updatelist_segment` | UpdateList 8-segment + marquee | 270 962 B | 270 962 B | **0** | 16 452 B | 16 452 B | **0** |
| `ex05_updatelist_menu` | UpdateList LCD variant | 270 866 B | 270 866 B | **0** | 16 492 B | 16 492 B | **0** |
| `ex06_counter_preempt` | Carminat, tap + preemption | 271 546 B | 271 546 B | **0** | 16 356 B | 16 356 B | **0** |
| `ex07_virtual_panel_c3` | Carminat + `proto/` + `vpanel/` | 282 504 B | 282 504 B | **0** | 24 396 B | 24 396 B | **0** |
| `ex08_radio_mitm` | Carminat + menu + subscriptions | 274 700 B | 275 740 B | **+1 040 B** | 17 756 B | 17 884 B | **+128 B** |
| `ex09_menu_widget` | one menu on three displays | 278 294 B | 276 904 B | **−1 390 B** | 19 876 B | 19 908 B | +32 B |
| `ex90_bench_ota` | web console + WiFi + ElegantOTA + twin | 899 032 B | 900 040 B | **+1 008 B** | 71 124 B | 71 236 B | **+112 B** |
| <sub>↑ `ex07` and `ex90`'s twin are gone; `ex90` measures 897 182 B / 70 204 B today</sub> | | | | | | | |
| `size_all` / `size_carminat` | every gate on | 266 116 B | 266 116 B | **0** | 16 380 B | 16 380 B | **0** |
| `size_min` | Carminat, everything optional off | 264 236 B | 264 236 B | **0** | 16 052 B | 16 052 B | **0** |

Read that table honestly: **removing the duplicate cost flash, it did not save it.** Every
build that does not use a menu is byte-identical — the deletion is free if you were not
paying for the menu anyway — but a Carminat build that *does* use one grew by **~1 kB flash
and 128 B RAM**. That is what generality costs: the deleted `Menu` had the geometry welded
in as compile-time constants and called `IPanel` directly, while `MenuModel` multiplies by a
`rowChars` it is handed and reaches the panel through a virtual `IMenuRenderer`, and
`CarminatDisplay` now holds an adapter object as well as a model. The one row that *shrank*
is `ex09_menu_widget`, by 1 390 B, because it stopped carrying its own copy of the Carminat
adapter and uses the library's — which is the same effect at a smaller scale, and the reason
the trade is still worth taking. A kilobyte is the price; one state machine instead of two,
so that a fix lands once, is what it buys. The project has already paid the other price: the
sync FSM was duplicated across `CarminatDisplay::tick()` and `UpdateListBase::tick()` and
**both copies carried the same two defects verbatim**.

A panel plus its rendering is ~5.5 kB over the bare core; the menu widget adds **4 380 B
flash and 1 696 B RAM** (turn `AFFA_MENU_MAX_ITEMS` / `AFFA_MENU_MAX_FIELDS` down if that
matters), which is why it is off by default; the inbound decoder behind
`AFFA_ENABLE_ISOTP_RX` adds **912 B / 384 B**. Both from the gate table below, which is the
instrument that measures gates rather than examples.
`ex90_bench_ota` is dominated by WiFi and the HTTP server and uses a 1.4 MB OTA partition.

#### What each gate is actually worth

The tables above measure *examples*, so a gate whose code the example never names is worth
zero there by construction. To measure the gates themselves, one probe build instantiates
**all three panels and calls every optional render**, so `--gc-sections` cannot remove a
feature the gate was supposed to remove. Reference build (`g_base`): **276 094 B flash /
19 988 B RAM**. Every row below is one flag flipped against that build, on the same board
and core. The harness is `platformio_footprint.ini` + `tools/footprint/gate_probe` — run
`pio run -c platformio_footprint.ini` to reproduce every number here, including the two
baselines above and the two `#error` guards, which are environments expected to *fail*.

Measured 2026-07-28, after the menu migration and after `src/vpanel/` was deleted. The two
gates that default **off** are measured from the other side (`=1`), because a `=0` row
against a reference build that already has them off measures nothing — which is exactly the
bug the previous `g_no_menu` row had.

| Flag | Flash | RAM | Symbol evidence in `firmware.elf` |
| --- | ---: | ---: | --- |
| `AFFA_ENABLE_ESP32CAN_LINK=0` | re-measure | re-measure | `Esp32CanLink` and direct-TWAI symbols gone |
| `AFFA_PANEL_CARMINAT=0` | −2 574 B | −1 168 B | every `CarminatDisplay::*` symbol gone |
| `AFFA_PANEL_UPDATELIST=0` (with `_MENU=0`) | −2 492 B | −2 560 B | every `UpdateList*` symbol gone |
| `AFFA_ENABLE_TRANSLITERATION=0` | −2 148 B | 0 | `affa::toAscii` gone (inlined bounded copy replaces it) |
| `AFFA_ENABLE_LOG=0` | −1 762 B | −8 B | `affa::detail::emit` gone, and with it every format string |
| `AFFA_MAX_SUBSCRIPTIONS=0` | −454 B | −960 B | `subscribe()` collapses from 0x9E to 4 bytes; the `Sub` table is gone |
| `AFFA_PANEL_UPDATELIST_MENU=0` | −368 B | −1 280 B | `UpdateListMenuDisplay::setText` gone |
| `AFFA_ENABLE_FULLSCREEN=0` | −326 B | 0 | `showFullscreenText` collapses from 0xCE to **4 bytes** |
| `AFFA_ENABLE_CONFIRMBOX=0` | −286 B | 0 | `showConfirmBox` 0xEC → 4 bytes |
| `AFFA_ENABLE_INFOPOPUP=0` | −268 B | 0 | `showInfoMenu` 0x52 + 0xAE lambda → 4 bytes |
| `AFFA_ENABLE_POPUP=0` | −252 B | 0 | `showPopupText` 0xC8 → 4 bytes |
| `AFFA_ENABLE_MENU=1` (default is `0`) | **+4 380 B** | **+1 696 B** | `affa::widget::MenuModel::*`, `MenuController::*`, `CarminatMenuRenderer::*` appear |
| `AFFA_ENABLE_ISOTP_RX=1` (default is `0` on target) | +912 B | +384 B | `isotp::Reassembler::onFrame`, `screen::menu/infoRow/windowText`, `AffaDisplayBase::pumpText` appear |

Three honest readings of that table:

* **The four screen gates are worth 252–326 B each, not kilobytes.** The gate replaces the
  builder with a four-byte `return NotSupported`, which is exactly what it promises and not
  much money. Turn them off for correctness (a panel that cannot do it should say so), not
  for space.
* **`AFFA_ENABLE_MENU=0` pulls in nothing — not even an empty `MenuModel`.** The 1 696 B of
  RAM is the model's storage, and it appears only on the `=1` side. The gate is the single
  most expensive thing in the library and it is off by default, which is the whole argument
  for the menu being a widget rather than protocol.
* **`AFFA_ENABLE_ISOTP_RX=1` used to measure +10 B, i.e. nothing**, because no shipped code
  path called the reassembler — the library declared a `Feature::RadioText` it could not
  deliver. `onText()` is that path, and the gate now costs what the decode is actually
  worth.

### Threading and the non blocking contract

* **No `delay()`, no busy-wait, and no `vTaskDelay()` in `core/`, `util/`, `proto/`,
  `widget/`, `link/` or either panel.** `IClock` exposes `millis()` and deliberately nothing
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
  sections. Any other context (an HTTP handler, a BLE callback, a second task) must post a
  request into a mailbox that the `poll()` task drains. `examples/90_bench_ota` does exactly
  this and is worth copying — **or set `AFFA_ENABLE_TASK=1` and let the library own both the
  task and the mailbox** (`examples/19_owned_task`, which needs neither), in which case
  `poll()` additionally *refuses* any caller that is not the owning task and counts it.
* **Callbacks fire from the `poll()` context**, never from the CAN driver task. State is
  committed *before* the callback that reports it, so a callback may call back into the
  library — render calls, `abortPending()`, `pressKey()`, `subscribe()` — but never `poll()`
  itself.
* **Pointers inside an `Event` are valid only for the duration of the callback.** They point
  at library-internal storage. Copy what you keep.
* **The frames we emit are frequency-independent; whether a transfer completes is not.**
  Calling `poll()` once per second and a million times per second produce the same frames in
  the same order with the same timing — nothing counts calls. But `AFFA_ACK_TIMEOUT_MS` and
  `AFFA_PEER_TIMEOUT_MS` are wall-clock deadlines evaluated *inside* `poll()`, so a late
  `poll()` does not delay a result, it **changes** it: `Ok` becomes `Timeout`, and an expired
  peer deadline tears down registration. Earlier revisions said "no minimum rate for
  correctness" without that second half, and it was read as licence to share the poll task.
  (docs/API.md §4.4.)
* **Since 0.3.0 the library can own the task, and on ESP32 it should.** `-D
  AFFA_ENABLE_TASK=1` compiles `src/rtos/`, creates a 2 ms task at priority 2, and makes
  every render callable **from any task** through `affa::rtos::AffaTask` — no mutex, no
  mailbox, no hand-off. `KeyCb` still fires synchronously inside `poll()`, so key latency is
  unchanged: it is bounded by the task period and nothing else. `poll()` refuses a caller
  that is not the owning task and counts it. Off by default, because turning it on changes
  which task your callbacks run on. **`examples/19_owned_task`** is the reference; docs/API.md
  §4b is the contract. `core/`, `util/`, `proto/` and `widget/` are untouched by it and still
  compile on the host against nothing but C++17.
* **There is no exception.** Earlier revisions offered one, `sendBlocking(ticket,
  timeoutMs)`, which spun on `poll()` until a ticket completed. It is gone: nothing in
  `src/`, `examples/` or `test/` ever called it, and a library whose headline promise is
  that it never blocks should not ship the one call that does. Wait on `onComplete()` — or
  on `EventKind::TxComplete` — from your own loop.

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
   pio test -e native            # 206 cases, ~14 s
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
   remember that **the panel opens the conversation** — nothing happens until it pings.
   Flash `examples/01_link_check` first: it names every known frame and, on a two-node bus,
   `txErr == 0` is the proof the panel is acknowledging you.

The same document also covers capturing your own traffic, diffing it against
`docs/WIRE-SPEC.md`, and adding a fourth panel family.

### Keep ownership of the controller

`Esp32CanLink` owns one ESP-IDF TWAI controller and its RX task. Do not call
`twai_driver_install`, `twai_start`, `twai_stop`, `twai_driver_uninstall`, mode changes,
or `twai_transmit` for that controller from application code. Use `setTxEnabled()` for
quiet periods and `setListenOnly()` only through the link; recovery is driven by the
library poll backoff.

`send()` is non-blocking: accepted means queued, not that the display processed the frame.
The display's protocol `0x74` reply is delivery proof; controller TX counters are
diagnostic only.

### Documents and tests

```
pio test -e native      # 244 host test cases across 14 suites, no hardware
pio run                 # all 15: two host environments plus 13 ESP32-C3 targets
```

| Document | What it is |
| --- | --- |
| [`docs/API.md`](docs/API.md) | The specification the implementation is written against. Where any other document disagrees with it, it wins. |
| [`docs/WIRE-SPEC.md`](docs/WIRE-SPEC.md) | The byte-level oracle: every frame layout, ready-to-paste golden vectors each tagged with the strongest witness that attests it, and the arithmetic for every frame count. **Where the code and this document disagree about a byte, the code is wrong.** |
| [`docs/PROTOCOL-NOTES.md`](docs/PROTOCOL-NOTES.md) | Provenance: every byte traced to a capture, an OEM log or a third-party reference, plus the open questions each phrased as the experiment that closes it. |
| [`docs/ESP32CAN-CONTRACT.md`](docs/ESP32CAN-CONTRACT.md) | Direct ESP-IDF TWAI ownership, RX/TX, lifecycle, and recovery contract. |
| [`docs/MENU-WIDGET.md`](docs/MENU-WIDGET.md) | The optional, display-agnostic menu: what `MenuModel` owns, what a renderer owns, the `MenuGeometry` fields, the `IMenuRenderer` contract, and a worked adapter for a display the library has never seen. |
| [`docs/PORTING.md`](docs/PORTING.md) | Moving an application off the old classes — and how to drop this library entirely, including which files are panel-specific and which are the reusable transport core. |
| [`docs/DEVELOPING-WITHOUT-HARDWARE.md`](docs/DEVELOPING-WITHOUT-HARDWARE.md) | The three tiers above, in full, plus capturing traffic and adding a panel. |

`core/`, `util/`, `link/LoopbackLink.h`, `proto/` and `widget/` must all compile for
`platform = native` with nothing but the C++17 standard library. If a change breaks that
build, the change is wrong, not the test. `<driver/twai.h>` is confined to
`src/link/Esp32CanLink.cpp`; the library uses no `ESP32_CAN` or `can_common` wrapper.

Licence: **MIT**, see [`LICENSE`](LICENSE).

---

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

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

static void onKey(affa::Key k, affa::KeyEdge e, void*) {
  if (k == affa::Key::Pause && e == affa::KeyEdge::Click) g_display.setText("PAUSED", 0);
}

void setup() {
  // Іменована структура: два піни неможливо переплутати на місці виклику, а їх плутали.
  g_link.begin(affa::CanPins{.rx = GPIO_NUM_3, .tx = GPIO_NUM_4}, 500000);
  g_display.onKey(&onKey, nullptr);
  g_display.begin();                                  // Carminat мовчить, доки не запитає панель
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
> Carminat/AFFA3 NAV відкриває сесію з нашого боку: після `begin()` бібліотека оголошує одну
> обмежену пару `3AF B9` + `3AF BA`, і вже на неї дисплей відповідає запитом
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

**Сторонній CAN-wrapper не потрібен.** `Esp32CanLink` напряму використовує вбудований у
ядро Arduino для ESP32 драйвер ESP-IDF TWAI. Транспорт володіє своєю RX-задачею та життєвим
циклом драйвера; бібліотека не встановлює `ESP32_CAN` чи `can_common`.

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

**Carminat/AFFA3 NAV: першими говоримо ми.** Після `begin()` бібліотека оголошує одну
обмежену пару `3AF B9` + `3AF BA` у тишу. Далі дисплей надсилає `0x3CF: 61 11 xx` за власним
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
| UpdateList / AFFA2, 8 сегментів | `affa::UpdateListDisplay` | `0x3DF` | `0x3CF` | `0x121`, `0x1B1` | `0x0A9` | `0x4A9` |
| UpdateList / AFFA2, моно-LCD | `affa::UpdateListMenuDisplay` | `0x3DF` | `0x3CF` | `0x121`, `0x1B1` | `0x0A9` | `0x4A9` |

ACK id завжди **обчислюється** як `funcId | 0x400`, і ніколи не береться з таблиці.
`0x0A9 | 0x400` — це `0x4A9`, а не `0x5A9`, бо біт 8 у `0x0A9` уже нульовий — унікально в цій
таблиці. Захардкоджений ACK id — це баг, який чекає на родину UpdateList.

Кожна родина колись мала **twin** — модель панелі, яка збирала те, що ви передали, і
відповідала ACK так, як це робить залізо. Twin-и **видалено**: це був код рівня застосунку,
який жив у бібліотеці. Те, для чого їх використовували, лишилося у двох менших частинах —
`setSelfAck()` дає ACK, коли панелі немає, а `AFFA_ENABLE_ISOTP_RX` купує збирач ISO-TP і
декодер екрана, якими можна прочитати шину назад.
Див. [Розробка без автомобіля](#розробка-без-автомобіля).

### Матриця можливостей

Питайте `display.supports(affa::Feature::X)` перед викликом; будь-який непідтримуваний виклик
повертає `Result::NotSupported`, а не робить вигляд, що все вдалося.

| Можливість | Carminat | UpdateList 8-сегм. | UpdateList LCD |
| --- | :---: | :---: | :---: |
| `Text` | так | так | так |
| `Time` | так | ні | ні |
| `Power` | так | так | так |
| `Menu` | якщо `AFFA_ENABLE_MENU` | ні | ні |
| `Popup` | якщо `AFFA_ENABLE_POPUP` | ні | ні |
| `Fullscreen` | якщо `AFFA_ENABLE_FULLSCREEN` | ні | ні |
| `ConfirmBox` | якщо `AFFA_ENABLE_CONFIRMBOX` | ні | ні |
| `InfoPopup` | якщо `AFFA_ENABLE_INFOPOPUP` | ні | ні |
| `KeyTx` | так (`0x1C1`) | так (`0x0A9`) | так (`0x0A9`) |
| `RadioText` | якщо `AFFA_ENABLE_ISOTP_RX` | так само | так само |

Одне чесне застереження, також зафіксоване в `docs/API.md` §6:

* `Feature::RadioText` повідомляє про **прапорець компіляції й нічого більше** — що збирач
  ISO-TP із `proto/` зібрано, тож вхідний текст *можна* відновити. **Жодна панель не
  передає відновлений текст застосунку**, і події для цього немає: ніколи не породжувану
  `EventKind::RadioText` видалено, а не залишено обіцянкою, якої бібліотека не виконує. Те,
  що UpdateList робить із вхідним `0x121`, — однокадрова перевірка на AUX, і вона
  повідомляється через захищений гак `UpdateListBase::onRadioText(bool)`; це шов для
  підкласу, і видалення його не зачепило. Застосунок, якому потрібен вхідний текст сьогодні,
  підписується на сирі кадри — див. `docs/PROTOCOL-NOTES.md` §8.

### Меню — це віджет, а не протокол

Насправді панель Carminat визначає рівно два виклики, і вони доступні беззастережно:
`showMenu(header, row0, row1, scrollByte)` — 96-байтовий екран `0x21/0x01` — і
`highlightItem(rowTag)`. Заголовок, два рядки, який із них підсвічено, які стрілки. Усе, що
вище (які пункти існують, який вибрано, як вікно ковзає по N пунктах, коли Select переходить
до наступного поля) — це стан інтерфейсу, про який панель нічого не знає; саме тому
`AFFA_ENABLE_MENU` типово дорівнює `0`.

Скажемо це прямо, бо решта розділу — про віджет, і це легко загубити: **`showMenu()` і
`highlightItem()` — це примітиви рівня протоколу, і вони не є необов'язковими.** Вони живуть
у `CarminatDisplay` поза всіма перемикачами меню. З `AFFA_ENABLE_MENU=0` — а це типове
значення — обидва так само компілюються, так само працюють і кладуть на шину ті самі байти;
втрачаєте ви `MenuModel`, `MenuController`, `IPage`, `nav()` і `getMenu()`, тобто одну
*думку* про те, як меню має поводитися. **Віджет необов'язковий; два виклики, на яких він
збудований, — ні.** Керуйте ними самі — і ви нічого цій бібліотеці не винні.

Якщо вам потрібен саме цей автомат, а не власний, то `src/widget/` тепер тримає його у формі,
**не привареній до геометрії однієї панелі**: `MenuModel` + `IMenuRenderer` + `MenuGeometry`.
Кількість рядків, символів у рядку і зациклення передаються ззовні, тож той самий алгоритм
працює на екрані меню Carminat 2 × 26, на інформаційному екрані 3 × 8 (`showInfoPopup`) і на
OLED 6 × 20. Модель оперує *індексом рядка* і віддає вже обрізаний і вже транслітерований
текст; теги рядків, кадр підсвічування і ціна перемальовування лишаються в адаптері, який ви
пишете самі — зазвичай менш ніж на тридцять рядків. Збирається на хості без Arduino, без CAN
і без заголовків панелі.

Читайте [`docs/MENU-WIDGET.md`](docs/MENU-WIDGET.md); запустіть `examples/09_menu_widget`, де
одне й те саме меню виводиться на три різні дисплеї. **Реалізація лишилася одна:**
`src/carminat/Menu/` — оригінал, приварений до панелі — видалено, `CarminatDisplay` керує
`MenuModel` через `affa::CarminatMenuRenderer`, а `getMenu()` зберіг назву й повертає
`widget::MenuModel&`. `affa::Menu` і `affa::MenuItem` лишилися як псевдоніми, тож наявний код
побудови пунктів збирається без змін; помітних відмінностей дві — `render()` повертає `void`
(вердикт панелі питайте в `menuRenderer().lastResult()`), а рядки обрізаються на переданих
26 символах. `MenuController` / `IPage` роблять те саме, що й раніше: стек сторінок і мапа
`(Key, KeyEdge)` → намір, тобто політика навігації, а не меню.

### Перемикачі конфігурації

`src/AffaConfig.h` — єдиний заголовок із перемикачами; кожен описано там разом із ціною і
наслідками. Задавайте їх у власних `build_flags` — заголовок лише підставляє значення за
замовчуванням.

| Макрос | Типово | Що вмикає |
| --- | :---: | --- |
| `AFFA_PANEL_CARMINAT` | `0`¹ | панель Carminat / AFFA3 |
| `AFFA_PANEL_UPDATELIST` | `0`¹ | восьмисегментна UpdateList |
| `AFFA_PANEL_UPDATELIST_MENU` | `0`¹ | моно-LCD різновид UpdateList (вмикає рядок вище) |
| `AFFA_PANEL_DEFAULT_ALL` | `0`¹ | явна згода «зібрати всі три панелі». Лише для першого знайомства і для довідкових збірок обсягу. |
| `AFFA_ENABLE_MENU` | **`0`** | `src/widget/`, `CarminatMenuRenderer`, `MenuController`, `IPage`, `nav()`, `getMenu()`. Найбільший опціональний блок, і **типово вимкнений**: меню — це віджет, а не протокол. `showMenu` / `highlightItem` доступні й з вимкненим меню. |
| ↳ `src/widget/` | *той самий прапорець* | `MenuModel` + `IMenuRenderer` + `MenuGeometry` — той самий алгоритм ковзного вікна, але кількість рядків, символів у рядку і зациклення стали **параметрами**, для будь-якого дисплея. Без панелі, тестується на хості, без купи після конструювання. Див. [`docs/MENU-WIDGET.md`](docs/MENU-WIDGET.md). |
| `AFFA_ENABLE_POPUP` | `1` | `showPopupText` / `hidePopup` |
| `AFFA_ENABLE_FULLSCREEN` | `1` | `showFullscreenText` / `hideFullscreenText` |
| `AFFA_ENABLE_CONFIRMBOX` | `1` | `showConfirmBox` (рівно на стелі в 113 байтів) |
| `AFFA_ENABLE_INFOPOPUP` | `1` | `showInfoPopup` / `hideInfoPopup` (три повідомлення) |
| `AFFA_ENABLE_TRANSLITERATION` | `1` | `toAscii` і його таблиця (~1.2 кБ). **0 — небезпечно**: UTF-8 тоді потрапляє на шину як є і малюється сміттям — це візуальна помилка, а не помилка компіляції. |
| `AFFA_ENABLE_LOG` | `1` | макроси `AFFA_LOG*`. При 0 жоден формат-рядок не потрапляє у флеш, тому ніколи не ховайте побічний ефект в аргументі логу. |
| `AFFA_LOG_LEVEL` | `3` | 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace. На етапі компіляції. |
| `AFFA_ENABLE_ESP32CAN_LINK` | `1` на Arduino, `0` на хості | `Esp32CanLink` і direct transport `<driver/twai.h>` |
| `AFFA_ENABLE_ISOTP_RX` | `0` на платі, `1` на хості | збирач ISO-TP, декодер екрана і `onText()`. Щоб читати канал, у який пише хтось інший; ролі радіо це не потрібно. |
| `AFFA_ENABLE_MARQUEE` | `1` | `widget::Marquee` і `setScrollText` / `setScrollActive` / `reassert` в `UpdateListDisplay`. Віджет, а не протокол — але маленький, а вісім комірок не вміщають назву треку. |
| `AFFA_TX_COALESCE` | `1` | «перемагає найновіше» в межах `RenderSlot`. 0 відтворює дефект «панель рахує далі після Pause». |
| `AFFA_TX_QUEUE_DEPTH` | `6` | слоти черги, приблизно `AFFA_MAX_PAYLOAD + 12` Б кожен. 6, а не 4, бо `showInfoPopup` — це три повідомлення, а перший виклик після ресинку тягне ще два зонди реєстрації. |
| `AFFA_MAX_PAYLOAD` | `113` | **межа протоколу, а не бюджет**: `8 + 15×7 = 113`, далі лічильник ISO-TP переповнюється. Нижче 96 меню Carminat повертає `TooLong`. |
| `AFFA_RX_RING_DEPTH` | `32` | степінь двійки. 32 × `sizeof(Frame)` = 448 Б; витримує паузу ~7 мс між `poll()` на завантаженій шині. |
| `AFFA_ACK_TIMEOUT_MS` | `2000` | дедлайн ACK на кадр; точно збігається зі старим блокуючим очікуванням |
| `AFFA_PEER_TIMEOUT_MS` | `5000` | тиша, після якої sync рветься. **Фактичне вікно — до цього значення плюс `AFFA_SYNC_INTERVAL_MS`**, бо watchdog перевіряється на такті heartbeat. Ніколи не опускайте нижче за найдовший запис у флеш: переривання TWAI не в IRAM, тож OTA чи запис NVS виглядає точно як панель, що замовкла. |
| `AFFA_SYNC_INTERVAL_MS` | `1000` | період heartbeat. Вважайте фіксованим: так показує захоплення шини. |
| `AFFA_MAX_SUBSCRIPTIONS` | `8` | слоти підписок Layer 1 |
| `AFFA_MENU_MAX_ITEMS` | `12` | місткість меню |
| `AFFA_MENU_MAX_FIELDS` | `3` | полів на пункт; `MenuItem` містить усі їх у собі — це основна частина RAM меню |
| `AFFA_MENU_ROW_MAX` | `32` | буфер відрендереного рядка |
| `AFFA_TEXT_MAX` | `64` | буфер тексту і біжучого рядка |

¹ **Не назвати жодної панелі — це помилка компіляції, а не значення за замовчуванням.** Усі
три прапорці типово `0`, і `AffaConfig.h` видає `#error`, коли всі три дорівнюють `0` — а це
рівно той стан, який лишає по собі помилка в написанні `-D AFFA_PANEL_CARMINET=1`, і єдиний
спосіб її упіймати (`-Wundef` тут безсилий: помилково названий макрос *визначено*, просто
його ніхто не читає). Якщо вам справді потрібні всі три — пишіть
`-D AFFA_PANEL_DEFAULT_ALL=1`; у цьому репозиторії так робить лише `size_all`. Хостовий
тестовий білд називає всі три явно.

**Кожен виклик, що повертає `Result`, позначено `[[nodiscard]]`.** Рендер, чий `Result` ви
відкинули, — це екран, який тихо не з'явився: `NoSync`, `QueueFull`, `TooLong` і
`NotSupported` з місця виклику виглядають так само, як успіх. Якщо ігноруєте свідомо —
скажіть це: `(void)display.setText("RENAULT", 0);`.

### Обсяг прошивки

ESP32-C3 (`board = esp32-c3-devkitm-1`, ядро Arduino 2.0.17), release, прямо з виводу
`pio run`. **База, виміряна на тому самому тулчейні**: порожній скетч зі `setup()`/`loop()` —
**218 912 Б** флеш / **13 476 Б** RAM.

| Збірка | Флеш | Δ до порожнього скетча | RAM | Δ до порожнього скетча |
| --- | ---: | ---: | ---: | ---: |
| `size_all` — усі панелі увімкнені (`AFFA_PANEL_DEFAULT_ALL=1`) | 266 078 Б | +47 166 Б | 16 380 Б | +2 904 Б |
| `size_carminat` — лише Carminat | 266 078 Б | +47 166 Б | 16 380 Б | +2 904 Б |
| `size_min` — Carminat без меню/popup/fullscreen/confirm/info, без транслітерації, без логу і підписок | 264 198 Б | +45 286 Б | 16 052 Б | +2 576 Б |
| `ex10_callbacks` — Carminat без меню, **плюс `AFFA_ENABLE_ISOTP_RX`** (`onText`) | 271 776 Б | +52 864 Б | 16 460 Б | +2 984 Б |
| `ex09_menu_widget` — Carminat **плюс віджет меню**, у вжитку | 276 826 Б | +57 914 Б | 19 892 Б | +6 416 Б |

<sub>Перевиміряно чистим `pio run` 2026-07-28, після видалення `src/vpanel/`. Рядок
`ex07_virtual_panel_c3`, який тут стояв, вимірював збірку, якої більше немає; його замінює
`ex10_callbacks` як рядок «скільки коштує читати шину назад». Дві *бази*, з якими їх
порівнюють (порожній скетч і підлога «лише CAN» нижче), перенесені з попередньої сесії
вимірювань і не перевимірювалися; тулчейн зафіксовано, тож вони мають триматися.</sub>

Ці числа треба читати з трьома поправками, інакше вони введуть в оману:

1. **CAN transport є частиною образу.** Ці абсолютні цифри передують переходу на
   direct-TWAI; перед цитуванням різниці transport або gate перевиміряйте
   `tools/footprint/`.
2. **`size_all` і `size_carminat` байт у байт однакові — і це результат, а не дефект.** Обидві
   збирають `examples/01_link_check`, який створює власний мінімальний нащадок
   `AffaDisplayBase` і не згадує жодного класу панелі, тож `--gc-sections` викидає кожну
   скомпільовану, але невикористану панель. **Невикористана панель коштує нуль, і це
   виміряно** — саме заради цього кожен опціональний `.cpp` загорнуто у `#if` цілком. Ціна
   *використання* панелі видно в таблиці прикладів нижче.
3. Два останні рядки — це можливості, які *використовують*, з тієї ж причини: увімкнути
   `AFFA_ENABLE_ISOTP_RX` чи `AFFA_ENABLE_MENU` у збірці, яка ніколи не викликає `onText()`
   чи `getMenu()`, теж коштує нуль. Ціну прапорця видно лише тоді, коли щось називає те, що
   він купує — саме для цього існує `tools/footprint/gate_probe`.

<sub>У технічному завданні для порожнього скетча на цій платі наводилася цифра 247 290 Б.
Вона не відтворюється на тулчейні, зафіксованому в цьому репозиторії (ядро Arduino 2.0.17 /
платформа espressif32 6.13.0); чиста збірка тут дає 218 912 Б. Відносно 247 290 Б дельти
були б +18 826, +18 826, +16 946 і +35 214 Б. Таблиця вище користується виміряними базами.</sub>

По прикладах, та сама плата і ядро, усе з реального виводу `pio run`. **Обидві колонки
виміряні того самого дня тим самим тулчейном**: «до» — дерево, у якому ще був
`src/carminat/Menu/`, «після» — дерево одразу по тому. Тож Δ — це ціна зведення двох
реалізацій меню в одну, і більше нічого.

<sub>**Це історичний зріз саме тієї міграції, а не поточне дерево.** Обидві колонки
заморожені на день вимірювання; абсолютні числа відтоді змінилися (видалено `src/vpanel/`,
додано `onText`, marquee переїхав у `src/widget/`). Поточні абсолютні числа — у таблиці
вище, вона перевиміряна. Рядок `ex07_virtual_panel_c3` вимірює збірку, якої вже немає, і
залишений лише щоб колонка Δ була повним звітом про міграцію.</sub>

| Env | Що задіює | Флеш до | Флеш після | Δ | RAM до | RAM після | Δ |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `ex01_link_check` | лише ядро, рівень логу 4 | 266 124 Б | 266 124 Б | **0** | 16 380 Б | 16 380 Б | **0** |
| `ex02_carminat_text` | Carminat без меню | 271 708 Б | 271 708 Б | **0** | 16 332 Б | 16 332 Б | **0** |
| `ex03_carminat_menu` | Carminat + `Menu` + сторінки | 274 672 Б | 275 700 Б | **+1 028 Б** | 17 900 Б | 18 028 Б | **+128 Б** |
| `ex04_updatelist_segment` | UpdateList 8 сегментів + біжучий рядок | 270 962 Б | 270 962 Б | **0** | 16 452 Б | 16 452 Б | **0** |
| `ex05_updatelist_menu` | різновид UpdateList LCD | 270 866 Б | 270 866 Б | **0** | 16 492 Б | 16 492 Б | **0** |
| `ex06_counter_preempt` | Carminat, tap і витіснення | 271 546 Б | 271 546 Б | **0** | 16 356 Б | 16 356 Б | **0** |
| `ex07_virtual_panel_c3` | Carminat + `proto/` + `vpanel/` | 282 504 Б | 282 504 Б | **0** | 24 396 Б | 24 396 Б | **0** |
| `ex08_radio_mitm` | Carminat + меню + підписки | 274 700 Б | 275 740 Б | **+1 040 Б** | 17 756 Б | 17 884 Б | **+128 Б** |
| `ex09_menu_widget` | одне меню на трьох дисплеях | 278 294 Б | 276 904 Б | **−1 390 Б** | 19 876 Б | 19 908 Б | +32 Б |
| `ex90_bench_ota` | вебконсоль + WiFi + ElegantOTA + twin | 899 032 Б | 900 040 Б | **+1 008 Б** | 71 124 Б | 71 236 Б | **+112 Б** |
| `size_all` / `size_carminat` | усі перемикачі ввімкнені | 266 116 Б | 266 116 Б | **0** | 16 380 Б | 16 380 Б | **0** |
| `size_min` | Carminat, усе необов'язкове вимкнено | 264 236 Б | 264 236 Б | **0** | 16 052 Б | 16 052 Б | **0** |

Читайте цю таблицю чесно: **видалення дубліката коштувало флешу, а не зекономило його.**
Кожна збірка, яка не використовує меню, побайтово ідентична — видалення безкоштовне, якщо ви
й так за меню не платили, — але збірка Carminat, яка меню *використовує*, зросла на **~1 кБ
флеш і 128 Б RAM**. Саме стільки коштує узагальнення: у видаленому `Menu` геометрія була
вварена як константи часу компіляції, і він викликав `IPanel` напряму, тоді як `MenuModel`
множить на переданий йому `rowChars` і дістається панелі через віртуальний `IMenuRenderer`, а
`CarminatDisplay` тепер тримає ще й об'єкт-адаптер. Єдиний рядок, який *зменшився*, —
`ex09_menu_widget`, на 1 390 Б, бо приклад перестав носити власну копію адаптера Carminat і
користується бібліотечним, — це той самий ефект у меншому масштабі й причина, чому обмін усе
одно вигідний. Кілобайт — це ціна; одна машина станів замість двох, щоб виправлення
застосовувалося один раз, — це те, що за неї купують. Іншу ціну проєкт уже заплатив: FSM
синхронізації був продубльований у `CarminatDisplay::tick()` та `UpdateListBase::tick()`, і
**обидві копії несли ті самі два дефекти дослівно**.

Панель разом із рендером — це ~5.5 кБ понад голе ядро; віджет меню додає **4 380 Б флеш і
1 696 Б RAM** (зменшіть `AFFA_MENU_MAX_ITEMS` / `AFFA_MENU_MAX_FIELDS`, якщо це критично),
і саме тому його типово вимкнено; вхідний декодер за `AFFA_ENABLE_ISOTP_RX` додає
**912 Б / 384 Б**. Обидва — з таблиці перемикачів нижче. `ex90_bench_ota` визначається
переважно WiFi і HTTP-сервером і використовує розділ OTA на 1.4 МБ.

#### Скільки насправді коштує кожен перемикач

Таблиці вище міряють *приклади*, тому перемикач, чий код приклад ніколи не називає, там
коштує нуль за побудовою. Щоб виміряти самі перемикачі, окрема пробна збірка створює **усі
три панелі й викликає кожен опціональний рендер**, аби `--gc-sections` не могла викинути те,
що мав викинути перемикач. Опорна збірка: **278 936 Б флеш / 21 588 Б RAM**. Кожен рядок —
це один прапорець, перемкнутий відносно неї, на тій самій платі і тому самому ядрі. Стенд —
це `platformio_footprint.ini` плюс `tools/footprint/gate_probe`; `pio run -c
platformio_footprint.ini` відтворює всі числа звідси, включно з обома базами вище і двома
охоронними `#error`, чиї середовища мають *не* збиратися.

Виміряно 2026-07-28, після міграції меню і після видалення `src/vpanel/`. Два перемикачі,
типово **вимкнені**, виміряні з іншого боку (`=1`): рядок `=0` відносно довідкової збірки, у
якій вони вже вимкнені, не вимірює нічого — саме ця вада була в старому `g_no_menu`.

| Прапорець | Флеш | RAM | Підтвердження в символах `firmware.elf` |
| --- | ---: | ---: | --- |
| `AFFA_ENABLE_ESP32CAN_LINK=0` | перевиміряти | перевиміряти | зникають `Esp32CanLink` і direct-TWAI symbols |
| `AFFA_PANEL_CARMINAT=0` | −2 574 Б | −1 168 Б | зникають усі `CarminatDisplay::*` |
| `AFFA_PANEL_UPDATELIST=0` (разом із `_MENU=0`) | −2 492 Б | −2 560 Б | зникають усі `UpdateList*` |
| `AFFA_ENABLE_TRANSLITERATION=0` | −2 148 Б | 0 | зникає `affa::toAscii` (його заміняє вбудована обмежена копія) |
| `AFFA_ENABLE_LOG=0` | −1 762 Б | −8 Б | зникає `affa::detail::emit`, а з ним усі формат-рядки |
| `AFFA_MAX_SUBSCRIPTIONS=0` | −454 Б | −960 Б | `subscribe()` стискається з 0x9E до 4 байтів; таблиці `Sub` немає |
| `AFFA_PANEL_UPDATELIST_MENU=0` | −368 Б | −1 280 Б | зникає `UpdateListMenuDisplay::setText` |
| `AFFA_ENABLE_FULLSCREEN=0` | −326 Б | 0 | `showFullscreenText` з 0xCE до **4 байтів** |
| `AFFA_ENABLE_CONFIRMBOX=0` | −286 Б | 0 | `showConfirmBox` 0xEC → 4 байти |
| `AFFA_ENABLE_INFOPOPUP=0` | −268 Б | 0 | `showInfoMenu` 0x52 + лямбда 0xAE → 4 байти |
| `AFFA_ENABLE_POPUP=0` | −252 Б | 0 | `showPopupText` 0xC8 → 4 байти |
| `AFFA_ENABLE_MENU=1` (типово `0`) | **+4 380 Б** | **+1 696 Б** | з'являються `affa::widget::MenuModel::*`, `MenuController::*`, `CarminatMenuRenderer::*` |
| `AFFA_ENABLE_ISOTP_RX=1` (на платі типово `0`) | +912 Б | +384 Б | з'являються `isotp::Reassembler::onFrame`, `screen::menu/infoRow/windowText`, `AffaDisplayBase::pumpText` |

Три чесні висновки з цієї таблиці:

* **Чотири «екранні» перемикачі коштують 252–326 байтів кожен, а не кілобайти.** Перемикач
  заміняє білдер чотирибайтним `return NotSupported` — рівно те, що обіцяно, і небагато.
  Вимикайте їх заради коректності (панель, яка чогось не вміє, має так і казати), а не заради
  місця.
* **`AFFA_ENABLE_MENU=0` не тягне нічого — навіть порожнього `MenuModel`.** 1 696 Б RAM — це
  сховище моделі, і воно з'являється лише з боку `=1`. Це найдорожча річ у бібліотеці, і
  типово вона вимкнена — власне, це і є весь аргумент на користь того, що меню віджет, а не
  протокол.
* **`AFFA_ENABLE_ISOTP_RX=1` колись давав +10 Б**, тобто нічого, бо жоден робочий шлях не
  викликав збирач — бібліотека оголошувала `Feature::RadioText`, якої не могла надати.
  `onText()` — це той шлях, і тепер перемикач коштує рівно стільки, скільки декодування
  справді варте.

### Багатозадачність і неблокуючий контракт

* **Ніякого `delay()`, активного очікування чи `vTaskDelay()` — ні в `core/`, `util/`,
  `proto/`, `widget/`, `link/`, ні в жодній із панелей.** `IClock` віддає `millis()` і
  навмисно більше нічого. **Єдиний виняток, доданий у 0.3.0, — `src/rtos/AffaTask.cpp`**:
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
  Будь-який інший контекст (HTTP-обробник, BLE callback, друга задача) має покласти запит у
  поштову скриньку, яку розгрібає задача з `poll()`. `examples/90_bench_ota` робить саме так —
  його варто копіювати. **Або ввімкніть `AFFA_ENABLE_TASK=1`**, і бібліотека візьме на себе
  і задачу, і скриньку (`examples/19_owned_task`, якому не потрібне ні те, ні те); тоді
  `poll()` ще й **відмовляє** будь-якому викликачеві, що не є задачею-власником.
* **Callback-и викликаються з контексту `poll()`**, ніколи із задачі драйвера CAN. Стан
  фіксується *до* того callback-у, який про нього повідомляє, тож із callback-у можна
  викликати бібліотеку далі — рендери, `abortPending()`, `pressKey()`, `subscribe()` — але
  ніколи сам `poll()`.
* **Вказівники всередині `Event` дійсні лише на час виконання callback-у.** Вони вказують на
  внутрішню пам'ять бібліотеки. Копіюйте те, що зберігаєте.
* **Кадри, які ми надсилаємо, не залежать від частоти виклику; а от чи завершиться передача —
  залежить.** Раз на секунду і мільйон разів на секунду дають ті самі кадри в тому самому
  порядку з тим самим таймінгом — ніщо не рахує виклики. Але `AFFA_ACK_TIMEOUT_MS` і
  `AFFA_PEER_TIMEOUT_MS` — це дедлайни за реальним часом, які перевіряються *всередині*
  `poll()`, тож запізнілий `poll()` не затримує результат, а **змінює** його: `Ok` стає
  `Timeout`, а прострочений дедлайн однолітка зносить реєстрацію. У ранніх редакціях була
  лише перша половина, і це читалося як дозвіл ділити задачу опитування. (docs/API.md §4.4.)
* **Починаючи з 0.3.0 бібліотека може володіти задачею — і на ESP32 має.** `-D
  AFFA_ENABLE_TASK=1` вмикає `src/rtos/`, створює задачу з періодом 2 мс і пріоритетом 2, і
  робить кожен рендер доступним **з будь-якої задачі** через `affa::rtos::AffaTask` — без
  м'ютексів, без поштової скриньки, без передавання. `KeyCb` і далі спрацьовує синхронно
  всередині `poll()`, тож затримка клавіші не змінилася: її межа — період задачі й нічого
  більше. `poll()` відмовляє викликачеві, який не є задачею-власником, і рахує такі виклики.
  Вимкнено за замовчуванням, бо ввімкнення змінює, у якій задачі працюють ваші колбеки.
  **`examples/19_owned_task`** — взірець; docs/API.md §4b — контракт.
* **Винятку немає.** У ранніх редакціях він був — `sendBlocking(ticket, timeoutMs)`, який
  крутив `poll()`, доки квиток не завершиться. Його видалено: його не викликало ніщо в
  `src/`, `examples/` чи `test/`, а бібліотека, головна обіцянка якої — ніколи не блокувати,
  не має постачати єдиний виклик, що блокує. Чекайте через `onComplete()` або
  `EventKind::TxComplete` у власному циклі.

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
   pio test -e native            # 206 випадків, ~14 с
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
   `(rx, tx)` і про те, що **розмову починає панель** — доки вона не подасть голос, не
   станеться нічого. Спершу прошийте `examples/01_link_check`: він називає кожен відомий
   кадр, і на шині з двох вузлів `txErr == 0` є доказом того, що панель вас підтверджує.

Той самий документ описує, як зняти власний трафік, як звірити його з `docs/WIRE-SPEC.md` і
як додати четверту родину панелей.

### Власник контролера

`Esp32CanLink` володіє одним контролером ESP-IDF TWAI і його RX-задачею. Не викликайте з
коду застосунку `twai_driver_install`, `twai_start`, `twai_stop`,
`twai_driver_uninstall`, зміну режиму або `twai_transmit` для цього контролера. Для тихого
періоду використовуйте `setTxEnabled()`, а `setListenOnly()` — лише через link; recovery
веде poll-backoff бібліотеки.

`send()` не блокує: accepted означає «поставлено в чергу», а не «дисплей опрацював кадр».
Відповідь протоколу дисплея `0x74` є доказом доставки; лічильники TX контролера лише
діагностичні.

### Документи і тести

```
pio test -e native      # 200 хостових тестів у 13 наборах, без заліза
pio run                 # усі 15: два хостові середовища плюс 13 цілей ESP32-C3
```

| Документ | Що це |
| --- | --- |
| [`docs/API.md`](docs/API.md) | Специфікація, під яку написано реалізацію. Якщо будь-який інший документ їй суперечить — перемагає вона. |
| [`docs/WIRE-SPEC.md`](docs/WIRE-SPEC.md) | Побайтовий оракул: усі розкладки кадрів, готові до вставки золоті вектори з позначкою найсильнішого свідка і арифметика для кожної кількості кадрів. **Якщо код і цей документ розходяться щодо байта — помиляється код.** |
| [`docs/PROTOCOL-NOTES.md`](docs/PROTOCOL-NOTES.md) | Походження: кожен байт зведено до захоплення шини, OEM-логу або сторонньої реалізації, плюс відкриті питання, кожне сформульоване як експеримент, що його закриває. |
| [`docs/ESP32CAN-CONTRACT.md`](docs/ESP32CAN-CONTRACT.md) | Контракт direct ESP-IDF TWAI: володіння, RX/TX, життєвий цикл і recovery. |
| [`docs/MENU-WIDGET.md`](docs/MENU-WIDGET.md) | Опціональне меню, незалежне від дисплея: що належить `MenuModel`, що — рендереру, поля `MenuGeometry`, контракт `IMenuRenderer` і готовий приклад адаптера для дисплея, якого бібліотека ніколи не бачила. |
| [`docs/PORTING.md`](docs/PORTING.md) | Як перевести застосунок зі старих класів — і як відмовитися від цієї бібліотеки взагалі, включно з тим, які файли специфічні для панелі, а які є придатним до повторного використання транспортним ядром. |
| [`docs/DEVELOPING-WITHOUT-HARDWARE.md`](docs/DEVELOPING-WITHOUT-HARDWARE.md) | Три рівні вище, докладно, плюс зняття трафіку і додавання панелі. |

`core/`, `util/`, `link/LoopbackLink.h`, `proto/` і `widget/` мають збиратися для
`platform = native` з нічим, окрім стандартної бібліотеки C++17. Якщо зміна ламає цю збірку —
неправа зміна, а не тест. `<driver/twai.h>` обмежений
`src/link/Esp32CanLink.cpp`; бібліотека не використовує wrapper `ESP32_CAN` чи `can_common`.

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
