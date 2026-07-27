# AffaDisplay — public API and configuration surface

Version of this contract: **0.1.0**. Namespace: `affa`. Language: **C++17**, built with
`-fno-exceptions`, `-fno-rtti`. Public signatures use `const char*`; Arduino `String`
never appears in a public signature and never appears in `core/` or `util/` at all.

This document is the specification the core implementer codes against. Every type
referenced here is declared here. Where a declaration in this document and a comment in
the source disagree, this document wins until it is amended.

Companion documents:

* `docs/WIRE-SPEC.md` — the byte-level frame layouts and where each byte was observed.
* `docs/ESP32CAN-CONTRACT.md` — the prohibition list for the collin80/esp32_can driver.

---

## 0. The four defects this API exists to make impossible

Everything unusual in the shapes below traces back to one of these. They are stated
first so that a reviewer can check the design against its purpose.

1. **The sync watchdog counted `tick()` calls, not milliseconds.** `static int8_t
   timeout = SYNC_TIMEOUT` decremented once per call meant "five seconds" only if the
   caller happened to tick at exactly 1 Hz. From a free-running loop it expired in
   milliseconds, tore down `FUNCSREG`, and restarted the handshake forever.
   *Fix in the API:* `IClock` exposes `millis()` and nothing else, all periodic
   behaviour is a wall-clock deadline inside `poll()`, and `poll()` is specified as
   frequency-independent (§4.4). A host test enforces it.

2. **A 2000 ms blocking ACK wait sat inside the only path that could deliver the ACK.**
   `affa3_do_send` spun on `delayMs(1)` waiting for a frame that only arrived if
   something drained RX — and the drain was downstream of the spin.
   *Fix in the API:* `ICanLink` is a **pull** port (`recv(Frame&)`), the transmit path
   is a state machine advanced by `poll()`, and there is **no `delayMs()` anywhere in
   the library**. A send cannot wait, so a send cannot deadlock.

3. **`delay(100)` in the sync-request branch**, and a duplicated copy of the whole sync
   FSM in `CarminatDisplay::tick()` and `UpdateListBase::tick()` — so both copies
   carried both defects. *Fix in the API:* one FSM in `AffaDisplayBase`, parameterised
   by `SyncProfile`; the `delay(100)` is deleted, not replaced.

4. **A queued render could not be superseded, and a key could not overtake one.** This
   defect has not bitten yet only because the extracted code had no queue at all — it
   blocked instead. Turning the blocking send into a queue *introduces* the defect
   unless it is designed out on day one: an application rendering a counter at 10 Hz
   leaves a backlog of stale values, and the panel visibly keeps counting for a second
   after the user pressed Pause and the library correctly received the key. It reads
   like a key-handling latency bug and is really a queueing bug.
   *Fix in the API:* `poll()` drains RX and delivers keys **strictly before** it pumps
   the transmit FSM (§3b.3); a render supersedes a queued-but-not-yet-started render of
   the same `RenderSlot` instead of stacking behind it (§3b.4); and preemption is
   explicit through `abortPending()` and `Priority::Urgent` (§3b.5). All three are
   specified as testable guarantees, not as intentions.

---

## 1. Header inventory

`Host` = must compile for `platform = native` with nothing but the C++17 standard
library (`<cstdint>`, `<cstddef>`, `<cstring>`, `<atomic>`, `<type_traits>`). No
`<Arduino.h>`, no `<esp32_can.h>`, no FreeRTOS headers, no `String`, no `std::vector`,
no `std::function`, no heap after `begin()`.

| File | Contents | May include | Host |
| --- | --- | --- | --- |
| `src/AffaDisplay.h` | Umbrella. The only header a consumer includes. Pulls `AffaConfig.h`, all of `core/`, `util/`, and — each behind its own gate — the selected panels and links. Declares no types of its own. | everything below | yes |
| `src/AffaConfig.h` | Every `#define` gate and sizing knob. Includes nothing. Included first by every other file in the library. | — | yes |
| `core/AffaTypes.h` | `Frame`, `Key`, `KeyEdge`, `Result`, `SyncState` (+ bit ops), `Feature`, `NavCommand`, `TxTicket`, `RenderSlot`, `Priority`, `TxOptions`, `Stats`. | `AffaConfig.h`, `<cstdint>`, `<cstddef>`, `<type_traits>` | yes |
| `core/ICanLink.h` | `ICanLink`. | `AffaTypes.h` | yes |
| `core/IClock.h` | `IClock`. | `<cstdint>` | yes |
| `core/IPanel.h` | `IPanel` — the four-primitive rendering port an `IPage` and `CarminatMenuRenderer` draw through. `widget::MenuModel` does **not** see it. | `AffaTypes.h` | yes |
| `core/IDisplay.h` | `IDisplay` — the panel-agnostic surface an application holds a reference to. | `AffaTypes.h` | yes |
| `core/AffaConstants.h` | Protocol constants shared by all panels: `kPacketLength`, `kKeyHoldMask`, `kReplyFlag`, ISO-TP opcodes, ACK bytes. No panel-specific IDs. | `<cstdint>` | yes |
| `core/AffaSyncProfile.h` | `SyncProfile`; the two profile constants live in the panel folders, not here. | `<cstdint>` | yes |
| `core/AffaRing.h` | `AffaRing<T,N>` — lock-free single-producer/single-consumer ring. | `<cstdint>`, `<atomic>` | yes |
| `core/AffaDisplayBase.{h,cpp}` | The sync FSM, the ISO-TP transmit FSM, the RX drain, the key decoder, `pressKey`/`nav`, the three-layer observation seam (tap, subscription table, event sink), the capability defaults. | `core/*`, `util/*` | yes |
| `util/AffaLog.h` | `ILogSink`, `AFFA_LOG*` macros, level gating. | `AffaConfig.h`, `<cstdarg>` | yes |
| `util/AffaLog.cpp` | Formatter + sink dispatch. **Entire body gated on `AFFA_ENABLE_LOG`.** | `<cstdio>` | yes |
| `util/AffaText.{h,cpp}` | `toAscii`, `normalizeTitle`. Pure C API, no allocation. **`.cpp` body gated on `AFFA_ENABLE_TRANSLITERATION`.** | `<cstdint>`, `<cstddef>`, `<cstring>` | yes |
| `link/LoopbackLink.h` | `LoopbackLink` — header-only test double: records TX, injects RX, optional synthetic ACK. | `core/*` | yes |
| `link/Esp32CanLink.h` | `CanPins`, `Esp32CanLink`. Includes `<driver/gpio.h>` **only** for `gpio_num_t`. Entire body gated on `AFFA_ENABLE_ESP32CAN_LINK`. | `core/*`, `<driver/gpio.h>` | no |
| `link/Esp32CanLink.cpp` | **The only file in the library permitted to `#include <esp32_can.h>`.** Driver bring-up, the general-callback trampoline, the software TX gate. Entire body gated. | `<esp32_can.h>` | no |
| `proto/IsoTp.{h,cpp}` | `IsoTp::fragment()` (the transmit layout, shared with the TX FSM) and `IsoTp::Reassembler` (the receive direction). Entire `.cpp` body gated on `AFFA_ENABLE_ISOTP_RX`. | `core/AffaTypes.h` | yes |
| `proto/ScreenModel.h` | `ScreenModel` — the decoded "what is on the panel" state. A plain aggregate, no methods beyond `clear()`. Header-only, so it costs nothing unless something instantiates it. | `<cstdint>` | yes |
| `proto/ScreenDecode.{h,cpp}` | Payload offsets and `menu()` / `segText()` / `frame()` / `asciiz()` — reassembled bytes → `ScreenModel`. Same gate as `IsoTp.cpp`. | `ScreenModel.h`, `core/AffaTypes.h` | yes |
| `widget/Marquee.{h,cpp}` | `MarqueeGeometry` + `Marquee` — a scrolling text window, no panel knowledge. Gated on `AFFA_ENABLE_MARQUEE`. | `AffaConfig.h`, `util/AffaText.h` | yes |
| `carminat/CarminatConstants.h` | `0x3AF`, `0x3CF`, `0x151`, `0x1F1`, `0x1C1`, filler `0x00`, scroll-indicator values, the Carminat `SyncProfile` instance. | `core/*` | yes |
| `carminat/CarminatDisplay.{h,cpp}` | Carminat frame builders: `setText`, `setTime`, `showMenu`, `highlightItem`, popup/fullscreen/confirm/info, key decode for `0x1C1`. Gated on `AFFA_PANEL_CARMINAT`. | `core/*`, `util/*`, `<Arduino.h>` permitted in the **.cpp only** | `.h` yes, `.cpp` yes if it avoids `<Arduino.h>` — it must |
| `widget/MenuGeometry.h` | `MenuGeometry` — `rows`, `rowChars`, `wrap`. The shape of the display, injected. Gated on `AFFA_ENABLE_MENU`. | `AffaConfig.h` | yes |
| `widget/IMenuRenderer.h` | The panel seam: `beginFrame` / `row` / `endFrame`, plus the non-pure `highlightOnly`. Same gate. | `AffaConfig.h` | yes |
| `widget/MenuModel.{h,cpp}` | `FieldType`, `Field`, `MenuItem`, `MenuModel` — the sliding-window menu state machine, display-agnostic. Fixed-capacity, no `String`, no `vector`, no `std::function`, **no panel header**. Gated on `AFFA_ENABLE_MENU` alone: it is not panel code. | `AffaConfig.h`, `util/*` | yes |
| `carminat/CarminatMenuRenderer.{h,cpp}` | The `IMenuRenderer` for this panel: 2 x 26 geometry, the `0x7E`/`0x7F` row tags, `showMenu` + `highlightItem`, the cheap `highlightOnly` path, and the `Result` the model does not carry. Gated on `AFFA_ENABLE_MENU && AFFA_PANEL_CARMINAT`. | `core/*`, `widget/*` | yes |
| `carminat/MenuController.{h,cpp}` | Page stack + key routing (page first, then menu, then fall through to the application callback). Owns the `(Key, KeyEdge)` → `MenuModel` intent map. Gated on `AFFA_ENABLE_MENU && AFFA_PANEL_CARMINAT`. | `core/*`, `IPage.h`, `widget/MenuModel.h` | yes |
| `carminat/IPage.h` | `IPage` — `onEnter/onExit/onTick/handleKey`. | `core/AffaTypes.h` | yes |
| `updatelist/UpdateListConstants.h` | `0x3DF`, `0x3CF`, `0x121`, `0x1B1`, `0x0A9`, filler `0x81`, the UpdateList `SyncProfile` instance. | `core/*` | yes |
| `updatelist/UpdateListBase.{h,cpp}` | Shared AFFA2 behaviour: `setPower`, `0x121` radio-text sniffing. Gated on `AFFA_PANEL_UPDATELIST`. | `core/*`, `util/*` | yes |
| `updatelist/UpdateListDisplay.{h,cpp}` | The 8-segment panel; non-blocking title scroll driven from `poll()`. Same gate. | as above | yes |
| `updatelist/UpdateListMenuDisplay.{h,cpp}` | The LCD variant; different `setText` channel/location encoding. Gated on `AFFA_PANEL_UPDATELIST_MENU`. | as above | yes |

**Enforced rules.**

* `<esp32_can.h>` appears exactly once in the repository, in `link/Esp32CanLink.cpp`.
  A CI grep asserts this. The original code's `Menu.h` included it for `CAN_FRAME`;
  the port removes that include and the `handleMessage(const CAN_FRAME&)` method with it.
* `<Arduino.h>` is permitted only in `link/Esp32CanLink.cpp` and in panel `.cpp` files,
  and even there only if something genuinely needs it. Prefer `<cstring>`/`<cstdio>`.
  `AuxModeTracker` and `Menu` used to include it; the port did not, and `AuxModeTracker`
  is gone entirely (§7b.7b).
* `core/` and `util/` are compiled by `test/` for `platform = native`. If a change
  breaks that build, the change is wrong, not the test.
* `proto/` and `widget/` are host-compilable for the same reason and a stronger one: the
  decoder is the test oracle and the widgets have no panel to need (§2.14).
  A `<Arduino.h>` or a driver type anywhere in either folder destroys their only
  purpose. They talk to `ICanLink` and `IClock` and to nothing else.

---

## 2. Complete declarations

### 2.1 `core/AffaTypes.h`

```cpp
#pragma once
#include "../AffaConfig.h"   // AFFA_TX_COALESCE, for the TxOptions default
#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace affa {

// Portable CAN frame. Deliberately not the driver's CAN_FRAME: this type crosses
// every seam in the library, including the ones compiled for the host.
struct Frame {
  uint32_t id   = 0;
  uint8_t  len  = 0;
  uint8_t  data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  bool     ext  = false;      // extended (29-bit) identifier; AFFA never uses one

  // Set by the library on every frame it hands to ICanLink::send(), and NEVER set by
  // an application. A real CAN controller does not receive its own transmissions, but
  // LoopbackLink does — so without this flag pressKey(..., Both) would fire once on
  // hardware and twice on the host, and every host test would be lying about the
  // target. The RX key decoder ignores any frame with fromSelf set, unconditionally.
  // See the echo rule in §7b.6.
  bool     fromSelf = false;
};

// Wire codes. DO NOT RENUMBER — these values are transmitted and received verbatim
// in bytes 2..3 of the 0x1C1 key frame.
enum class Key : uint16_t {
  Load     = 0x0000,   // the button at the bottom of the stalk
  SrcNext  = 0x0001,
  SrcPrev  = 0x0002,
  VolUp    = 0x0003,
  VolDown  = 0x0004,
  Pause    = 0x0005,
  RollUp   = 0x0101,   // wheel, one detent up
  RollDown = 0x0141,   // wheel, one detent down

  // AND ANY OTHER uint16_t VALUE IS A RAW WIRE CODE. This enum is OPEN, deliberately.
  // The eight names above are [REF]-attested (affa3.h) and the encoder exemption is
  // provable, but NOTHING establishes that the list is COMPLETE — a different stalk, a
  // different model year or an unlisted button would produce a code not named here. Such
  // a code must never be dropped, so the decoder delivers `static_cast<Key>(raw & 0xFF3F)`
  // — a valid Key carrying the wire code, with the two hold bits stripped into KeyEdge.
  //
  // There is deliberately NO `Unknown` sentinel: a single sentinel value cannot carry the
  // code, and carrying the code is the whole requirement. Compare against the names you
  // handle and pass anything else through with its numeric value:
  //
  //     default: LOGI("unmapped key 0x%04X", static_cast<uint16_t>(k)); break;
  //
  // A switch over Key therefore always needs a `default:`; -Wswitch cannot help here.
};

// Click vs hold. The panel encodes hold as bits 0x80|0x40 set in the LOW byte of the
// key code — but only for the non-wheel keys; see §8.3 for why that matters.
enum class KeyEdge : uint8_t { Click = 0, Hold = 1 };

// Where an emulated key press is to have its effect. A press on the real system is
// transmitted by the PANEL and received by us, so "emulate a key" legitimately means
// two things at once, and an application wants each of them separately at different
// times. One function with a source, not two lookalike functions — see §7b.6.
enum class KeySource : uint8_t {
  Local = 1,   // as if a key arrived: drive our menu + fire the Key event. Nothing goes
               // on the bus. THE DEFAULT, because in the radio role that is what a key
               // press IS from our side.
  Wire  = 2,   // impersonate the panel: encode and transmit the key frame. Only
               // meaningful when a REAL radio is on the bus and you are driving it.
  Both  = 3,   // both at once. Rarely what you want — see §7b.6.
};
constexpr bool hasSource(KeySource v, KeySource f) noexcept {
  return (static_cast<uint8_t>(v) & static_cast<uint8_t>(f)) != 0;
}

// Values 0..5 keep the numeric identities of the legacy AffaCommon::AffaError so a
// migrating application that logged the raw number sees the same number.
enum class Result : uint8_t {
  Ok          = 0,
  NoSync      = 1,   // link not established (was AffaError::NoSync)
  UnknownFunc = 2,   // funcId not in this panel's function table
  SendFailed  = 3,   // panel answered something that was neither DONE nor PARTIAL
  Timeout     = 4,   // no ACK within AFFA_ACK_TIMEOUT_MS
  TooLong     = 5,   // payload exceeds AFFA_MAX_PAYLOAD (was StrTooLong)
  QueueFull   = 6,
  NotSupported= 7,   // this panel does not implement this Feature
  BadArgument = 8,   // null pointer, len == 0, index out of range
  LinkDown    = 9,   // ICanLink::isLive() was false
  Cancelled   = 10,  // job discarded because sync was lost or begin() was re-run
  // 11 was Busy, returned only by sendBlocking(); both are gone and 11 is NOT reused.
  Aborted     = 12,  // discarded by the APPLICATION before any byte reached the wire:
                     // abortPending(), abortAll(), or superseded by a newer render of
                     // the same RenderSlot. onComplete only — never returned by an
                     // enqueue call. See §3b.
};

// Bit flags, exactly the legacy bit assignment. FuncsReg is dropped whenever Failed
// is raised — registration does not survive a resync.
enum class SyncState : uint8_t {
  None      = 0x00,
  Failed    = 0x01,
  PeerAlive = 0x02,
  Start     = 0x04,
  FuncsReg  = 0x08,
};

constexpr SyncState  operator|(SyncState a, SyncState b) noexcept {
  return static_cast<SyncState>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr SyncState  operator&(SyncState a, SyncState b) noexcept {
  return static_cast<SyncState>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
constexpr SyncState  operator~(SyncState a) noexcept {
  return static_cast<SyncState>(static_cast<uint8_t>(~static_cast<uint8_t>(a)));
}
inline SyncState& operator|=(SyncState& a, SyncState b) noexcept { a = a | b; return a; }
inline SyncState& operator&=(SyncState& a, SyncState b) noexcept { a = a & b; return a; }
constexpr bool hasFlag(SyncState v, SyncState f) noexcept {
  return (static_cast<uint8_t>(v) & static_cast<uint8_t>(f)) == static_cast<uint8_t>(f);
}

// Optional capabilities. supports() answers before you call; an unsupported call
// returns Result::NotSupported (§6).
enum class Feature : uint8_t {
  Text,         // setText
  Time,         // setTime
  Power,        // setPower
  Menu,         // getMenu / showMenu / highlightItem / nav
  Popup,        // showPopupText / hidePopup
  Fullscreen,   // showFullscreenText / hideFullscreenText
  ConfirmBox,   // showConfirmBox
  InfoPopup,    // showInfoPopup / hideInfoPopup
  KeyTx,        // this panel family has a key-transmit id, so pressKey(..., Wire)
                // can put a frame on the bus (§7b.6)
  RadioText,    // the ISO-TP reassembler is compiled in (AFFA_ENABLE_ISOTP_RX), so
                // inbound text is reconstructed and delivered to onText() (§2.14.1)
};

// Navigation intent. Mapped to (Key, KeyEdge) by AffaDisplayBase::nav() — see §8.
enum class NavCommand : uint8_t {
  Open,      // hold-Load: open the menu
  Next,      // RollDown click  (in edit mode: next value)
  Prev,      // RollUp   click  (in edit mode: previous value)
  Select,    // Load click: activate item / enter edit / advance to next field
  Back,      // hold-Load: leave edit / close the menu
  Increase,  // RollDown hold: coarse step (Field::stepMultiplier)
  Decrease,  // RollUp   hold: coarse step
};

// Handle for one enqueued transmission. Strictly increasing, wraps 0xFFFF -> 1.
// Zero is never issued and means "no ticket" / "rejected at enqueue".
using TxTicket = uint16_t;
inline constexpr TxTicket kNoTicket = 0;

// What a queued message is FOR, so a newer one can supersede an older one that has not
// started yet. Latest value wins, per slot. None opts out of coalescing entirely: a
// None message never replaces anything and is never replaced. See §3b.4.
enum class RenderSlot : uint8_t {
  Text,        // setText
  Clock,       // setTime
  Menu,        // showMenu (the 96-byte screen)
  Highlight,   // highlightItem (the single-frame selection move)
  Popup,       // showPopupText / hidePopup
  Fullscreen,  // showFullscreenText / hideFullscreenText
  ConfirmBox,  // showConfirmBox
  InfoPopup,   // showInfoPopup / hideInfoPopup
  Control,     // setPower and other non-rendering control payloads
  None,        // raw enqueue(); never coalesced
};

// Urgent jumps the queue ahead of Normal work. It never splits a message that is
// already on the wire, and it never overtakes a pending function-registration job —
// the panel rejects payloads sent before registration completes.
enum class Priority : uint8_t { Normal = 0, Urgent = 1 };

// Named, so the three trailing arguments cannot be swapped at the call site.
struct TxOptions {
  RenderSlot slot     = RenderSlot::None;
  Priority   priority = Priority::Normal;
  bool       coalesce = (AFFA_TX_COALESCE != 0);  // per-message opt-out
};

// Free-running counters. Cleared only by Esp32CanLink::begin(). Read from any
// context; each field is a plain uint32_t updated by a single writer, so a torn
// read is impossible on a 32-bit target and harmless on the host.
struct Stats {
  uint32_t rxFrames    = 0;  // frames pushed into the RX ring by the driver callback
  uint32_t txFrames    = 0;  // frames accepted by the driver
  uint32_t txDropped   = 0;  // send() refused: TX gate closed, or driver said no
  uint32_t ringOverflow= 0;  // RX ring was full — frames LOST. Non-zero means poll()
                             // is not being called often enough, or the ring is small.
  uint32_t txErr       = 0;  // controller TX error counter (driver status, read-only)
  uint32_t rxErr       = 0;  // controller RX error counter
  uint32_t txFailed    = 0;  // controller TX failure count (arbitration/ack loss)
};

// ---------------------------------------------------------------------------
// The three-layer observation seam. Rationale, firing context and worked
// examples are in §7b; the declarations live here because every layer speaks
// only in types this header already owns.
// ---------------------------------------------------------------------------

// Which way a frame went. A tap that cannot tell inbound from outbound is
// useless to a sniffer, and a subscription that cannot say "only what the panel
// sent" would fire on our own echo of the same id — 0x151 carries both.
//
// A frame arriving through ICanLink::recv() with Frame::fromSelf set is our own
// transmission coming back off a link that echoes. It is presented to Layer 0 and
// Layer 1 as Direction::Tx, never as Rx, so a `dir = Rx` subscription means "what
// the other node actually sent" on every link, echoing or not (§7b.6).
enum class Direction : uint8_t { Rx = 1, Tx = 2, Both = 3 };

// Layer 0: every frame, in and out, unfiltered. One tap, replaces the previous.
using FrameTap = void (*)(const Frame& f, Direction d, void* ctx);

// Layer 1: filtered raw subscription.
using FrameCb  = void (*)(const Frame& f, void* ctx);

// Match an id under a mask, then optionally match payload bytes under a mask.
// A DEFAULT-CONSTRUCTED FrameMatch matches id 0x000 inbound and nothing else: AFFA
// uses no such id, so a half-filled match is inert rather than a firehose. Matching
// every id is the explicit opt-in `idMask = 0`. The matching rule is in §7b.4.
struct FrameMatch {
  uint32_t  id      = 0;        // frame id to match
  uint32_t  idMask  = 0x7FF;    // 0x7FF exact, 0 = any id
  uint8_t   data[8]    = {0};   // expected bytes
  uint8_t   dataMask[8]= {0};   // which of those bytes must match (0 = don't care)
  uint8_t   len     = 0;        // significant bytes of data[]/dataMask[]; 0 = id only
  Direction dir     = Direction::Rx;
};

// Opaque, non-zero when valid. Encodes slot index and a generation counter, so a
// stale handle from a slot that was freed and reused cannot unsubscribe the new
// owner — the failure mode of a bare index, and it is silent.
struct SubHandle {
  uint16_t v = 0;
  bool valid() const { return v != 0; }
};
inline constexpr SubHandle kNoSub{};

// Layer 2: decoded protocol events.
enum class EventKind : uint8_t {
  SyncChanged,   // ev.sync   — the state word changed
  Registered,    // ev.sync   — FUNCSREG latched
  PeerLost,      // ev.sync   — the peer-alive deadline expired
  Key,           // ev.key    — decoded from the wire OR from pressKey/nav with a
                 //             source that includes Local (§7b.6)
  TxComplete,    // ev.tx     — same information as CompleteCb
  LinkError,     // ev.error  — ring overflow, dropped TX, controller error
  // RadioText and ScreenChanged were declared here and NOTHING ever constructed
  // either — reassembly lives in proto/ and the panels do not depend on it. Two
  // events that cannot arrive are two false promises, so both were removed. Re-add
  // one WITH its emitter, never before it.
};

enum class LinkErrorKind : uint8_t {
  RingOverflow,     // Stats::ringOverflow advanced: frames were LOST
  TxDropped,        // ICanLink::send() refused a frame
  ControllerError,  // the driver's own error counters advanced
};

// A tagged union, not std::variant and not a class hierarchy. std::variant costs
// an index, alignment padding and a valueless-by-exception state this library
// (built -fno-exceptions) cannot even reach; a hierarchy costs a vtable pointer
// per event and forces the event to outlive the callback. This is 12 bytes of
// POD built on the poll() stack, copied nowhere, allocated never.
struct Event {
  EventKind kind;
  union {
    struct { SyncState prev; SyncState now; }            sync;
    struct { Key key; KeyEdge edge; }                    key;
    struct { TxTicket ticket; Result result; }           tx;
    struct { LinkErrorKind kind; uint32_t count; }       error;
  };
};

using EventCb = void (*)(const Event& ev, void* ctx);

} // namespace affa
```

No arm of `Event` currently carries a pointer, because the two that did — `text` and
`screen` — went with `EventKind::RadioText` and `EventKind::ScreenChanged`. **If one comes
back, its rule comes back with it:** a pointer inside an `Event` points at library-internal
storage and is valid **only for the duration of the callback**. Copy what you need; do not
store the pointer. That is the one rule of the event seam a reviewer must check by eye,
because keeping the pointer compiles and works right up until the next frame arrives.

### 2.2 `core/ICanLink.h`

```cpp
#pragma once
#include "AffaTypes.h"

namespace affa {

// The CAN seam, deliberately a PULL port.
//
// The obvious design is a push callback (`onReceive(cb)`), and that is what the code
// this library was extracted from used. It is also what made the ACK deadlock
// possible: the protocol layer blocked waiting for a frame that only a push from
// somewhere else could deliver. With recv(), the protocol layer owns its own drain
// and cannot wait on a thing it is preventing.
struct ICanLink {
  virtual ~ICanLink() = default;

  // Hand one frame to the controller. MUST NOT BLOCK. Returns false if the frame was
  // not accepted (TX gate closed, driver queue full, bus off). Never retries.
  virtual bool send(const Frame& f) = 0;

  // Pop one buffered received frame. Returns false when the buffer is empty.
  // MUST NOT BLOCK. Called in a tight loop by AffaDisplayBase::poll().
  virtual bool recv(Frame& out) = 0;

  // False disables all transmission at the protocol layer: enqueue() returns
  // LinkDown and in-flight jobs complete LinkDown. Default true.
  virtual bool isLive() const { return true; }

  virtual Stats stats() const { return Stats{}; }
};

} // namespace affa
```

### 2.3 `core/IClock.h`

```cpp
#pragma once
#include <cstdint>

namespace affa {

// There is deliberately NO delayMs(). Nothing in this library is allowed to sleep.
// If an implementation wants one, the state machine is wrong.
struct IClock {
  virtual ~IClock() = default;
  virtual uint32_t millis() const = 0;
};

} // namespace affa
```

All time comparisons in the library are wrap-safe and written exactly one way:

```cpp
if (static_cast<int32_t>(now - deadline) >= 0) { /* expired */ }
```

Never `now > deadline`, never `now - last > interval` with unsigned `last` sourced
from a different epoch.

### 2.4 `core/IPanel.h`

```cpp
#pragma once
#include "AffaTypes.h"

namespace affa {

// The minimal RENDERING port: "how to draw", nothing else.
//
// An IPage, and the menu ADAPTER (carminat/CarminatMenuRenderer), draw through this
// rather than through the concrete display, so both are unit-testable against a fake
// panel and a future WebPanel (render to a browser, no CAN) satisfies the same four
// calls. The menu STATE MACHINE does not: widget::MenuModel draws through
// widget::IMenuRenderer and has no idea this interface exists. No defaults: a
// rendering caller passes every argument explicitly.
struct IPanel {
  virtual ~IPanel() = default;

  virtual Result showMenu(const char* header, const char* row0, const char* row1,
                          uint8_t scrollIndicator) = 0;
  virtual Result setText(const char* text, uint8_t digit) = 0;
  virtual Result highlightItem(uint8_t row) = 0;
  virtual Result showPopupText(const char* text, uint8_t icon,
                               uint8_t srcIcon, uint8_t fmt) = 0;
};

} // namespace affa
```

### 2.5 `core/IDisplay.h`

```cpp
#pragma once
#include "AffaTypes.h"

namespace affa {

// The panel-agnostic surface. An application that wants to work across Carminat and
// UpdateList holds an IDisplay&; AffaDisplayBase implements it.
//
// Every render call ENQUEUES and returns immediately (§3). The Result is an
// ACCEPTANCE verdict, never a delivery verdict.
//
// EVERY ONE OF THEM IS [[nodiscard]], and so is every override in AffaDisplayBase and in
// the panels, plus enqueue(), pressKey(), nav() and subscribe(). The
// Result is the only thing separating "queued" from NoSync / QueueFull / TooLong /
// NotSupported, and a dropped Result is a screen that silently never appears — the exact
// legacy failure §6 exists to stop repeating. Ignore one on purpose and say so:
//     (void)display.setText("RENAULT", 0);
struct IDisplay {
  virtual ~IDisplay() = default;

  virtual bool     begin() = 0;
  virtual void     poll()  = 0;
  virtual bool     supports(Feature f) const = 0;
  virtual SyncState syncState() const = 0;

  [[nodiscard]] virtual Result setText(const char* text, uint8_t digit = 255) = 0;
  [[nodiscard]] virtual Result setTime(const char* hhmm)                      = 0;
  [[nodiscard]] virtual Result setPower(bool on)                              = 0;

  [[nodiscard]] virtual Result showMenu(const char* header, const char* row0,
                                        const char* row1,
                                        uint8_t scrollIndicator = 0x0B)       = 0;
  [[nodiscard]] virtual Result highlightItem(uint8_t row)                     = 0;

  [[nodiscard]] virtual Result showPopupText(const char* text, uint8_t icon = 0x09,
                                             uint8_t srcIcon = 0xFF,
                                             uint8_t fmt = 0x60)              = 0;
  [[nodiscard]] virtual Result hidePopup()                                    = 0;
  [[nodiscard]] virtual Result showFullscreenText(const char* l1, const char* l2,
                                                  const char* l3)             = 0;
  [[nodiscard]] virtual Result hideFullscreenText()                           = 0;
  [[nodiscard]] virtual Result showConfirmBox(const char* caption, const char* row0,
                                              const char* row1)               = 0;
  [[nodiscard]] virtual Result showInfoPopup(const char* l1, const char* l2,
                                             const char* l3)                  = 0;
  [[nodiscard]] virtual Result hideInfoPopup()                                = 0;
};

} // namespace affa
```

Note the difference from the code being extracted: the legacy `IDisplay` gave these
methods **silently no-op defaults**. Here they are pure virtual on the interface and
`AffaDisplayBase` supplies a single default body that returns `Result::NotSupported`.
See §6.

### 2.6 `util/AffaLog.h`

```cpp
#pragma once
#include "../AffaConfig.h"
#include <cstdint>

namespace affa {

// Levels are ordered; AFFA_LOG_LEVEL compiles out everything above it.
// 0 = off, 1 = error, 2 = warn, 3 = info, 4 = debug, 5 = trace.
struct ILogSink {
  virtual ~ILogSink() = default;
  virtual void write(uint8_t level, const char* tag, const char* msg) = 0;
};

namespace detail {
#if AFFA_ENABLE_LOG
void setSink(ILogSink* s);
void emit(uint8_t level, const char* tag, const char* fmt, ...)
     __attribute__((format(printf, 3, 4)));
#else
inline void setSink(ILogSink*) {}
#endif
} // namespace detail

} // namespace affa

#if AFFA_ENABLE_LOG
#  define AFFA_LOG_(lvl, tag, ...) ::affa::detail::emit((lvl), (tag), __VA_ARGS__)
#else
#  define AFFA_LOG_(lvl, tag, ...) do {} while (0)
#endif

#if AFFA_ENABLE_LOG && AFFA_LOG_LEVEL >= 1
#  define AFFA_LOGE(tag, ...) AFFA_LOG_(1, tag, __VA_ARGS__)
#else
#  define AFFA_LOGE(tag, ...) do {} while (0)
#endif
// ... AFFA_LOGW (2), AFFA_LOGI (3), AFFA_LOGD (4), AFFA_LOGT (5) follow the same shape.
```

**The disabled macro discards its arguments entirely.** A format string that is never
referenced is never emitted into `.rodata`; that is the whole point of the knob. The
consequence: *never put a side effect inside a log argument.* `AFFA_LOGD("X", "%d",
counter++)` compiles to nothing and the increment disappears. A CI grep rejects `++`,
`--` and `=` inside `AFFA_LOG*` argument lists.

### 2.7 `core/AffaSyncProfile.h`

```cpp
#pragma once
#include <cstdint>

namespace affa {

// One sync FSM, two panel families. Everything that differed between
// CarminatDisplay::tick() and UpdateListBase::tick() is data in here; the code is
// in AffaDisplayBase::pumpSync(). Duplicating the FSM is what let the same two
// defects live twice.
struct SyncProfile {
  uint16_t syncId;        // Carminat 0x3AF   UpdateList 0x3DF   (we transmit here)
  uint16_t syncReplyId;   // both 0x3CF                          (panel transmits here)
  uint16_t replyFlag;     // both 0x400
  uint8_t  aliveByte;     // 0xB9 / 0x79   heartbeat, data[0]
  uint8_t  requestByte;   // 0xBA / 0x7A   sync request, data[0]
  uint8_t  requestArg;    // Carminat 0x00, UpdateList 0x01 — data[1] of the request
  uint8_t  filler;        // 0x00 / 0x81   pads every frame we build
  const uint8_t (*hello)[8];  // frames sent in reply to `61 11`, in order
  uint8_t  helloCount;    // Carminat 3, UpdateList 1
};

} // namespace affa
```

The two instances, for reference (full byte-level justification in `WIRE-SPEC.md`):

```cpp
// carminat/CarminatConstants.h
namespace affa { namespace carminat {
inline constexpr uint8_t kHello[3][8] = {
  {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01},
  {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
  {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},   // sent TWICE — observed, not a typo
};
inline constexpr SyncProfile kSync = {
  0x3AF, 0x3CF, 0x400, 0xB9, 0xBA, 0x00, 0x00, kHello, 3
};
inline constexpr uint16_t kFuncIds[] = { 0x151, 0x1F1 };  // ORDER IS ON THE WIRE
}}

// updatelist/UpdateListConstants.h
namespace affa { namespace updatelist {
inline constexpr uint8_t kHello[1][8] = {
  {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01},
};
inline constexpr SyncProfile kSync = {
  0x3DF, 0x3CF, 0x400, 0x79, 0x7A, 0x01, 0x81, kHello, 1
};
inline constexpr uint16_t kFuncIds[] = { 0x121, 0x1B1 };
}}
```

### 2.8 `core/AffaRing.h`

```cpp
#pragma once
#include <cstdint>
#include <atomic>

namespace affa {

// Lock-free single-producer / single-consumer ring.
//
// Producer = the esp32_can general callback, running in task_CAN (prio 15).
// Consumer = whatever task calls AffaDisplayBase::poll().
// Exactly one thread may call push(); exactly one may call pop(). Two producers or
// two consumers corrupt it silently — there is no lock and there is not going to be
// one, because push() runs in a driver task and must not be able to block it.
//
// N must be a power of two: the modulo is a mask, and the head/tail counters are
// free-running so a full ring is distinguishable from an empty one without a spare
// slot or a count.
template <typename T, uint16_t N>
class AffaRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "AffaRing capacity must be a power of two");

 public:
  // Producer side. Returns false and bumps overflow() when full; the frame is LOST.
  // Dropping the newest is deliberate: overwriting the oldest would hand the protocol
  // layer a sequence with a hole in the middle of an ISO-TP transfer.
  bool push(const T& v) {
    const uint16_t h = _head.load(std::memory_order_relaxed);
    const uint16_t t = _tail.load(std::memory_order_acquire);
    if (static_cast<uint16_t>(h - t) >= N) {
      _overflow.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    _buf[h & (N - 1)] = v;
    _head.store(static_cast<uint16_t>(h + 1), std::memory_order_release);
    return true;
  }

  // Consumer side.
  bool pop(T& out) {
    const uint16_t t = _tail.load(std::memory_order_relaxed);
    const uint16_t h = _head.load(std::memory_order_acquire);
    if (h == t) return false;
    out = _buf[t & (N - 1)];
    _tail.store(static_cast<uint16_t>(t + 1), std::memory_order_release);
    return true;
  }

  bool     empty() const { return _head.load(std::memory_order_acquire) ==
                                  _tail.load(std::memory_order_acquire); }
  uint16_t size()  const { return static_cast<uint16_t>(_head.load(std::memory_order_acquire) -
                                                        _tail.load(std::memory_order_acquire)); }
  static constexpr uint16_t capacity() { return N; }
  uint32_t overflow() const { return _overflow.load(std::memory_order_relaxed); }
  // Consumer side only, and only while the producer is known idle (before begin()).
  void reset() { _head.store(0); _tail.store(0); _overflow.store(0); }

 private:
  T _buf[N];
  std::atomic<uint16_t> _head{0};   // written by producer only
  std::atomic<uint16_t> _tail{0};   // written by consumer only
  std::atomic<uint32_t> _overflow{0};
};

} // namespace affa
```

### 2.9 `core/AffaDisplayBase.h`

```cpp
#pragma once
#include "../AffaConfig.h"
#include "AffaTypes.h"
#include "AffaSyncProfile.h"
#include "ICanLink.h"
#include "IClock.h"
#include "IDisplay.h"
#include "IPanel.h"
#include "../util/AffaLog.h"

namespace affa {

// NOTHING MENU-SHAPED IS DECLARED HERE, not even a forward declaration. The base reaches the
// menu through three virtual hooks (menuOpen / openMenu / routeKeyToMenu, §7b) and never
// through a type, which is what lets widget::MenuModel exist without core/ knowing it does.

// Implements both interfaces. The four IPanel primitives (showMenu, setText,
// highlightItem, showPopupText) are declared with SIGNATURES IDENTICAL to IDisplay's,
// so a single override in a panel satisfies both bases with no ambiguity. Interfaces
// carry no data, so there is no diamond. Do not "tidy" either signature.
class AffaDisplayBase : public IDisplay, public IPanel {
 public:
  // Callback signatures. C function pointers, not std::function: no allocation, no
  // hidden vtable, and a plain pointer can be compared and reset.
  using KeyCb      = void (*)(Key k, KeyEdge e, void* ctx);
  using CompleteCb = void (*)(TxTicket t, Result r, void* ctx);
  using SyncCb     = void (*)(SyncState s, void* ctx);

  // The link, the clock and the profile are constructor arguments because none of
  // them is optional and none of them may change after begin().
  AffaDisplayBase(ICanLink& link, IClock& clock, const SyncProfile& profile,
                  const uint16_t* funcIds, uint8_t funcCount);
  ~AffaDisplayBase() override = default;

  AffaDisplayBase(const AffaDisplayBase&)            = delete;
  AffaDisplayBase& operator=(const AffaDisplayBase&) = delete;

  // ---- lifecycle ----------------------------------------------------------
  // Resets the FSMs, clears the queue (queued tickets complete Cancelled), arms the
  // peer deadline and the heartbeat. Transmits NOTHING by itself — the first frame
  // leaves on the first poll(). Safe to call again; idempotent apart from the reset.
  bool begin() override;

  // The single pump. In this order, every call, no exceptions:
  //   1. drain the RX ring; per frame: tap -> subscriptions -> library consumption
  //      (sync frames, ACKs, auto-ACK, key frames -> KeyCb)
  //   2. advance the sync FSM
  //   3. advance the TX FSM
  //   4. onPoll() panel hook
  // Step 1 strictly precedes step 3 so that key latency is bounded by the poll period
  // alone and is independent of the transmit queue (§3b.3). See §4 for the
  // frequency-independence contract.
  void poll() override;

  // ---- ports and options --------------------------------------------------
  void setLogSink(ILogSink* s);
  void onKey(KeyCb cb, void* ctx);
  void onComplete(CompleteCb cb, void* ctx);
  void onSync(SyncCb cb, void* ctx);        // fires only on an actual state change

  // ---- observation seam (§7b) ----------------------------------------------
  // Layer 0: every frame in and out, unfiltered, for sniffers and consoles.
  // One tap; a second call replaces the first. Pass nullptr to remove it.
  void onFrame(FrameTap cb, void* ctx);

  // Layer 1: filtered raw subscription. Fixed table of AFFA_MAX_SUBSCRIPTIONS
  // entries, no allocation. Returns kNoSub when the table is full or the match
  // is unsatisfiable (dir == 0, or len > 8).
  SubHandle subscribe(const FrameMatch& m, FrameCb cb, void* ctx);
  bool      unsubscribe(SubHandle h);       // false if the handle is stale
  uint8_t   subscriptions() const;          // slots in use, for diagnostics

  // Layer 2: decoded protocol events. One sink; a second call replaces the
  // first. Fires IN ADDITION TO KeyCb/CompleteCb/SyncCb, never instead of them.
  void onEvent(EventCb cb, void* ctx);

  // Passive mode: a real radio is on the bus and owns the handshake. We then send no
  // sync frames, no hello, no generic 0x74 ACK, and never latch FUNCSREG — we only
  // inject data. This was `setSkipFuncReg`. On a vehicle bus, set it.
  void setPassive(bool on);
  bool passive() const;

  // Bench self-ACK. With no panel on the bus the per-frame ACK never arrives and only
  // the first frame of a multi-frame message would go out. With this on, the TX FSM
  // acknowledges its own frames (PARTIAL while bytes remain, DONE on the last) so the
  // complete, real frame sequence is emitted for a PC-side decoder. The wire bytes are
  // identical to a real send; only the external ACK is skipped.
  void setSelfAck(bool on);

  // ---- observation --------------------------------------------------------
  SyncState syncState() const override;
  bool      synced()     const;   // !hasFlag(state, Failed)
  bool      registered() const;   //  hasFlag(state, FuncsReg)
  bool      busy()       const;   // a job is in flight or queued
  Result    lastResult() const;   // Result of the most recently COMPLETED ticket
  TxTicket  lastTicket() const;   // that ticket
  // The ticket issued by the most recent successful enqueue, INCLUDING one made
  // inside a render call — which is how an application that used setText() rather
  // than enqueue() learns which ticket to match in onComplete. kNoTicket if the last
  // enqueue was rejected. Read it immediately after the call; it is overwritten by
  // the next one, including by a render the menu makes on your behalf.
  TxTicket  lastEnqueued() const;
  uint8_t   queued()     const;   // jobs waiting behind the active one
  Stats     stats()      const;   // forwarded from the link

  // ---- capability ---------------------------------------------------------
  bool supports(Feature f) const override = 0;   // each panel answers for itself

  // ---- transmit -----------------------------------------------------------
  // Copy `len` bytes into a queue slot and return immediately. The bytes need not
  // outlive the call. Returns kNoTicket on rejection; the reason is in lastResult().
  // `opt` carries the coalescing slot, the priority and the per-message coalescing
  // opt-out (§3b). The default is a plain FIFO append — slot None never coalesces —
  // which is what a raw protocol send wants.
  TxTicket enqueue(uint16_t funcId, const uint8_t* data, uint8_t len,
                   TxOptions opt = TxOptions{});

  // ---- preemption (§3b.5) --------------------------------------------------
  // Drop every job that is QUEUED AND NOT YET STARTED — i.e. every job of which not
  // one byte has been handed to ICanLink::send(). The job on the wire is NOT touched.
  // Each dropped ticket is reported through onComplete with Result::Aborted. Returns
  // how many were dropped. The queue is mutated BEFORE any callback fires, so a nested
  // abortPending() from inside one of those callbacks finds nothing and returns 0.
  uint8_t abortPending();

  // abortPending(), plus abandon the message currently on the wire. The abandon
  // happens at the next FRAME BOUNDARY — after the in-flight frame's ACK arrives or
  // its deadline expires — never mid-frame, and the ISO-TP continuation counter is
  // reset so the next message starts clean at its own frame 0. Returns true if a job
  // was actually abandoned. Bench and shutdown use: the panel is left holding a
  // half-received transfer, and how it recovers is the panel's business, not ours.
  // Routine preemption is coalescing + abortPending() + Priority::Urgent.
  bool abortAll();

  // Is a not-yet-started job for this slot sitting in the queue? Cheap, exact, and
  // the thing to assert in a test rather than counting frames.
  bool pending(RenderSlot s) const;

  // NOTE: there is no sendBlocking(). One existed — it pumped poll() until a ticket
  // completed — and it was the only call in the library that waited for anything.
  // Nothing in src/, examples/ or test/ ever used it, so it was removed rather than
  // maintained. Watch onComplete() from your own loop.

  // ---- input seam (see §8 and §7b.6) ---------------------------------------
  // Emulate a key press. The Local half takes the IDENTICAL path to a key decoded off
  // the wire, so anything a test or a web page can drive is provably what the panel
  // drives. Safe from an application task; NOT safe from an ISR (it can render, which
  // enqueues). BOTH default to Local: in the radio role we are the RECEIVER of key
  // frames, so transmitting one does not make the emulation more faithful — it puts a
  // frame on the bus that nothing is listening for (§7b.6).
  Result pressKey(Key k, KeyEdge e, KeySource src = KeySource::Local);
  Result nav(NavCommand c,          KeySource src = KeySource::Local);

#if AFFA_ENABLE_MENU
  // The gesture that OPENS the menu. "Hold Load opens the menu" is the OEM convention
  // for this panel, so it ships as the default — but it is UI policy, not wire format,
  // and an application with its own remote or its own idea must be able to replace it.
  // Affects opening ONLY: once the menu is open, key routing into it is rendering
  // behaviour and is not configurable (§7b.7c).
  void setMenuHotkey(Key k, KeyEdge e);   // default: Key::Load, KeyEdge::Hold
  void clearMenuHotkey();                 // no gesture opens the menu; only nav(Open)
  bool menuHotkey(Key& k, KeyEdge& e) const;   // false when cleared

  // NOTE: getMenu() is NOT here. It is declared on CarminatDisplay, the panel that owns
  // a Menu; the base declares no Menu accessor and no `virtual Menu* menu()` seam,
  // because UpdateList has no menu at all. nav() above is the panel-agnostic half. See
  // §8.7 — the library still hands out an EMPTY menu that the application fills.
#endif

  // ---- rendering: default bodies return NotSupported ------------------------
  Result setText(const char*, uint8_t digit = 255) override;
  Result setTime(const char*) override;
  Result setPower(bool) override;
  Result showMenu(const char*, const char*, const char*, uint8_t = 0x0B) override;
  Result highlightItem(uint8_t) override;
  Result showPopupText(const char*, uint8_t = 0x09, uint8_t = 0xFF, uint8_t = 0x60) override;
  Result hidePopup() override;
  Result showFullscreenText(const char*, const char*, const char*) override;
  Result hideFullscreenText() override;
  Result showConfirmBox(const char*, const char*, const char*) override;
  Result showInfoPopup(const char*, const char*, const char*) override;
  Result hideInfoPopup() override;

 protected:
  // ---- panel hooks ---------------------------------------------------------
  // Every frame we build pads with this. Carminat 0x00, UpdateList 0x81.
  virtual uint8_t packetFiller() const = 0;

  // Called for each received frame the base did not consume itself (i.e. not a sync
  // frame on syncReplyId and not an ACK on funcId|replyFlag). Panels decode their key
  // frame and their radio-text frame here. Return true if consumed.
  virtual bool onFrame(const Frame& f) { (void)f; return false; }

  // Called after a key has been decoded (from the wire, or from pressKey/nav with a
  // source that includes Local). The base implementation applies the menu hotkey,
  // routes to MenuController when AFFA_ENABLE_MENU, then falls through to the
  // application's KeyCb and fires EventKind::Key. Panels override only to add routing,
  // never to replace the fall-through.
  virtual void routeKey(Key k, KeyEdge e);

  // Build and transmit the panel's key frame: keyId : 03 89 <hi> <lo|hold> <filler x4>.
  // The Wire half of pressKey(). Returns NotSupported when the panel has no key
  // transmit id, or for a hold edge on a wheel code (§7b.6). Not queued: a key frame
  // is a single frame on its own id with no ACK and no function registration, so
  // putting it behind the ISO-TP queue would give it exactly the latency §3b exists
  // to remove. Tagged fromSelf, and reported to the tap as Direction::Tx.
  Result transmitKey(Key k, KeyEdge e);

  // Called from poll() once per pass, after the sync and TX FSMs. Panels put their
  // own time-driven work here (the UpdateList title scroll). MUST be deadline-driven
  // against _clock, never a call counter.
  virtual void onPoll() {}

  // Decoded a hold edge from a raw wire code: shared helper so both panels use the
  // same mask. Wheel codes (0x0101/0x0141) never carry the hold bits — see §8.3.
  static void decodeKey(uint16_t raw, Key& out, KeyEdge& edge);

  ICanLink&          _link;
  IClock&            _clock;
  const SyncProfile& _profile;

 private:
  enum class TxState : uint8_t { Idle, SendingFrame, WaitAck, Done, Failed };
  enum class JobKind : uint8_t { Payload, Registration };

  struct TxJob {
    uint16_t   funcId;
    TxTicket   ticket;      // kNoTicket for Registration jobs — they are invisible
    JobKind    kind;
    RenderSlot slot;        // coalescing key, with funcId
    Priority   prio;
    bool       coalesce;    // false = this message is never replaced
    bool       started;     // true once ONE byte has gone to ICanLink::send().
                            // The single authority for "not yet started" (§3b.4).
    bool       abandon;     // abortAll() asked for it; honoured at the frame boundary
    uint8_t    len;
    uint8_t    sent;        // bytes already handed to the link
    uint8_t    frameIndex;  // ISO-TP continuation counter `num`
    uint8_t    data[AFFA_MAX_PAYLOAD];
  };

  // One subscription slot. `gen` is bumped on every unsubscribe so a stale SubHandle
  // cannot unsubscribe the slot's next owner — the silent failure mode of a bare index.
  struct Sub {
    FrameMatch m;
    FrameCb    cb  = nullptr;
    void*      ctx = nullptr;
    uint8_t    gen = 0;
    bool       used = false;
  };

  void pumpRx();            // ALWAYS first in poll(); delivers keys (§3b.3)
  void pumpSync();
  void pumpTx();            // ALWAYS last; never reached before pumpRx() has returned
  // The single choke point every frame passes through, in BOTH directions: tap first,
  // then the subscription table. Called from pumpRx() for each received frame and from
  // txFrame() for each transmitted one, so a sniffer sees the whole bus in order.
  void observe(const Frame& f, Direction d);
  bool txFrame(const Frame& f);     // stamps fromSelf, calls _link.send(), observes
  void emit(const Event& ev);       // fires the Layer 2 sink if one is installed
  int  findCoalescable(uint16_t funcId, RenderSlot s) const;  // -1 if none
  uint8_t insertIndexFor(Priority p) const;   // after started + Registration jobs
  bool handleSyncFrame(const Frame& f);
  bool handleAckFrame(const Frame& f);
  void sendGenericAck(uint16_t id);      // the 0x74 reply on id|replyFlag
  bool pushJob(uint16_t funcId, const uint8_t* d, uint8_t len, JobKind kind, TxTicket t);
  void finishJob(Result r);
  void failAllQueued(Result r);
  void setSync(SyncState s);             // single choke point; fires SyncCb on change

  TxJob     _queue[AFFA_TX_QUEUE_DEPTH];
  uint8_t   _qHead = 0, _qCount = 0;
  TxState   _tx = TxState::Idle;
  uint32_t  _ackDeadlineMs = 0;
  uint32_t  _nextSyncMs    = 0;
  uint32_t  _peerDeadlineMs= 0;
  SyncState _sync = SyncState::Failed;
  TxTicket  _nextTicket = 1;
  TxTicket  _lastCompleted = kNoTicket;
  Result    _lastResult = Result::Ok;
  const uint16_t* _funcIds;
  uint8_t   _funcCount;
  bool      _passive = false;
  bool      _selfAck = false;
  bool      _inPoll  = false;   // re-entrancy guard, see §4.3
  KeyCb      _keyCb = nullptr;      void* _keyCtx = nullptr;
  CompleteCb _cplCb = nullptr;      void* _cplCtx = nullptr;
  SyncCb     _syncCb = nullptr;     void* _syncCtx = nullptr;
  FrameTap   _tap   = nullptr;      void* _tapCtx = nullptr;
  EventCb    _evCb  = nullptr;      void* _evCtx  = nullptr;
  Sub        _subs[AFFA_MAX_SUBSCRIPTIONS];
#if AFFA_ENABLE_MENU
  Key        _hotkey     = Key::Load;      // §7b.7c — OEM default, replaceable
  KeyEdge    _hotkeyEdge = KeyEdge::Hold;
  bool       _hotkeyOn   = true;
#endif
};

} // namespace affa
```

#### 2.9.1 The sync FSM, specified

`pumpSync()` runs once per `poll()`. Nothing here is conditional on how often `poll()`
is called.

```
if (_passive) return;                      // the radio owns the handshake

if ((int32_t)(now - _nextSyncMs) < 0) return;
_nextSyncMs = now + AFFA_SYNC_INTERVAL_MS;   // NOT `+=`: a stalled caller must not
                                             // produce a catch-up burst of heartbeats

emit  syncId : { aliveByte, 0x00, filler x6 }          // TX 3AF B9 00 00 ...

if (hasFlag(_sync, Failed) || hasFlag(_sync, Start)) {
    emit syncId : { requestByte, requestArg, filler x6 }  // TX 3AF BA 00 00 ...
    _sync &= ~Start;
    _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;        // fresh window for the peer
    // the legacy delay(100) that lived here is DELETED, not replaced
} else if (hasFlag(_sync, PeerAlive)) {
    _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;
    _sync &= ~PeerAlive;
} else if ((int32_t)(now - _peerDeadlineMs) >= 0) {
    setSync(Failed);                 // clears FuncsReg with it — registration does
    _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;   // not survive a resync
}
```

RX side, on `syncReplyId`:

* `61 11 xx …` — the panel asks us to announce. Emit `helloCount` frames from
  `profile.hello`, in order, on `syncId`. Clear `Failed`. If `data[2] == 0x01`, set
  `Start`. (The live capture shows `61 11 00`, so `Start` is normally not set.)
* `69 …` — peer alive. Set `PeerAlive` and **return**. The legacy code called `tick()`
  from here, which emitted an extra heartbeat per ping. It does not any more: exactly
  one `B9` leaves per `AFFA_SYNC_INTERVAL_MS`, which is what the capture shows.
* anything else — ignored, logged at trace.

Any other received frame, when `!_passive` and the id does not carry `replyFlag`, is
answered with the generic ACK `id|replyFlag : { 0x74, filler x7 }`. That is the
`RX 1C1 70 …` → `TX 5C1 74 00 …` pair in the capture.

#### 2.9.2 The transmit FSM, specified

```
Idle
 └─ queue non-empty ────────────────────────────────► SendingFrame
SendingFrame
 ├─ build the next ISO-TP frame from the head job:
 │     frame 0 : data[0..7]   = payload[0..7]
 │     frame n : data[0]      = 0x20 + n, data[1..7] = payload[...]
 │     tail    : padded with packetFiller()
 ├─ _link.send(frame) == false  ──► finishJob(SendFailed)
 ├─ job.started = true   ── from here the job is no longer preemptable (§3b.4)
 ├─ _selfAck  ──► synthesise DONE (no bytes left) or PARTIAL, stay in WaitAck for
 │                exactly one pass so the sequence is identical to a real send
 └─ _ackDeadlineMs = now + AFFA_ACK_TIMEOUT_MS ──────► WaitAck
WaitAck                                    ← the ONLY place a job may be abandoned
 ├─ ACK 0x74            ──► finishJob(Ok)
 ├─ ACK 30 01 00 and bytes remain ──► job.abandon ? finishJob(Aborted)
 │                                                 : frameIndex++ ──► SendingFrame
 ├─ ACK 30 01 00 and no bytes remain ──► finishJob(SendFailed)   [legacy behaviour]
 ├─ any other ACK byte  ──► finishJob(SendFailed)
 ├─ deadline expired    ──► job.abandon ? finishJob(Aborted) : finishJob(Timeout)
 └─ !_link.isLive()     ──► finishJob(LinkDown)
```

`abandon` is only ever consulted in `WaitAck`, which is exactly what "abandoned at a
frame boundary, never mid-frame" means: the CAN frame already handed to the link is
always transmitted whole, and the next frame of that job is simply never built.
`finishJob()` unconditionally clears `frameIndex`, `sent` and `started` as it pops, so
an abandoned transfer cannot leave a continuation counter behind for the following
message to inherit — the next job's first frame is its own frame 0 (byte 0 of its
payload, `0x10` for any multi-frame Carminat screen), never a stray `0x2n`.

`finishJob(r)`: pop the head job; if `kind == Registration` and `r == Ok` and it was
the last registration job, latch `FuncsReg`. If `kind == Registration` and `r != Ok`,
call `failAllQueued(r)` — the payload behind it completes with the registration's
failure, exactly as the legacy `affa3_send` propagated it. If `ticket != kNoTicket`,
record `_lastCompleted = ticket`, `_lastResult = r`, and fire `CompleteCb`.

**Lazy function registration, byte-identical to the legacy wire order.** Inside
`enqueue()`, before pushing the payload job:

```
if (!_passive && !hasFlag(_sync, FuncsReg) && no Registration job is already queued) {
    for (i = 0; i < _funcCount; ++i)
        pushJob(_funcIds[i], {0x70}, 1, Registration, kNoTicket);   // always FIFO tail
}
// then the payload, subject to coalescing and priority — §3b.4, §3b.5
pushJob(funcId, data, len, Payload, ticket, opt);
```

Registration jobs are pushed with `slot = None`, `prio = Normal` and `coalesce =
false`: nothing may replace them and nothing, not even `Priority::Urgent`, may overtake
them. A payload that reached the panel before its function was registered is rejected
by the panel, and the resulting `SendFailed` would look like a wire-format bug.
`abortPending()` does not drop Registration jobs either — it drops a payload and leaves
the registration that was pushed for it, which then completes harmlessly and latches
`FuncsReg` for the next send.

Carminat therefore emits, on the first send after a resync:
`0x151 : 70 00 00 00 00 00 00 00`, wait ACK, `0x1F1 : 70 00 …`, wait ACK, latch
`FuncsReg`, then the payload. That is the legacy loop in `affa3_send`, turned inside
out into a queue without changing a byte or an order.

The queue must have room for `_funcCount + 1` jobs on the first send, so
`AFFA_TX_QUEUE_DEPTH` must be at least 3 for either panel. The default 4 leaves one
spare.

### 2.10 `link/Esp32CanLink.h`

```cpp
#pragma once
#include "../AffaConfig.h"
#if AFFA_ENABLE_ESP32CAN_LINK

#include <driver/gpio.h>          // gpio_num_t ONLY. Not <esp32_can.h>.
#include "../core/ICanLink.h"
#include "../core/AffaRing.h"

namespace affa {

// Named so the two pins cannot be swapped at the call site. They have been, and the
// symptom is a silent bus: no TX error, no RX, nothing.
//   this board (ESP32-C3 SuperMini) : rx = GPIO_NUM_4, tx = GPIO_NUM_3
//   MeganeCAN's board is MIRRORED   : rx = GPIO_NUM_3, tx = GPIO_NUM_4
struct CanPins { gpio_num_t rx; gpio_num_t tx; };

// THE ONLY CLASS IN THIS LIBRARY THAT KNOWS A DRIVER EXISTS.
//
// PROHIBITION, not advice. After begin() returns, this class never touches the driver
// again except to call sendFrame() and to read status counters. Specifically it must
// never call setListenOnlyMode, setNoACKMode, enable, disable, or any twai_* function,
// and it implements no bus-off recovery of its own (AffaDisplayBase's peer watchdog
// owns that). The runtime mode setters in esp32_can are implemented as
// disable()+enable(), i.e. reinstalling the driver on a live bus, and that is what
// repeatedly left the controller stopped in the previous project. MeganeCAN worked for
// months precisely because it never touched the driver after begin().
class Esp32CanLink final : public ICanLink {
 public:
  Esp32CanLink() = default;

  // Exactly this sequence, exactly once:
  //   CAN0.setCANPins(pins.rx, pins.tx);   // signature IS (rx, tx) — verified in
  //                                        // esp32_can_builtin.h:
  //                                        // setCANPins(gpio_num_t rxPin, gpio_num_t txPin)
  //   CAN0.begin(bitrate);
  //   CAN0.setGeneralCallback(&trampoline);
  //   CAN0.watchFor();
  bool begin(CanPins pins, uint32_t bitrate = 500000);

  bool  send(const Frame& f) override;   // never blocks; false if the gate is shut
  bool  recv(Frame& out) override;       // pops the RX ring
  bool  isLive() const override;         // began, and the TX gate is open
  Stats stats() const override;

  // "Silent mode" is a SOFTWARE TX GATE: send() simply returns false. It is NOT a
  // driver mode change, and it never will be. See the prohibition above.
  void setTxEnabled(bool on);
  bool txEnabled() const;

 private:
  friend struct Esp32CanTrampoline;   // defined in the .cpp, where CAN_FRAME exists

  // Called from task_CAN (prio 15) via the driver's general callback. Pushes into the
  // ring and returns. It does not log, allocate, block, or call user code — the whole
  // reason ICanLink is a pull port.
  void ingest(const Frame& f);

  AffaRing<Frame, AFFA_RX_RING_DEPTH> _rx;
  Stats _stats{};
  bool  _began = false;
  bool  _txEnabled = true;
};

} // namespace affa
#endif // AFFA_ENABLE_ESP32CAN_LINK
```

`Esp32CanLink` is effectively a singleton: `setGeneralCallback` takes a plain function
pointer with no context argument, so the `.cpp` holds a `static Esp32CanLink* s_self`
set by `begin()`. A second `begin()` on a second instance returns `false` and logs an
error rather than silently stealing the callback.

### 2.11 `link/LoopbackLink.h`

```cpp
#pragma once
#include "../core/ICanLink.h"
#include "../core/AffaRing.h"

namespace affa {

// Header-only test double. Records everything the library transmits, lets a test
// inject what the panel would have said, and can synthesise the per-frame ACK so a
// test does not have to hand-write one per ISO-TP frame.
template <uint16_t N = 128>
class LoopbackLink final : public ICanLink {
 public:
  bool send(const Frame& f) override {
    if (!_live) { ++_stats.txDropped; return false; }
    if (!_sent.push(f)) { ++_stats.txDropped; return false; }
    ++_stats.txFrames;
    if (_autoAck) synthesiseAck(f);
    return true;
  }
  bool  recv(Frame& out) override { return _rx.pop(out); }
  bool  isLive() const override   { return _live; }
  Stats stats()  const override   { return _stats; }

  // ---- test side ----
  void inject(const Frame& f)  { _rx.push(f); ++_stats.rxFrames; }   // panel -> library
  bool takeSent(Frame& out)    { return _sent.pop(out); }            // library -> test
  uint16_t sentCount() const   { return _sent.size(); }
  void setLive(bool v)         { _live = v; }
  // Answers each transmitted frame on id|0x400 with 0x74 (or 30 01 00 while the
  // library still has bytes to send, if `partialFrames` > 0 for this transfer).
  void setAutoAck(bool v)      { _autoAck = v; }
  void setAckReplyFlag(uint16_t f) { _replyFlag = f; }
  void clear() { _rx.reset(); _sent.reset(); _stats = Stats{}; }

 private:
  void synthesiseAck(const Frame& f);
  AffaRing<Frame, N> _rx;
  AffaRing<Frame, N> _sent;
  Stats    _stats{};
  uint16_t _replyFlag = 0x400;
  bool     _live = true;
  bool     _autoAck = false;
};

} // namespace affa
```

`test/` additionally provides a `FakeClock` (`uint32_t millis() const` returning a
member the test advances). It lives in `test/`, not in `src/` — it is not part of the
library's surface.

### 2.12 `widget/MenuModel.h` — the application-facing menu API

The menu is **not panel code**. It used to be: `carminat/Menu/Menu.{h,cpp}` was a two-row
sliding window with `IPanel` calls in the middle of the state machine and `row0`/`row1`
welded into the arithmetic. That file is gone. What is left is one implementation —
`widget::MenuModel` — with the display behind `widget::IMenuRenderer` and its shape in a
`widget::MenuGeometry`. `carminat/CarminatMenuRenderer` is this panel's adapter, and it is
what `CarminatDisplay` constructs for you. The full account is **docs/MENU-WIDGET.md**.

`affa::Menu`, `affa::MenuItem`, `affa::Field`, `affa::FieldType` and the three field builders
are still spelled that way in `carminat/CarminatDisplay.h` — as **aliases**, not a second
implementation. Two differences a caller can observe:

* `MenuModel::render()` returns **void**. Whether a frame reached the panel is not something
  a UI state machine can act on; the adapter is the layer holding the `IPanel`, so the
  verdict is `CarminatDisplay::menuRenderer().lastResult()`.
* the geometry is injected (`CarminatMenuRenderer::geometry()`, 2 x 26), so a row is
  truncated at 26 characters rather than at `AFFA_MENU_ROW_MAX - 1`.

There is **no `handleKey`**. The model has six intents and no key vocabulary at all; mapping
`(Key, KeyEdge)` onto them is `MenuController`'s job (§7b), and its `default:` is what keeps
SrcNext / SrcPrev / VolUp / VolDown / Pause reaching the application while the menu is open.

```cpp
#pragma once
#include "../AffaConfig.h"
#if AFFA_ENABLE_MENU

#include "IMenuRenderer.h"
#include "MenuGeometry.h"

namespace affa {
namespace widget {

enum class FieldType : uint8_t { Integer, List };

// One editable value inside a menu item. Fixed layout, no allocation. `unit` and the
// strings in `list` are CALLER-OWNED and must outlive the model — they are pointed at,
// never copied. String literals and static tables are the intended sources.
struct Field {
  FieldType type           = FieldType::Integer;
  int32_t   value          = 0;     // Integer: the value.  List: the index.
  int32_t   minValue       = 0;
  int32_t   maxValue       = 0;
  int32_t   step           = 1;
  int32_t   stepMultiplier = 1;     // multiplies `step` on increase()/decrease() (hold)
  const char*        unit  = nullptr;              // Integer only, may be null
  const char* const* list  = nullptr;              // List only
  uint8_t   listCount      = 0;
  bool      readOnly       = false;
};

// Builders — the whole item-construction API an application needs.
Field integerField(int32_t value, int32_t min, int32_t max,
                   int32_t step = 1, int32_t stepMultiplier = 10,
                   const char* unit = nullptr);
Field readOnlyField(int32_t value, const char* unit = nullptr);
Field listField(const char* const* values, uint8_t count, uint8_t index = 0);

struct MenuItem {
  const char* label = nullptr;                   // caller-owned, must outlive the model
  Field    fields[AFFA_MENU_MAX_FIELDS];
  uint8_t  fieldCount = 0;
  char     separator  = ' ';                     // between label/value and fields
  bool     editable   = true;                    // false -> select() does nothing
  // Fired after a field's value changed. Never called from an interrupt.
  void   (*onChange)(const MenuItem& item, uint8_t fieldIndex, void* ctx) = nullptr;
  // If set, select() on this item calls this INSTEAD of entering edit mode.
  void   (*onActivate)(void* ctx) = nullptr;
  void*    ctx = nullptr;
};

class MenuModel {
 public:
  using CloseCb = void (*)(void* ctx);

  MenuModel(IMenuRenderer& renderer, const MenuGeometry& geom, const char* header);

  // ---- content, owned by the application ----------------------------------
  void      setHeader(const char* h);
  int       addItem(const MenuItem& item);   // index, or -1 if full
  MenuItem* item(uint8_t index);             // nullptr if out of range
  uint8_t   count() const;
  void      clear();                         // also closes the menu, silently
  // Change a value from outside (a sensor, a web request). Redraws only if the item
  // is currently inside the visible window.
  bool      setFieldValue(uint8_t itemIndex, uint8_t fieldIndex, int32_t value);
  void      onClose(CloseCb cb, void* ctx);  // default: nothing. CarminatDisplay sets
                                             // setText("RENAULT", 0), as today.

  // ---- state --------------------------------------------------------------
  bool    isOpen()        const;
  bool    isEditing()     const;
  uint8_t selectedIndex() const;
  uint8_t selectedRow()   const;             // 0 = top row of the window
  uint8_t editingField()  const;
  uint8_t topIndex()      const;             // the item shown on row 0
  uint8_t scrollMask()    const;             // 0x00 / 0x07 / 0x0B / 0x0C, see §8.6
  const MenuGeometry& geometry() const;

  // ---- navigation: one method per intent, no Key enum ----------------------
  // Each returns true when the model consumed the intent; false means "the menu is
  // closed, this key is the application's" — the contract Menu::handleKey had.
  bool next();       // wheel down:      edit ? +1 step : selection down
  bool prev();       // wheel up:        edit ? -1 step : selection up
  bool increase();   // wheel down held: edit ? +coarse : selection down
  bool decrease();   // wheel up   held: edit ? -coarse : selection up
  bool select();     // activate / enter edit / advance to the next field
  bool back();       // leave the menu

  void open();                               // no-op on an EMPTY menu
  void close();                              // clears editing state; see §8.5

  // ---- rendering -----------------------------------------------------------
  void render();                             // re-emit the whole window. VOID: the
                                             // panel's verdict lives on the adapter
  void rowText(uint8_t itemIndex, char* out, size_t outSize) const;
};

}  // namespace widget
}  // namespace affa
#endif
```

### 2.13 `proto/` — ISO-TP and screen decoding

The transmit frame layout is specified once, in §2.9.2, and `AffaDisplayBase::pumpTx()`
builds frames inline from it — the core does **not** call into `proto/`, so a consumer
who never enables `AFFA_ENABLE_ISOTP_RX` links none of this. `isotp::fragment()` is the
same layout expressed as a function, for the twins and the tests.

That duplication is deliberate and it is fenced, and the fence is **written and passing**:
`test_isotp_edges/test_fragment_matches_the_transmit_fsm_for_every_length` drives a
payload of every length from 1 to `AFFA_MAX_PAYLOAD` through both paths and asserts the
frame sequences are byte-identical, plus `frameCount()` agreement. If the two ever
disagree, the test says so on the host, long before a panel does.

> One shape wart, inherited from this section rather than introduced by the implementation:
> `isotp::fragment()` is **declared ungated** in the header but **defined in a `.cpp` whose
> body is gated**. With both gates off, calling it is a link error rather than a compile
> error. Nothing in the library calls it and `AffaDisplay.h` does not include the header
> when the gate is off, so it is latent — but it should eventually either be inlined into
> the header or have its declaration gated too.

```cpp
// proto/IsoTp.h
#pragma once
#include "../AffaConfig.h"
#include "../core/AffaTypes.h"

namespace affa { namespace isotp {

// AFFA3 framing, matching §2.9.2 exactly:
//   frame 0 : 8 raw payload bytes, NO PCI prefix   (the 0x10 of a Carminat screen is
//             payload byte 0, not a PCI byte — see WIRE-SPEC.md)
//   frame n : [0x20 + n] then 7 payload bytes
//   every frame padded to 8 with the protocol filler (Carminat 0x00 / UpdateList 0x81)
// A 120-byte payload is 8 + 16x7 = 17 frames, so size `out` accordingly.
uint8_t fragment(uint16_t id, const uint8_t* payload, uint8_t len, uint8_t filler,
                 Frame* out, uint8_t maxOut);

// Number of frames fragment() would produce. Constexpr so a caller can size a buffer.
constexpr uint8_t frameCount(uint8_t len) {
  return (len <= 8) ? 1 : static_cast<uint8_t>(1 + (len - 8 + 6) / 7);
}

#if AFFA_ENABLE_ISOTP_RX
// The receive direction. Feed every frame in arrival order; read buffer()/len() after
// each. A frame whose data[0] is 0x10 starts a fresh message, 0x2N appends, anything
// else is ignored and leaves the buffer untouched.
//
// There is no continuation-sequence check and no gap detection, deliberately: this is
// a decoder for traffic we are watching, not a transport we depend on. A dropped frame
// yields a short or scrambled payload, which ScreenDecode rejects on length.
class Reassembler {
 public:
  bool onFrame(const Frame& f);
  const uint8_t* buffer() const { return _buf; }
  uint8_t len()    const { return _len; }
  bool    active() const { return _active; }
  void    reset()        { _len = 0; _active = false; }

 private:
  uint8_t _buf[AFFA_MAX_PAYLOAD] = {0};
  uint8_t _len = 0;
  bool    _active = false;
};
#endif

}} // namespace affa::isotp
```

```cpp
// proto/ScreenModel.h — header-only aggregate, no gate needed
#pragma once
#include <cstdint>

namespace affa {

// The decoded state of a panel screen: the semantic oracle. A test asserts
// "the header says CLOCK and row 1 says 12:30", not "the bytes matched".
//
// Offsets refer to the reassembled 0x151 showMenu payload, which begins with its
// own 0x10 0x5A bytes. They are justified byte by byte in WIRE-SPEC.md.
struct ScreenModel {
  enum class Mode : uint8_t { None, Menu, Info };

  Mode    mode      = Mode::None;
  char    header[27] = {0};   // payload [11..36]
  char    row0[26]   = {0};   // payload [39..63]
  char    row1[31]   = {0};   // payload [66..95]
  uint8_t row0Id     = 0;     // marker at [38], 0x7E
  uint8_t row1Id     = 0;     // marker at [65], 0x7F
  int16_t sel        = -1;    // highlighted row id (0x7E/0x7F); -1 = none
  uint8_t scroll     = 0;     // payload [10], the scroll-arrow byte

  char    info[3][9] = {{0}}; // info popup: 3 slots x 8 chars + NUL
  uint8_t infoCount  = 0;

  void clear() { *this = ScreenModel(); }
};

} // namespace affa
```

```cpp
// proto/ScreenDecode.h
#pragma once
#include "../AffaConfig.h"
#if AFFA_ENABLE_ISOTP_RX

#include "ScreenModel.h"
#include "../core/AffaTypes.h"

namespace affa { namespace screen {

// TWO thresholds, and the difference is the ACK model — see the note under this block.
constexpr uint8_t kMenuMinLen   = 96;  // PAYLOAD bytes our BUILDER emits
constexpr uint8_t kMenuHwMinLen = 92;  // PAYLOAD bytes a REAL panel ever receives
constexpr uint8_t kOffScroll   = 10;
constexpr uint8_t kOffHeader   = 11;   // .. 36  (26 bytes)
constexpr uint8_t kOffRow0Mark = 38;
constexpr uint8_t kOffRow0     = 39;   // .. 63  (25 bytes)
constexpr uint8_t kOffRow1Mark = 65;
constexpr uint8_t kOffRow1     = 66;   // .. 95  (30 bytes)

// Every Carminat screen — menu, now-playing, notification — is a showMenu over 0x151,
// so this one function covers all of them. No-op if len < kMenuHwMinLen. Resets `sel`:
// a fresh screen clears the highlight, exactly as the panel does.
void menu(const uint8_t* payload, uint8_t len, ScreenModel& out);

// UpdateList 8-segment setText payload on 0x121:
//   [0]10 [1]19 [2]76 [3]chan [4]loc [5..12]old(8) [13]10 [14..25]new(12)
// `new` is what the panel will show -> header; `old` -> row0.
constexpr uint8_t kSegMinLen = 26;
constexpr uint8_t kSegOld    = 5;    // .. 12  (8 bytes)
constexpr uint8_t kSegNew    = 14;   // .. 25  (12 bytes)
void segText(const uint8_t* payload, uint8_t len, ScreenModel& out);

// A single non-reassembled control frame: `07 29 01 <rowId>` -> highlight.
// Returns true if the frame was recognised and applied.
bool frame(const Frame& f, ScreenModel& out);

// Trim and copy printable ASCII from payload[a..b] inclusive, stopping at NUL.
// dstSize includes the NUL. Exposed because the tests pin it directly.
void asciiz(const uint8_t* payload, uint8_t len, uint8_t a, uint8_t b,
            char* dst, uint8_t dstSize);

}} // namespace affa::screen
#endif
```

> **Why `menu()` guards on 92 and not 96 — the same fact WIRE-SPEC states from the other
> side.** `kMenuMinLen = 96` is the length our **builder** emits. It is not the length a
> **panel** ever holds. A real Carminat terminates `showMenu` at the declared FF_DL
> (`payload[1] = 0x5A` = 90 content bytes, satisfied at 6 + 12×7), so it stops after 13
> frames and receives `payload[0..91]` — 92 bytes. The last four cells of row1 never reach
> it. Earlier revisions of this section said "no-op if `len < kMenuMinLen`", which was
> written against the self-ACK emulator's 14-frame form; under that rule **no
> hardware-faithful twin could ever decode a menu**. Both names are published because both
> facts are load-bearing, and every golden vector for `showMenu` is parameterised by ACK
> model rather than carrying a bare frame count.

### 2.14 Reading the wire back — `onText()` and `affa::screen`

`src/vpanel/` was here: `IVirtualPanel`, `VirtualPanelBase` and three panel twins, about
825 lines, behind `AFFA_ENABLE_VIRTUAL_PANEL`. They were **deleted**. They bought two
genuinely useful things, and both are still available in smaller pieces that are not
library surface:

| What the twins were for | What does it now |
| --- | --- |
| a no-hardware development loop — the library running against something that ACKs like a panel | `setSelfAck(true)` (§2.6), plus one injected `61 11` to complete the handshake |
| a semantic test oracle — `twin.screen().header == "CLOCK"` rather than a byte comparison | `isotp::Reassembler` + `affa::screen::*`, driven from the Layer-0 tap. Thirty lines; `test_bench_surface::decodeTx()` and `examples/90_bench_ota`'s `BenchScreen` are the two worked copies |

The reason for the deletion is the one that runs through this whole document: a model of a
panel is an *application* of the protocol, not part of it. Nothing in the library called
the twins, no consumer could reach them without opting into a gate, and a library that
ships its own test oracle has put its thumb on the scale — the oracle is worth more when
the code under test cannot see it.

Both replacements are behind **`AFFA_ENABLE_ISOTP_RX`**, off on target and on for the host.

#### 2.14.1 `onText()` — inbound text, with an emitter

Earlier revisions declared `EventKind::RadioText` and never constructed it (§6). It was
removed with a note in `AffaTypes.h` saying to re-add it *with* its emitter and never
before. This is that emitter, as a callback rather than an event:

```cpp
// AffaDisplayBase — behind AFFA_ENABLE_ISOTP_RX
using TextCb = void (*)(const char* text, void* ctx);
void onText(TextCb cb, void* ctx);
```

Text that **another node** drew on the panel's text channel, reassembled from its ISO-TP
frames and delivered once per complete message. In the radio role nothing else produces it
— we are the node that normally writes that channel — so this is the sniff/MITM seam, and
it is why the callback costs a gate rather than shipping on.

Three properties worth stating, because each is a silent failure if it goes the other way:

* **`text` points into library storage** and is valid only for the duration of the
  callback. Copy it if you need it afterwards.
* **Completion is the DECLARED length**, `2 + payload[1]`, not a frame count. Emitting per
  appended frame would deliver the same screen once per continuation. The one exception is
  the `AFFA_MAX_PAYLOAD` ceiling: the reassembler stops appending there, so a message
  declaring more than it can hold is delivered short rather than dropped in silence.
* **Our own renders never arrive here.** A self-sent frame coming back off an echoing link
  carries `fromSelf` and is dropped before the decoder, so `onText` is never a mirror of
  `setText`.

The split between base and panel follows `packetFiller()` / `keyTxId()`:

```cpp
 protected:
  // 0 (the default) means this panel decodes no inbound text and the reassembler is
  // never fed. Carminat 0x151, UpdateList 0x121.
  virtual uint16_t textRxId() const { return 0; }

  // Panel-specific because the command byte is: Carminat text is 0x74/0x77,
  // UpdateList's is 0x76/0x7F. Return false for a payload that is not text — a menu
  // screen, an info row — and nothing is delivered.
  virtual bool decodeText(const uint8_t* payload, uint8_t len,
                          char* out, uint8_t outSize) const;
```

The base owns the reassembly, the completion rule and the callback plumbing; the panel owns
the command byte and the offsets. `Feature::RadioText` reports this gate, and for the first
time it reports something the library can actually deliver.

#### 2.14.2 Decoding a screen yourself

`onText()` gives you a string. When you want the whole screen — header, both rows, the
highlight, the info rows — decode it yourself; that is what the twins did and it is not
much code:

```cpp
isotp::Reassembler asmb;
ScreenModel        model;

// from a Layer-0 tap, or a subscribe(), or a replayed capture
void onFrame(const Frame& f) {
  if (f.id != carminat::kIdSetText || f.len == 0) return;
  if (screen::frame(f, model)) return;     // the standalone 07 29 01 highlight
  if (!asmb.onFrame(f)) return;

  const uint8_t* p = asmb.buffer();
  const uint8_t  n = asmb.len();
  if (n < 4) return;                       // p[2] is the command, p[3] its first operand
  switch (p[2]) {
    case screen::kMenuCmd:
      if (p[3] == screen::kMenuModeWin) screen::menu(p, n, model);
      break;
    case screen::kWinTextCmdFull:
    case screen::kWinTextCmdWindow:
      screen::windowText(p, n, model);
      break;
    case screen::kInfoCmd:
      screen::infoRow(p, n, model);
      break;
    default: break;                        // unmodelled: decoded as nothing, not a guess
  }
}
```

Four traps, all of them paid for once already:

1. **Feed EVERY frame to the reassembler**, including first frames whose command you do not
   decode. Returning early on an unrecognised first frame leaves the previous message
   active, and its continuations then append to *that* buffer — which grew the menu past
   its end on every info popup.
2. **The highlight is a standalone single frame**, not ISO-TP, and `screen::frame()` carries
   the full `07 29 01` guard because `0x151` also carries `03 52 …`, `05 56 …` and
   `02 54 03`. A looser test manufactures a highlight out of one of them.
3. **Menu mode `0x05` is the fullscreen variant** and has a different layout entirely.
   Decoding it with the windowed-menu offsets produces a confident wrong screen, which is
   the one thing a semantic oracle must never do.
4. **Decode the frames you TRANSMITTED, not a model of a panel that consumed them.** Both
   worked copies read the Layer-0 tap, which is one layer closer to the glass than the
   twins were, and it is the same wiring whether a real panel is attached or not.

#### 2.14.3 What this does not prove

The same caveat the twins carried, and it has not moved: agreement between two halves of
one repository is not evidence about hardware. The decoder reads what our own encoder
produced. It is an *independent witness* only in the sense that it was transcribed from
`docs/WIRE-SPEC.md` rather than sharing code with the builders — which is why a decoder
reporting a field one byte off from what a render call put there is a **finding**, not a
calibration error. Do not move an offset to make a test pass.


---

## 3. `Result` semantics

Two disjoint populations. The set a call can return **at enqueue** is not the set that
can arrive **through `onComplete`**, and confusing them is how "it returned Ok so it
displayed" bugs get written.

| Value | Returned by an enqueue call | Delivered via `onComplete` | Meaning |
| --- | :---: | :---: | --- |
| `Ok` | yes | yes | *Enqueue:* accepted into the queue, nothing has been transmitted yet. *Complete:* the panel acknowledged the last frame with `0x74`. |
| `NoSync` | yes | no | Not passive, and `SyncState::Failed` is set. Nothing was queued. |
| `UnknownFunc` | yes | no | `funcId` is not in this panel's function table. Nothing was queued. |
| `SendFailed` | no | yes | `ICanLink::send()` refused a frame, or the panel answered with something that was neither `0x74` nor `30 01 00`, or it answered `30 01 00` when no bytes remained. |
| `Timeout` | no | yes | No ACK within `AFFA_ACK_TIMEOUT_MS` for the frame in flight. |
| `TooLong` | yes | no | `len > AFFA_MAX_PAYLOAD`, or a text argument exceeds the panel's field. Nothing was queued. |
| `QueueFull` | yes | no | `AFFA_TX_QUEUE_DEPTH` reached, counting the registration jobs the call would have to push ahead of itself. Nothing was queued. |
| `NotSupported` | yes | no | `supports(Feature)` is false for this call on this panel. Nothing was queued. |
| `BadArgument` | yes | no | Null pointer, `len == 0`, row/index out of range. Nothing was queued. |
| `LinkDown` | yes | yes | *Enqueue:* `ICanLink::isLive()` was already false. *Complete:* it went false while the job was in flight. |
| `Cancelled` | no | yes | The job was discarded: sync was lost, `begin()` was re-run, or a registration job ahead of it failed and propagated. |
| `Aborted` | **no** | **yes** | The application discarded the message before any byte of it reached the wire, through `abortPending()`, through `abortAll()`, or by enqueuing a newer message for the same `RenderSlot` (§3b.4). `Aborted` is an `onComplete`-only value: no enqueue call ever returns it, because a call cannot abort itself. It is the caller's own decision reported back, which is why it is distinct from `Cancelled` (the library discarded the job because the link or the sync went away). |

**Read the two columns as two different questions.** "Was it accepted?" is answered
synchronously and can only be `Ok`, `NoSync`, `UnknownFunc`, `TooLong`, `QueueFull`,
`NotSupported`, `BadArgument` or `LinkDown`. "Did the panel display it?" is
answered later, through `onComplete`, and can only be `Ok`, `SendFailed`, `Timeout`,
`LinkDown`, `Cancelled` or `Aborted`. `Ok` and `LinkDown` are the only two values that
appear in both columns, and they mean different things in each.

Every ticket returned non-zero by an enqueue call completes exactly once, with exactly
one of the six completion values. There is no path on which a ticket is issued and
never completed — including `abortAll()` on a job in flight, which completes it at the
frame boundary.

`lastResult()` returns the `Result` of the most recently **completed** ticket, except
immediately after a rejected enqueue, where it holds the rejection reason and
`lastTicket()` is unchanged. If you need to distinguish, compare `lastTicket()`.

A render call returns only the acceptance verdict, not the ticket. To follow one to
completion, read `lastEnqueued()` immediately after the call:

```cpp
if (display.setText("HELLO") == affa::Result::Ok) {
  const affa::TxTicket t = display.lastEnqueued();   // match this in onComplete
}
```

**`pressKey()` and `nav()` are not enqueue calls** and their `Result` is a third thing
again: it reports whether the *intent was delivered*, not whether anything was queued and
not whether anything changed on screen. They return `Ok`, `NotSupported` (no menu, no key
transmit id, or a hold edge on a wheel code with a source that includes `Wire` — §7b.6),
`LinkDown` or `SendFailed` (the `Wire` half only). Any rendering they cause is enqueued by
the menu on their behalf and reports through `onComplete` under its own ticket, which
`lastEnqueued()` will hold immediately after the call.

Tickets are issued strictly increasing, but they **do not necessarily complete in issue
order**, and code must not assume they do. `Priority::Urgent` overtakes queued `Normal`
work, and a superseded render completes `Aborted` ahead of the message that replaced
it. Anything an application builds on completion order must therefore use `onComplete`
and match the **exact ticket** it was given, never compare ticket numbers. (`Result::Busy`
used to appear in the table above, and a one-slot ticket watcher used to exist inside the
base class; both were there only for `sendBlocking()`, and all three are gone.)

---

## 3b. Latency and preemption

A key press must land **now**, not after the queue drains. This is a headline guarantee
of the library, specified here in full and pinned by tests, because getting it wrong is
invisible in code review and obvious on the bench.

### 3b.1 Two latencies, two different questions

They get conflated, and then the wrong one gets optimised. Name them separately and
measure them separately.

| | Definition | Who owns it |
| --- | --- | --- |
| **L1 — key delivery** | From the key frame entering the RX ring to the application's `KeyCb` returning from its first statement. | **The library, entirely.** |
| **L2 — reaction on the wire** | From the same instant to the first byte of the application's reaction leaving `ICanLink::send()`. | Shared: the library owns the queueing, the panel owns the ACK turnaround, the application owns what it does in the callback. |

```
key frame in ring ──L1──► KeyCb fires ──► app enqueues ──L2-L1──► first byte on the wire
                    │                                        │
        bounded by the poll period                bounded by the frame in flight,
        and by NOTHING ELSE                       not by the queue behind it
```

**L1 is bounded by the poll period and nothing else.** Not by the transmit queue depth,
not by the length of a message in flight, not by whether the TX FSM is waiting on an
ACK with a 2000 ms deadline. That is the guarantee in §3b.3.

**L2 cannot be bounded by the library alone**, and any document that claims otherwise
is lying: the message on the wire is abandoned only at a frame boundary, so the
reaction waits for at most one ACK round-trip (or one `AFFA_ACK_TIMEOUT_MS`, if the
panel has gone away). What the library *does* guarantee about L2 is that **nothing
queued behind the in-flight message contributes to it**, provided the application uses
either coalescing (automatic, §3b.4) or `Priority::Urgent` / `abortPending()`
(explicit, §3b.5).

### 3b.2 Three mechanisms, all of them the library's job

1. **Ordering inside `poll()`** — RX drain and key delivery strictly before the
   transmit pump. Fixes L1.
2. **Latest-value-wins coalescing** — a render supersedes a queued, not-yet-started
   render of the same slot instead of stacking behind it. Fixes the *stale backlog*,
   which is the part that actually looks like a latency bug.
3. **Explicit preemption** — `abortPending()`, `abortAll()`, `Priority::Urgent`. Fixes
   the residual L2 for applications that need it.

Mechanism 2 is the one that is easy to leave out and expensive to leave out. Without
it, the key arrives on time, the application reacts on time, and the panel *still*
keeps counting for a second — because a dozen stale counter values are queued in front
of the reaction. It reads as a key-handling defect and it is not one.

### 3b.3 The ordering guarantee

`poll()` performs, in this order, on every single call, with no exceptions and no
early-out that can skip a step:

```
poll():
  1. pumpRx()    while (_link.recv(f)) { sync? ack? key? -> KeyCb / CompleteCb }
  2. pumpSync()  heartbeat, sync request, peer watchdog
  3. pumpTx()    build and send at most one frame; check the ACK deadline
  4. onPoll()    panel hook
```

There is no configuration, no priority, no queue state and no error path that reorders
these. `pumpTx()` is never entered before `pumpRx()` has returned. Stated as the
sentence a test asserts:

> **The number of `poll()` calls between a key frame entering the RX ring and the key
> callback firing is exactly one, regardless of transmit queue depth or of any message
> in flight.**

`test/test_latency` enforces it as a **poll count**, at both ends of the range: with an
empty queue, and with a 96-byte `showMenu` in `WaitAck` and every one of the
`AFFA_TX_QUEUE_DEPTH` slots occupied (the next `enqueue` returning `QueueFull` is asserted
too, so the queue really is full). In both cases a `0x1C1` key frame injected into
`LoopbackLink` reaches `KeyCb` after **exactly one** `poll()`, and the in-flight job is
untouched. A five-key burst behind an ACK is delivered whole in one poll. A regression that
moves `pumpTx()` above `pumpRx()` fails every one of them.

Two consequences worth stating outright, because they are the reason the ordering is
specified rather than assumed:

* A `WaitAck` with 1900 ms left on its deadline delays **nothing** on the receive side.
  The TX FSM never waits; it checks a deadline and returns.
* `pumpRx()` drains the ring to empty, not one frame per call. A burst that arrived
  between two polls is delivered in full on the next one, in arrival order, so a key
  that arrived behind an ACK is still delivered in that same poll.

### 3b.4 Coalescing: latest value wins, per slot

**The rule.** At `enqueue()`, if `opt.coalesce` is true and `opt.slot != RenderSlot::None`,
scan the queue for a job that satisfies **all** of:

* `started == false` — not one byte of it has gone to `ICanLink::send()`;
* `slot == opt.slot`;
* `funcId == funcId`;
* `coalesce == true`;
* `kind == Payload` — Registration jobs are never coalesced (§2.9.2).

If one is found, **replace its payload in place**: copy the new bytes over it, give it
the new ticket, and complete the **old** ticket immediately with `Result::Aborted`.
Otherwise append normally. At most one job is ever replaced — the queue can never hold
two coalescable jobs for the same slot, by induction from this rule.

**Queue position is inherited, not reset.** The replacement keeps the superseded job's
position, so ordering relative to *other* slots is exactly what the application asked
for. The one exception: if the new job is `Urgent` and the superseded one was `Normal`,
the entry is also moved to the urgent insertion point (§3b.5) — a promotion cannot be
silently ignored.

**"Not yet started", defined once and precisely.** A job is *started* from the moment
`pumpTx()` has passed its first frame to `ICanLink::send()` and returned true, and it
remains started until `finishJob()` pops it. `TxJob::started` is the single authority;
no other condition (being at the head of the queue, the FSM being non-`Idle`, having a
non-zero `frameIndex`) is used anywhere to decide preemptability. A message that is
started is **never** touched by coalescing, and never has its bytes altered mid-
sequence. This is not a performance choice: rewriting the payload of a transfer whose
first frames are already at the panel would produce a screen assembled from two
different messages, and the panel has no way to detect it.

**Slot assignment.** Every render call passes a slot; an application calling `enqueue()`
directly chooses its own.

| Call | Slot | Note |
| --- | --- | --- |
| `setText` | `Text` | the counter case |
| `setTime` | `Clock` | |
| `showMenu` | `Menu` | the 96-byte screen |
| `highlightItem` | `Highlight` | deliberately **not** `Menu`: a highlight must not replace a pending full redraw, and vice versa |
| `showPopupText`, `hidePopup` | `Popup` | |
| `showFullscreenText`, `hideFullscreenText` | `Fullscreen` | |
| `showConfirmBox` | `ConfirmBox` | |
| `showInfoPopup`, `hideInfoPopup` | `InfoPopup` | |
| `setPower` | `Control` | |
| `enqueue(...)` | `None` by default | raw protocol send: never coalesced |

Note that a `hide` shares its slot with the matching `show`. That is intended: if
`showPopupText` is still queued when `hidePopup` is called, the net effect the
application asked for is "no popup", and the panel should never see the popup at all.
Latest instruction wins, per slot, is the whole semantic.

**Why the slot and not the funcId alone.** On Carminat, `showMenu`, `setText`,
`highlightItem` and `showPopupText` all transmit on `0x151`. Coalescing on the function
id would let a highlight eat a menu redraw. Coalescing on the slot *and* the function
id is the narrowest key that is still correct.

**Opting out.** `TxOptions::coalesce = false` on a specific message makes it neither a
replacer nor a replaceable. Use it when consecutive messages of the same slot are a
*sequence* rather than a *value* — an animation, or a deliberate flash where every
intermediate state must be seen. `AFFA_TX_COALESCE = 0` turns the mechanism off
library-wide; §3b.7 is what you get.

### 3b.5 Explicit preemption

```cpp
uint8_t abortPending();          // drop everything queued and not started
bool    abortAll();              // + abandon the in-flight job at a frame boundary
TxTicket enqueue(uint16_t funcId, const uint8_t* data, uint8_t len, TxOptions opt);
                                 // opt.priority = Priority::Normal | Priority::Urgent
```

**`abortPending()`** drops every `started == false` `Payload` job, reports
`Result::Aborted` through `onComplete` for each dropped ticket, and returns the count.
It does not touch the job on the wire, does not touch Registration jobs, and does not
transmit anything. It is the correct thing to call from a key callback when the key
invalidates whatever the application had queued — which is most keys.

Ordering inside `abortPending()` matters and is specified: **the queue is mutated
first, the callbacks fire second.** Consequently a nested `abortPending()` from inside
one of those `CompleteCb` invocations finds nothing to drop and returns 0, and the
recursion depth is bounded by `AFFA_TX_QUEUE_DEPTH` even if a callback enqueues and
aborts in a loop. This is the general rule for the whole library: *state first,
callbacks second* (§4.3).

**`abortAll()`** additionally sets `abandon` on the in-flight job. The FSM honours it in
`WaitAck` only, so the frame already handed to the link is transmitted whole and the
next frame of that job is never built; the job completes `Aborted` and the continuation
counter resets (§2.9.2). The panel is then holding a partial transfer. **Whether it
recovers cleanly on the next frame 0 has not been verified on hardware and must not be
assumed** — verify with `examples/90_bench_ota` before using `abortAll()` in an
application. Routine preemption does not need it.

**`Priority::Urgent`** inserts the new job after the last started job and after any
queued Registration job, and before the first queued `Normal` job. It therefore:

* never splits a message that is already on the wire;
* never overtakes function registration (the panel would reject the payload);
* overtakes any number of queued `Normal` renders;
* is FIFO among other `Urgent` jobs.

`insertIndexFor(Priority)` is the single function that computes this, and inserting in
the middle of the queue moves at most `AFFA_TX_QUEUE_DEPTH - 1` slots — three
`memmove`s of a `TxJob` at the default depth, inside `poll()`, on a queue that is
statically sized. That is the whole cost.

**Which to use.** Coalescing handles the repeated-render case with no application code
at all, and should handle it. Reach for `abortPending()` when the queued work is of a
*different* slot than the reaction (a queued clock update in front of a "PAUSED" text).
Reach for `Priority::Urgent` when the reaction must go first but the queued work is
still wanted afterwards. `abortAll()` is a bench and shutdown tool.

### 3b.6 The scenario, step by step

The user's scenario, exactly: an application renders a counter at 10 Hz; a 14-frame
menu render is on the wire; **Pause arrives while frame 3 of 14 is in flight.** The
counter must stop the moment the key is pressed.

Setup: `poll()` every 5 ms from the application task. Counter = `setText` of a 4-digit
value, a 22-byte payload → 3 ISO-TP frames, slot `Text`, `0x151`. Menu = `showMenu`, a
96-byte payload → **14 frames** (frame 0 carries 8 payload bytes, each continuation
carries 7: 8 + 13×7 = 99 ≥ 96), slot `Menu`, also `0x151`. The panel is assumed to ACK
within one poll period. `t = 0` at an arbitrary poll; the key frame lands in the RX
ring at `t = 6.2 ms`.

```mermaid
sequenceDiagram
    autonumber
    participant P as Panel
    participant L as RX ring
    participant D as AffaDisplayBase::poll()
    participant A as Application
    Note over D: P0 t=0 ms — menu frame 3/14 on the wire, "0342" queued
    P->>L: ACK 30 01 00 (frame 4)
    P->>L: 1C1 key frame — Pause (t=6.2 ms)
    Note over D: P2 t=10 ms — poll() begins
    D->>D: 1. pumpRx: ACK -> frameIndex=5
    D->>A: 1. pumpRx: KeyCb(Pause, Click)   [L1 = 3.8 ms]
    A->>D: counter stopped; abortPending()
    D->>A: CompleteCb("0342", Aborted)      [nested, no frame was sent]
    A->>D: setText("PAUSE", Urgent)
    D->>P: 3. pumpTx: menu frame 5/14 — the transfer is NOT split
    Note over D,P: P3..P11 — menu frames 6..14
    D->>P: P12 t=60 ms: "PAUSE" frame 1/3   [L2 = 53.8 ms]
```

| poll | t (ms) | 1. `pumpRx()` | 3. `pumpTx()` | queue afterwards |
| --- | --- | --- | --- | --- |
| P0 | 0 | ring empty | frame 3/14 → link; `WaitAck` | `[menu*(3), text "0342"]` |
| P1 | 5 | ACK `30 01 00` → `frameIndex = 4` | frame 4/14 → link | `[menu*(4), text "0342"]` |
| | 6.2 | — | — | **Pause key frame `0x1C1` enters the RX ring** |
| P2 | 10 | pops the ACK for frame 4, then pops the key frame → `decodeKey` → `routeKey` → the menu does not consume `Pause` → **`KeyCb` fires, `t ≈ 10.0 ms`, L1 = 3.8 ms.** The application stops incrementing and calls `abortPending()`: the queued `text "0342"` is dropped and its ticket completes `Aborted` in a nested `CompleteCb`. The application enqueues `setText("PAUSE")` with `Priority::Urgent`. | frame 5/14 → link — **the menu is not split** | `[menu*(5), text "PAUSE"(urgent)]` |
| P3…P10 | 15…50 | one ACK each | frames 6…13 | unchanged |
| P11 | 55 | ACK for frame 13 | frame 14/14 → link | unchanged |
| P12 | 60 | ACK `0x74` → menu ticket completes `Ok` | `"PAUSE"` frame 1/3 → link. **L2 = 53.8 ms**, all of it the menu finishing. | `[text "PAUSE"*(1)]` |
| P13, P14 | 65, 70 | ACK each | frames 2/3, 3/3 | `[]` → `Idle` |

**Where the counter actually stops: at P2, `t = 10 ms`.** Not because a frame stopped
going out — frames 5…14 of the menu keep going out, and they must — but because the
last counter value the panel will ever be told about is the one that completed *before*
the menu started. `"0342"` was queued and not started, so it is dropped and never
reaches the wire. Zero stale counter values are displayed after the key. Had the
application not called `abortPending()`, coalescing alone would still have capped the
damage at exactly one stale value; `abortPending()` takes it to zero.

L1 = 3.8 ms is the poll period, and would be the same if the queue had been full and a
120-byte message had been in flight. L2 = 53.8 ms is 3.8 ms of L1 plus 50 ms of "the
menu had ten frames left", and no part of it is queueing behind the reaction.

To cut L2 further there are exactly two levers, both application decisions: poll more
often (linear in L1, and it also shortens each frame's turnaround since a frame only
leaves on a poll), or call `abortAll()` and accept a partial transfer at the panel.

### 3b.7 What `AFFA_TX_COALESCE = 0` costs, arithmetically

A render loop at `f` Hz in front of a transfer that takes `T` seconds queues
`min(⌈f·T⌉, AFFA_TX_QUEUE_DEPTH − 1)` stale messages, and every render beyond that
returns `Result::QueueFull`. Both halves are wrong in a different way:

* the panel keeps displaying superseded values for `⌈f·T⌉ / f` seconds after the key —
  the exact symptom this section exists to prevent;
* renders are rejected with `QueueFull`, so the application starts dropping updates
  arbitrarily rather than dropping the *oldest* ones, and the value that finally sticks
  is not necessarily the newest.

With coalescing on, a repeated render of one slot occupies **exactly one queue slot
regardless of render rate**, and it always holds the newest value. That is also why
`AFFA_TX_QUEUE_DEPTH = 4` is enough: the depth is a function of how many distinct slots
an application drives concurrently, not of how fast it drives them.

### 3b.8 Numbers

The library's own terms are exact and computable:

| Term | Value | Where it comes from |
| --- | --- | --- |
| One 8-byte standard CAN frame at 500 kbit/s | 222 µs unstuffed, ≤ ~260 µs worst-case stuffing | 111 bit times including IFS |
| L1 worst case | one poll period + the driver's `task_CAN` delivery jitter | §3b.3 |
| L1 as a poll count | **exactly 1**, always | §3b.3 |
| Queue contribution to L2 | **0** with coalescing or `Urgent` or `abortPending()` | §3b.4, §3b.5 |
| In-flight contribution to L2 | one ACK round-trip, or `AFFA_ACK_TIMEOUT_MS` if the panel is gone | §2.9.2 |

The one term the library cannot compute is the panel's ACK turnaround, and it dominates
L2. It is measured, not estimated: `examples/90_bench_ota` timestamps each transmitted
frame and its ACK against `IClock`, and prints the min/mean/max over a 14-frame
`showMenu`. **The measured figure for the Carminat panel on the 2-node 500 kbit/s bus
must be recorded here and in the README before v0.1.0 is tagged — owner: the core
implementer, as part of the on-vehicle acceptance run.** Until that number exists,
quote L2 as "L1 + the remaining frames of the transfer in flight" and nothing more
precise; a guessed millisecond figure in a datasheet-shaped table is worse than no
figure at all.

### 3b.9 The tests that pin this section

None of the above is a claim until one of these fails when it is broken. All run on the
host against `LoopbackLink` + `FakeClock`; none needs hardware.

| Test | Asserts |
| --- | --- |
| `test_isotp/key_latency_matrix` | The §3b.3 sentence: for queue occupancy 0…`AFFA_TX_QUEUE_DEPTH` × in-flight frame index 0…13, one injected key frame plus exactly one `poll()` fires `KeyCb`. |
| `test_isotp/coalesce_latest_wins` | With `FuncsReg` already latched, 100 `setText` calls and no `poll()` between them leave exactly one job in the queue, carrying the 100th payload, with 99 tickets completed `Aborted` and zero frames transmitted. |
| `test_isotp/coalesce_never_touches_started` | A `setText` enqueued while frame 2 of a 3-frame `setText` is in flight does **not** modify the in-flight job; the wire shows both messages complete and in order. |
| `test_isotp/abort_pending_reports_aborted` | Every dropped ticket produces exactly one `onComplete(_, Aborted)`; the in-flight job is untouched; a nested `abortPending()` returns 0. |
| `test_isotp/abort_all_frame_boundary` | After `abortAll()` mid-transfer, no further continuation frame of that job appears on the wire, the job completes `Aborted`, and the next message's first transmitted frame is its own frame 0. |
| `test_isotp/urgent_never_splits` | An `Urgent` enqueue during a 14-frame transfer appears on the wire strictly after the 14th frame and strictly before every queued `Normal` job. |
| `test_isotp/urgent_never_overtakes_registration` | With `FuncsReg` unlatched, an `Urgent` payload is still transmitted after both `0x70` registration probes. |
| `test_sync/poll_frequency_independence` | §4.4: one million `poll()` calls across one simulated second emit exactly one `0xB9` and never set `Failed`. |

## 4. Threading contract

### 4.1 Where frames come from

`collin80/esp32_can` delivers frames from its own FreeRTOS tasks:

```
task_LowLevelRX (prio 19)  ->  callbackQueue (depth 16)  ->  task_CAN (prio 15)
                                                                  |
                                                          general callback
                                                                  |
                                              Esp32CanLink::ingest -> AffaRing (SPSC)
                                                                  |
                                                     ... your task calls poll() ...
                                                                  |
                                                   AffaDisplayBase::poll()
```

The general callback therefore runs **in `task_CAN`**, not in an ISR — but it is on
the critical path of every frame on the bus, and `callbackQueue` is only 16 deep. It
does one thing: copy into the ring. It must never log, never allocate, never block,
never call user code. Everything else happens in the caller's task, inside `poll()`.

### 4.2 Which context each callback fires in

| Callback | Fires from | May it call back into the library? |
| --- | --- | --- |
| `KeyCb` (`onKey`) | the task that called `poll()`, or the task that called `pressKey`/`nav` | Yes, except `poll()`. Render calls are fine — they only enqueue. `abortPending()` from here is the intended reaction to a key that invalidates queued work (§3b.5). |
| `CompleteCb` (`onComplete`) | the task that called `poll()`, or from inside `KeyCb` when a key handler aborted queued work | Same. Enqueuing the next message from here is the intended pattern. Note it may be delivering `Result::Aborted` for a message *you* just discarded — check the ticket rather than assuming it was the panel. |
| `SyncCb` (`onSync`) | the task that called `poll()` | Same. |
| `FrameTap` (`onFrame`) | the task that called `poll()` (RX) or whichever task caused a transmission (TX) — including `pressKey`, a render call is queued so its frames always leave from `poll()` | Yes, but it is on the path of **every** frame on the bus. Keep it to a ring push. Do not render from it. |
| `FrameCb` (`subscribe`) | as `FrameTap` | Same, and the same warning applies less forcefully because the match already filtered. Rendering from here is permitted and is what §7b.7a does. |
| `EventCb` (`onEvent`) | the task that called `poll()`, or `pressKey`/`nav` for `EventKind::Key` | Same as `KeyCb`. No arm of `Event` carries a pointer today; if one returns, it dies when the callback returns (§2.1). |
| `Field::onChange` / `MenuItem::onChange` / `onActivate` / `MenuModel::CloseCb` | the task that called `poll()`/`pressKey`/`nav` | Same. Note `onChange` fires *before* the resulting re-render is enqueued. |
| `ILogSink::write` | any of the above, plus `Esp32CanLink::begin()` | **No.** Treat it as a leaf. It may be called with the library's internal state mid-transition. |

A callback must not block. It is running inside `poll()`; anything it waits for that
needs `poll()` to happen will not happen.

### 4.3 Re-entrancy

`poll()` sets `_inPoll` for its duration. A nested `poll()` returns immediately having
done nothing. This is a guard, not a feature — do not build on it. (It also refused the
now-deleted `sendBlocking()`, which is what `Result::Busy` was for.)

**State first, callbacks second.** Every callback in the library is fired *after* the
transition that caused it is complete: `finishJob()` pops the job before it invokes
`CompleteCb`; `abortPending()` empties the pending set before it reports any
`Aborted`; `setSync()` stores the new state before it invokes `SyncCb`. A callback
therefore always observes a consistent library, and a re-entrant call from inside one
sees the world as it is, not as it was.

Callbacks consequently nest, legitimately. The intended and tested shape is: `poll()` →
`pumpRx()` → `KeyCb` → the application calls `abortPending()` → `CompleteCb(ticket,
Aborted)` for each dropped message. `CompleteCb` running inside `KeyCb` is normal. What
is not allowed from any callback is `poll()`; everything else,
including `enqueue`, every render call, `abortPending()`, `abortAll()`, `pressKey()`
and `nav()`, is permitted. Nesting depth is bounded by `AFFA_TX_QUEUE_DEPTH`.

Two re-entrancy rules specific to the observation seam, stated here so §7b does not have
to repeat them. **`subscribe()` and `unsubscribe()` are legal from inside a `FrameCb`**;
the table is walked by index against the generation counter, so a slot freed mid-walk is
skipped rather than mis-dispatched, and a slot added mid-walk is not delivered the frame
currently being dispatched. And **`pressKey(..., KeySource::Wire)` from inside a
`FrameCb` is legal and is the shape §7b.7a uses** — the frame it transmits is observed
after the current dispatch finishes, so the tap sees frames in wire order, not nested.

### 4.4 `poll()` is frequency-independent

> Every periodic behaviour in the library is a comparison against `IClock::millis()`.
> Nothing counts calls. Calling `poll()` once per second and calling it a million
> times per second produce **the same frames, in the same order, with the same
> timing**; they differ only in how soon a received frame is noticed and in how much
> CPU is burned. There is no minimum call rate for correctness — only for latency and
> for keeping `Stats::ringOverflow` at zero.

Sizing follows from that last clause, not from the protocol: at 500 kbit/s a full
8-byte frame takes ~228 µs, so `AFFA_RX_RING_DEPTH = 32` tolerates a ~7 ms gap between
`poll()` calls on a fully saturated bus, and far longer on the 2-node bus this library
actually runs on.

Two tests enforce this, one per profile. `test_core` calls `poll()` a million times across
one simulated second on the Carminat profile; `test_sync_profiles` does the same 20 000
times on the UpdateList profile, because a call-counting implementation would emit a storm
on **both** and the FSM is shared. Each asserts that

* exactly **one** heartbeat was transmitted (`0xB9` / `0x79`), and no sync request, and
* `SyncState::Failed` was never set — the link did not break.

Run the same test with `poll()` called twice per simulated second and the transmitted
frame sequence must be identical.

### 4.5 What an application task may call

Safe from any task, at any time, as long as **only one task drives the library**:
`poll()`, `enqueue()`, every render call, `abortPending()`, `abortAll()`,
`pressKey()`, `nav()`, `subscribe()`, `unsubscribe()`, `onFrame()`, `onEvent()`,
`getMenu()` and everything on `Menu`, all the observers (`syncState`, `busy`,
`pending`, `stats`, …).

None of these is safe from an **ISR**, including `pressKey()` — it can route into the
menu, which renders, which enqueues. An ISR-sourced key goes into a FreeRTOS queue and
is replayed through `pressKey()` from the task that owns `poll()`.

The library is **not internally locked**. If two tasks must reach it, either put a
mutex around every entry point in your adapter, or — better — give the second task a
queue and drain it from the task that owns `poll()`.

The one thing that is genuinely concurrent, `Esp32CanLink::ingest` writing into
`AffaRing` from `task_CAN` while `poll()` reads, is handled by the ring's SPSC
discipline and needs no lock.

### 4.6 The recommended shape for key handling

Split the reaction in two. The part that decides *what must stop* is cheap, cannot
block, and belongs in the callback — deferring it is what puts stale frames on the
panel. The part that talks to WiFi, NVS or a media player is expensive and belongs in
the application task.

```cpp
// Fires inside poll(), before the transmit pump. Cheap half only.
static void onKey(affa::Key k, affa::KeyEdge e, void* ctx) {
  auto* app = static_cast<App*>(ctx);
  if (k == affa::Key::Pause) {
    app->counterRunning = false;      // stop producing renders
    app->display->abortPending();     // discard the ones already queued
    app->display->setText("PAUSE");   // reaction, ahead of nothing
  }
  app->queueForLater(k, e);           // expensive half
}
```

Then the expensive half, off the callback:

```cpp
struct KeyEvent { affa::Key k; affa::KeyEdge e; };
static QueueHandle_t g_keys;   // xQueueCreate(8, sizeof(KeyEvent))

// App::queueForLater — still inside poll(). Never blocks, never allocates, never logs.
void App::queueForLater(affa::Key k, affa::KeyEdge e) {
  const KeyEvent ev{k, e};
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(g_keys, &ev, &woken);   // FromISR variant: never blocks, ever
}

void appTask(void*) {
  for (;;) {
    display.poll();                          // drains RX, may fire onKey
    KeyEvent ev;
    while (xQueueReceive(g_keys, &ev, 0) == pdTRUE)
      handleKeyProperly(ev.k, ev.e);         // WiFi, NVS, whatever you like
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
```

Note what is **not** deferred: `abortPending()` and the reaction render stay in the
callback. Deferring them by one task wakeup would put the reaction behind whatever the
counter enqueued in the meantime and reintroduce exactly the backlog §3b exists to
prevent. Defer the slow work, never the preemption.

`examples/03_carminat_menu` ships this shape. **The library will not own that loop for
you**: it creates no task, and there is no flag that makes it — see the `AFFA_ENABLE_TASK`
row in §5.3, which documents a knob that was specified, never implemented, and now
`#error`s rather than silently doing nothing.

---

## 5. `src/AffaConfig.h` in full

This is the single knob header. Everything optional in the library is gated from here.

### 5.1 Why every optional `.cpp` gates its whole body

PlatformIO's `build_src_filter` applies to the **project's** `src/` directory. A
library pulled in through `lib_deps` is built by the Library Dependency Finder, which
compiles **every** `.cpp` it finds under the library's `src/`. The consumer's
`build_src_filter` cannot reach into it, and `library.json`'s `srcFilter` is a static
declaration in the library that cannot see the consumer's `build_flags` — so it cannot
react to `-D AFFA_PANEL_CARMINAT=1`.

The preprocessor is therefore the only mechanism that can. Each optional translation
unit wraps its **entire** body:

```cpp
// carminat/CarminatDisplay.cpp
#include "../AffaConfig.h"          // the ONLY thing outside the gate
#if AFFA_PANEL_CARMINAT

#include "CarminatDisplay.h"
// ... the entire implementation ...

#endif // AFFA_PANEL_CARMINAT
```

and the matching header does the same, so a call site that references a
disabled panel fails to **compile** (loudly, at the call site) rather than failing to
**link** (obscurely, at the end of the build).

With the panel off, the translation unit compiles to an empty object file. Combined
with the toolchain's `-ffunction-sections -fdata-sections -Wl,--gc-sections` — already
on for `platform = espressif32` — nothing from it reaches the image. That, and only
that, is what makes the flash cost of an unused panel actually zero. The README carries
the measured table.

### 5.2 The panel gates

```cpp
// Panel selection. SILENCE IS AN ERROR, NOT A DEFAULT: name at least one panel in
// build_flags. "Compile all three" is available but has to be asked for.
#ifndef AFFA_PANEL_DEFAULT_ALL
#  define AFFA_PANEL_DEFAULT_ALL 0
#endif
#if AFFA_PANEL_DEFAULT_ALL
#  ifndef AFFA_PANEL_CARMINAT
#    define AFFA_PANEL_CARMINAT 1
#  endif
#  ifndef AFFA_PANEL_UPDATELIST
#    define AFFA_PANEL_UPDATELIST 1
#  endif
#  ifndef AFFA_PANEL_UPDATELIST_MENU
#    define AFFA_PANEL_UPDATELIST_MENU 1
#  endif
#endif

#ifndef AFFA_PANEL_CARMINAT
#  define AFFA_PANEL_CARMINAT 0
#endif
#ifndef AFFA_PANEL_UPDATELIST
#  define AFFA_PANEL_UPDATELIST 0
#endif
#ifndef AFFA_PANEL_UPDATELIST_MENU
#  define AFFA_PANEL_UPDATELIST_MENU 0
#endif

// UpdateListMenuDisplay derives from UpdateListDisplay: selecting the LCD variant
// necessarily compiles the 8-segment base too.
#if AFFA_PANEL_UPDATELIST_MENU && !AFFA_PANEL_UPDATELIST
#  undef  AFFA_PANEL_UPDATELIST
#  define AFFA_PANEL_UPDATELIST 1
#endif

// The guard, and the ONE thing that catches a mis-typed panel flag. With
// -D AFFA_PANEL_CARMINET=1 every real macro stays undefined, so all three end up 0 and
// this fires. An earlier revision made "define nothing" mean "compile all three", which
// turned exactly that typo into a bigger image and no diagnostic whatsoever.
#if !AFFA_PANEL_CARMINAT && !AFFA_PANEL_UPDATELIST && !AFFA_PANEL_UPDATELIST_MENU
#  error "AffaDisplay: no panel selected. Add -D AFFA_PANEL_CARMINAT=1 (and/or _UPDATELIST / _UPDATELIST_MENU), or -D AFFA_PANEL_DEFAULT_ALL=1 for all three."
#endif
```

The development-loop gates follow the same shape, with one difference: their default
depends on where you are building.

```cpp
// Inbound multi-frame decode: the ISO-TP reassembler, the screen decoder, and the
// onText() callback they feed. For sniffing another head unit or replaying a capture —
// nothing in the radio role needs it, so it is off on target and on for the host, where
// the tests live. PlatformIO defines ARDUINO for framework=arduino and not for native.
//
// AFFA_ENABLE_VIRTUAL_PANEL (removed) gated vpanel/, a set of panel twins used as a test
// oracle and as a dev loop with no panel attached. They were application-shaped code
// shipped as library surface; setSelfAck() covers the no-panel loop, and a decoder built
// on the two headers this gate buys covers the oracle. See section 2.14.
#ifndef AFFA_ENABLE_ISOTP_RX
#  if defined(ARDUINO)
#    define AFFA_ENABLE_ISOTP_RX 0
#  else
#    define AFFA_ENABLE_ISOTP_RX 1
#  endif
#endif

// src/widget/Marquee and, with it, UpdateListDisplay's setScrollText / setScrollActive /
// reassert. A widget rather than protocol, like the menu — but ON by default, because it
// is small and eight segment cells do not hold a track title.
#ifndef AFFA_ENABLE_MARQUEE
#  define AFFA_ENABLE_MARQUEE 1
#endif
```

There used to be a feature gate here that defaulted to 0 rather than 1 —
`AFFA_ENABLE_AUX_TRACKER`, which compiled in `AuxModeTracker`. It was the boundary
principle in `#define` form: every other `AFFA_ENABLE_*` gate describes something the
PANEL defines and is on by default, whereas that one described *someone else's product*.
The gate and the class are both gone — nothing in the library depended on it, no test
covered it and no example used it, and a default-off feature that nobody switches on is
not a feature. Its reverse-engineered pattern table survives in `docs/PROTOCOL-NOTES.md`
§8, where it belongs: as an observation an application may act on, not as a claim the
library makes. `AFFA_ENABLE_MENU` still carries the same principle — default 0, because
the menu is a widget and not the protocol (§5.3).

`AFFA_ENABLE_ISOTP_RX` used to carry a dependent gate above it — the twins, which could not
work without the decoder — and the pair was policed by an `#error` rather than a silent
`#undef`/`#define` promotion, unlike `AFFA_PANEL_UPDATELIST_MENU`. With `src/vpanel/` gone
the gate stands alone and there is nothing left to contradict, so the `#error` went with it.
The principle it stood on is still the live one: selecting the LCD panel without its base is
obviously a spelling of "I want the LCD panel" and gets promoted, whereas two flags that
directly contradict each other mean one of them is a mistake, and guessing which would hide
it.

Every gate is `#define`d to 0 rather than left undefined, and every use is `#if`, never
`#ifdef`. A `-Wundef` build (which the library's own `platformio.ini` turns on) then
catches a misspelling *inside the library*: `#if AFFA_ENALBE_MENU` is a diagnostic rather
than a silently false branch.

**`-Wundef` does not catch a misspelling in a consumer's `build_flags`, and cannot.**
`-D AFFA_PANEL_CARMINET=1` defines a macro that nothing ever reads, so there is no
undefined macro anywhere and nothing to diagnose. That is why the panel flags carry a
second mechanism: naming none of them is an `#error`, and a mis-typed flag lands in exactly
that state. For the feature gates the same class of typo is not solvable in the
preprocessor at all; the observable symptom is "the block I tried to switch off is still in
the image", which the map file and the flash number report.

### 5.3 Every macro

| Macro | Default | What it costs / buys | Failure mode if set wrong |
| --- | --- | --- | --- |
| `AFFA_PANEL_CARMINAT` | **0** — naming no panel is an `#error` | Carminat frame builders, key decode, `0x151`/`0x1F1` tables. | 0 while using a Carminat: `CarminatDisplay` is not declared — compile error at your call site. |
| `AFFA_PANEL_UPDATELIST` | as above | AFFA2 base + 8-segment display + title scroll. | as above |
| `AFFA_PANEL_UPDATELIST_MENU` | as above | LCD `setText` channel/location encoding. Forces `AFFA_PANEL_UPDATELIST` on. | as above |
| `AFFA_ENABLE_MENU` | **0** | `widget/` (`MenuModel`, `IMenuRenderer`, `MenuGeometry`), `CarminatMenuRenderer`, `MenuController`, `IPage` routing, `nav()`, `getMenu()`. The single largest optional block, and **off by default**: the menu is a widget, not protocol. `showMenu()` / `highlightItem()` are unconditional and stay available with this at 0. | 0 (the default): `nav()` returns `NotSupported`, `getMenu()` is not declared, `supports(Feature::Menu)` is false. A menu-driven application stops compiling — which is the point; set it to 1 in your `build_flags`. |
| `AFFA_ENABLE_POPUP` | 1 | `showPopupText` / `hidePopup` (mode `0x74` overlay). | 0: both return `NotSupported`. |
| `AFFA_ENABLE_FULLSCREEN` | 1 | `showFullscreenText` / `hideFullscreenText` (`0x21` mode `0x05`). | 0: both return `NotSupported`. |
| `AFFA_ENABLE_CONFIRMBOX` | 1 | `showConfirmBox` and its offset builder. | 0: returns `NotSupported`. |
| `AFFA_ENABLE_INFOPOPUP` | 1 | `showInfoPopup` / `hideInfoPopup` (the 3-row info menu). | 0: returns `NotSupported`. |
| `AFFA_ENABLE_TRANSLITERATION` | 1 | `AffaText.cpp` and its ~1.2 kB mapping table. | **0 is dangerous.** `toAscii` becomes a bounded copy that passes bytes through unchanged; any UTF-8 that reaches the wire renders as garbage on the panel. Set it to 0 only if you have proved every string is already 7-bit ASCII. Not a compile error — a visual one. |
| `AFFA_ENABLE_LOG` | 1 | The `AFFA_LOG*` macros and `AffaLog.cpp`. | 0: every macro expands to `do {} while (0)`; **no format strings enter flash**. Side effects written inside a log argument vanish (§2.6). |
| `AFFA_LOG_LEVEL` | 3 (info) | 0 off … 5 trace. Compile-time: levels above it emit nothing at all. | Too high on a live bus floods the sink; the `0x3AF`/`0x3CF` sync chatter is ~2 frames/s and trace prints all of it. |
| `AFFA_ENABLE_ESP32CAN_LINK` | 1 | `Esp32CanLink.{h,cpp}` and the `<esp32_can.h>` dependency. | 0 on a project that uses a different CAN driver: nothing pulls `esp32_can` in, and you supply your own `ICanLink`. 1 without the dependency in `lib_deps`: link error. |
| `AFFA_ENABLE_ISOTP_RX` | 0 on target, 1 on host | `isotp::Reassembler` + `screen::*` + the `onText()` path — decoding inbound multi-frame traffic (sniffing another head unit, replaying a capture) and the callback that reports it (§2.14). | 1 on a shipping target in the radio role: you pay ~900 B flash / ~384 B RAM for a decode path nothing in that role calls. 0 on the host: `test_seam`'s `onText` cases and `test_bench_surface`'s wire oracle stop compiling — which is the point, since a host build without them tests bytes instead of meaning. |
| `AFFA_ENABLE_MARQUEE` | 1 | `widget::Marquee`, and with it `UpdateListDisplay::setScrollText` / `setScrollActive` / `reassert` / `setReassertOnAux`. A widget, not protocol — but a cheap one. | 0 on a Carminat-only build: it costs nothing there anyway, since nothing names it. 0 on an UpdateList build: the 8-segment panel shows the first eight characters of a title and no more, which is usually not what anyone wants. |
| `AFFA_ENABLE_TASK` | — | **NOT IMPLEMENTED.** An owned FreeRTOS task that calls `poll()` was specified here and written nowhere: there is no `vTaskCreate` under `src/`, and there will not be one while "no `vTaskDelay` in `src/`" (§4) is the contract. Until the review that found this, the knob existed, defaulted to 0 and was referenced by nothing — so a consumer who set it to 1 got a library that never polled, with no diagnostic. | Setting it to 1 is now an `#error`. Own the loop: call `poll()` from exactly one task. `AFFA_TASK_PERIOD_MS`, `AFFA_TASK_STACK` and `AFFA_TASK_PRIO` were removed with it. |
| `AFFA_TX_COALESCE` | 1 | Latest-value-wins replacement of a queued, not-yet-started render of the same `RenderSlot` (§3b.4). Costs one linear scan of at most `AFFA_TX_QUEUE_DEPTH` entries per enqueue, and 4 bytes per `TxJob`. Buys a bounded queue under any render rate. | 0: a repeated render stacks. At `f` Hz in front of a `T`-second transfer you get `min(⌈f·T⌉, depth−1)` stale messages on screen after the key and `QueueFull` for the rest (§3b.7) — the panel keeps counting after Pause. Set it to 0 only when consecutive same-slot messages are a sequence that must all be seen, and prefer `TxOptions::coalesce = false` on those specific messages instead. |
| `AFFA_TX_QUEUE_DEPTH` | 6 | `sizeof(TxJob)` ≈ `AFFA_MAX_PAYLOAD + 12` bytes each ≈ 750 B of static RAM at the defaults. 6 and not 4 because `showInfoPopup` is three non-coalescing messages and the first call after a resync also carries two registration probes: 2 + 3 = 5 outstanding, plus one slot of headroom for an `Urgent`. | Below 3 the lazy registration burst (2 probes + 1 payload for either panel) cannot fit and the first send after a resync returns `QueueFull` forever. Below what your app bursts: renders start returning `QueueFull`. |
| `AFFA_MAX_PAYLOAD` | 113 | Largest single ISO-TP message. **113 is a wire limit, not a budget**: 8 bytes in frame 0 plus 15 continuations of 7 is `8 + 15×7 = 113`, the point at which the continuation counter would wrap. `showConfirmBox` sits at exactly 113. The Carminat menu screen is 96 bytes; the `setText` payload is 22. | Below 96: `showMenu` returns `TooLong` and the menu never draws. Above 113: `#error`, unless you define `AFFA_UNSAFE_LONG_PAYLOAD` after bench-validating a longer transmit. |
| `AFFA_PANEL_DEFAULT_ALL` | 0 | Opt in to "compile all three panels" instead of naming them. For a first look and for the footprint reference builds; `size_all` is the one environment here that uses it. | 1 in a shipping build: you compile panels you do not use. `--gc-sections` removes the unused ones from the image, so the cost is build time, not flash (README, Footprint). |
| `AFFA_UNSAFE_LONG_PAYLOAD` | undefined | Escape hatch for `AFFA_MAX_PAYLOAD > 113`. | Defining it disables the ceiling `#error`. The ISO-TP counter wrap past 15 continuations has never been validated against a panel. |
| `AFFA_RX_RING_DEPTH` | 32 | `32 × sizeof(Frame)` = 448 B static RAM. Must be a power of two. | Too small: `Stats::ringOverflow` climbs, ACKs are lost, sends time out, sync flaps. Non-power-of-two: `static_assert` fires. |
| `AFFA_ACK_TIMEOUT_MS` | 2000 | Per-frame ACK deadline. 2000 matches the legacy blocking wait exactly, so timing-sensitive panel behaviour is unchanged. | Too low: a slow panel yields `Timeout` mid-transfer and the screen is left half-drawn. Too high: a dead panel wedges the queue for that long per frame (it no longer wedges the *loop* — that was defect #2). |
| `AFFA_PEER_TIMEOUT_MS` | 5000 | How long the link may go without a `69` ping before sync is torn down. The panel pings ~1 Hz. | Below ~3000 you tear down on a single missed ping. This is the value the old `SYNC_TIMEOUT = 5` counter was *meant* to express. |
| `AFFA_SYNC_INTERVAL_MS` | 1000 | Heartbeat cadence, enforced inside `poll()`. | Changing it changes what the panel sees; 1000 is what the capture shows and what the panel expects. Treat as fixed. |
| `AFFA_MAX_SUBSCRIPTIONS` | 8 | Size of the Layer 1 `FrameMatch` table (§7b.4). `sizeof(Sub)` ≈ 32 B, so 8 slots ≈ 256 B of static RAM, and one linear scan of it per frame in either direction. | `subscribe()` returns `kNoSub` once full, and an application that ignores the return value silently loses a subscription — check `valid()`. **0 removes the table entirely**: `subscribe()` always returns `kNoSub`, `unsubscribe()` always returns false, and the scan is compiled out. Layers 0 and 2 are unaffected. |
| `AFFA_MENU_MAX_ITEMS` | 12 | `12 × sizeof(MenuItem)` static RAM (≈ 1.1 kB at 3 fields). | `addItem` returns -1 past the limit; the item is silently absent from the menu. |
| `AFFA_MENU_MAX_FIELDS` | 3 | Fields per item. | An item with more fields than this: the extras are dropped at `addItem`. |
| `AFFA_MENU_ROW_MAX` | 32 | Rendered row buffer. The Carminat window row is 26 usable bytes. | Below 27 truncates rows that would have fitted on screen. |
| `AFFA_TEXT_MAX` | 64 | Transliteration scratch buffer on the stack of each render call. | Below the longest string you pass: silently truncated (never mid-sequence, always NUL-terminated). |

Recommended `build_flags` for this project's board:

```ini
build_flags =
  -D AFFA_PANEL_CARMINAT=1
  -D AFFA_ENABLE_MENU=1
  -D AFFA_ENABLE_TRANSLITERATION=1
  -D AFFA_ENABLE_LOG=1
  -D AFFA_LOG_LEVEL=3
```

---

## 6. Capability model

Not every panel does popups, fullscreen or menus. The 8-segment UpdateList has eight
characters and no window at all.

```cpp
bool supports(Feature f) const;   // pure virtual on AffaDisplayBase; each panel answers
```

| Feature | Carminat | UpdateList (8-seg) | UpdateList (LCD) |
| --- | :---: | :---: | :---: |
| `Text` | yes | yes | yes |
| `Time` | yes | no | no |
| `Power` | yes | yes | yes |
| `Menu` | yes (if `AFFA_ENABLE_MENU`) | no | no |
| `Popup` | yes (if `AFFA_ENABLE_POPUP`) | no | no |
| `Fullscreen` | yes (if `AFFA_ENABLE_FULLSCREEN`) | no | no |
| `ConfirmBox` | yes (if `AFFA_ENABLE_CONFIRMBOX`) | no | no |
| `InfoPopup` | yes (if `AFFA_ENABLE_INFOPOPUP`) | no | no |
| `KeyTx` | yes (`0x1C1`) | yes (`0x0A9`) | yes (`0x0A9`) |
| `RadioText` | yes (if `AFFA_ENABLE_ISOTP_RX`) | yes (if `AFFA_ENABLE_ISOTP_RX`) | yes (if `AFFA_ENABLE_ISOTP_RX`) |

`supports()` reflects both the panel **and** the compile-time gates: a Carminat built
with `AFFA_ENABLE_POPUP=0` reports `supports(Feature::Popup) == false`, and
`showPopupText` returns `Result::NotSupported`.

> **`Feature::RadioText` reports a compile gate — and, since `onText()`, a gate that buys
> something.** Both panels answer it with `AFFA_ENABLE_ISOTP_RX != 0`, and with that gate on
> the base reassembles inbound ISO-TP on the panel's text id and delivers a decoded string
> to `onText()` (§2.14.1). For most of this library's life the answer was true about the
> reassembler being *compiled* and false about anything reaching the application; that gap
> is closed.
>
> Earlier revisions carried an `EventKind::RadioText` and an `EventKind::ScreenChanged`
> against that future wiring, plus a canary test asserting that neither ever fired. **Both
> enumerators were removed**, and the canary with them, with a note in `AffaTypes.h` saying
> to re-add them *with* an emitter and never before. `onText()` is that emitter — as a
> callback rather than an event, because it carries a pointer into library storage and the
> event union's arms were the delicate part. The rule stands for anything that comes next:
> the emitter and the thing it emits land in the same change.
>
> **What did NOT go, and must not be confused with it:** `UpdateListBase` decodes the
> radio's inbound `0x121` text and reports it through the protected virtual
> `onRadioText(bool isAux)`. That hook is real, it is exercised, and it stays. It is a
> single-frame AUX heuristic on a subclass seam — not `onText()`, which delivers the whole
> reassembled string, and not a published event.
>
> An application that wants inbound text as a **string** uses `onText()`. One that needs
> the raw bytes — because the discriminator it wants is a header byte a decoded string does
> not carry — uses `subscribe()` (Layer 1) on the panel's text id instead.
> `examples/08_radio_mitm` is the second case: its AUX classifier needs the `setText` format
> byte. `docs/PROTOCOL-NOTES.md` §8 has the pattern table that used to live in
> `AuxModeTracker`.

> **The one deliberate behaviour change versus the code being extracted.** The legacy
> `IDisplay` gave `showInfoPopup`, `showConfirmBox`, `showFullscreenText`,
> `showPopupText` and friends **silently no-op default bodies returning
> `AffaError::NoError`**. Calling one on a panel that could not do it looked exactly
> like success. Here every unsupported call returns `Result::NotSupported`, and
> `supports()` lets you ask before you call. Applications that relied on the silent
> no-op will now see a non-`Ok` return where they previously saw `NoError` — guard with
> `supports()` (see `examples/02_carminat_text`) rather than ignoring the result. This
> is called out again in the README.

---

## 7. Migration from the extracted classes

| Old (MegaOpen) | New (AffaDisplay) | Note |
| --- | --- | --- |
| `#include "display/Carminat/CarminatDisplay.h"` | `#include <AffaDisplay.h>` | one umbrella header |
| `AffaCommon::AffaKey` | `affa::Key` | same wire values; `SrcRight`→`SrcNext`, `SrcLeft`→`SrcPrev`, `VolumeUp`→`VolUp`, `VolumeDown`→`VolDown` |
| `bool isHold` | `affa::KeyEdge` | `false`→`Click`, `true`→`Hold` |
| `AffaCommon::AffaError` | `affa::Result` | `NoError`→`Ok`, `StrTooLong`→`TooLong`; values 0..5 unchanged numerically |
| `AffaCommon::SyncStatus` | `affa::SyncState` | same bits; `FUNCSREG`→`FuncsReg` etc. |
| `Frame` (global) | `affa::Frame` | field `extended` → `ext` |
| `ICanBus` + `onReceive` push callback | `affa::ICanLink` + `recv()` pull | **the** structural change; see §0.2 |
| `IClock::delayMs()` | *(removed)* | nothing in the library sleeps |
| `CanUtils::sendCan/sendFrame/sendMsgBuf` | *(removed)* | all TX goes through `ICanLink::send` |
| `HwCanBus` | `affa::Esp32CanLink` | now the only file that knows the driver exists |
| `display.tick()` + `display.recv(f)` + `display.processEvents()` | `display.poll()` | one pump; RX is drained by the library, you no longer feed it frames |
| `setKeyHandler(bool(*)(AffaKey,bool))` | `onKey(KeyCb, void* ctx)` | now returns `void` and takes a context pointer; routing/consumption is decided inside the library |
| `setSkipFuncReg(bool)` | `setPassive(bool)` | same semantics, honest name |
| `setEmuSelfAck(bool)` | `setSelfAck(bool)` | unchanged behaviour |
| `setClock(IClock&)` / `setBus(ICanBus&)` | constructor arguments | neither may change after `begin()` |
| `syncStatus()` / `synced()` / `funcsRegistered()` | `syncState()` / `synced()` / `registered()` | |
| `setText(const char*, uint8_t)` | same signature, returns `Result` | now **asynchronous**: `Ok` means queued. It also carries `RenderSlot::Text`, so a repeated call supersedes a queued one instead of stacking (§3b.4) — an application that rendered in a tight loop and relied on every call reaching the panel must pass `TxOptions::coalesce = false` |
| *(no equivalent — the send blocked)* | `abortPending()` / `abortAll()` / `Priority::Urgent` | a blocking send needed no preemption because there was never a queue. There is one now, so preemption is part of the contract (§3b.5) |
| *(no equivalent)* | `onComplete(cb, ctx)` | the delivery verdict the old blocking return value carried. `AffaError` from `affa3_send` ≈ the `Result` now delivered here, not the one returned by the render call |
| `showMenu(h, i1, i2, scroll)` | same signature, returns `Result` | now asynchronous |
| `highlightItem(uint8_t)` | same | now asynchronous |
| `showConfirmBoxWithOffsets(...)` | `showConfirmBox(...)` | the offset-taking builder is internal |
| `showInfoMenu(i1,i2,i3,o1,o2,o3,prefix)` | `showInfoPopup(l1,l2,l3)` | the offsets are internal constants; the `delay(5)` between its frames is gone — the TX FSM paces them |
| `getMenu()` returning the `String`/`vector` `Menu` | `getMenu()` returning the fixed-capacity `affa::widget::MenuModel` (still spelled `affa::Menu` — the alias in `CarminatDisplay.h`) | see §8.7 for the new item-building API and §2.12 for what the change of type means |
| `MenuItem(String, {Field...})` | `affa::MenuItem{ .label = "...", .fields = {...} }` + `integerField/listField/readOnlyField` | no `String`, no `std::function`; callbacks are function pointers with a `ctx` |
| `Menu::handleMessage(const CAN_FRAME&)` | *(removed)* | it was already a no-op, and it was the only reason the original `Menu.h` included `<esp32_can.h>` |
| `Menu::handleKey(Key, KeyEdge)` | *(removed)* → `MenuController::routeKey()` + `MenuModel::next/prev/increase/decrease/select/back()` | the model has no key vocabulary, so the `(Key, KeyEdge)` map moved up one layer into navigation policy. The fall-through for SrcNext / SrcPrev / VolUp / VolDown / Pause is unchanged and now lives in that map's `default:` |
| `Menu::render()` returning `Result` | `MenuModel::render()` returning `void` | the verdict is `CarminatDisplay::menuRenderer().lastResult()`: only the layer holding the `IPanel` can answer it |
| `IDisplay::isCarminat()` | `supports(Feature)` | ask about the capability, not the model |
| `emulateKey(AffaKey, bool hold)` (file-static in `CarminatDisplay.cpp`) | `pressKey(Key, KeyEdge, KeySource::Wire)` | it built the `0x1C1` frame by hand and pushed it through `CanUtils`. The wire bytes are unchanged. `KeySource::Wire` is the source that reproduces it exactly, and it must be passed explicitly — the default is `Local`, which drives our own menu and transmits nothing (§7b.6) |
| *(no equivalent — `ProcessKey` was the only entry)* | `pressKey(..., KeySource::Local)` / `nav(NavCommand)` | the input seam: a web console, a BLE remote or a test now drives the same path as the wheel (§8.3) |
| `sendPasswordSequence()` in `CarminatDisplay.cpp` | *(not in the library)* → `examples/08_radio_mitm` | one car, one radio, `delay(1000)`+`delay(200)`. Reimplemented against `subscribe()` + `pressKey(..., Wire)` and fully non-blocking — §7b.7a. **The library never emulates a key on its own initiative** |
| `AuxModeTracker::onCanMessage(const CAN_FRAME&)` | your own `FrameCb` on a `subscribe()` of `0x151` | **deleted**, gate and all. It was extracted out of the RX path, then shipped default-off, then removed once it was clear nothing used it. The seven patterns are tabulated in `docs/PROTOCOL-NOTES.md` §8 and implemented in `examples/08_radio_mitm` — §7b.7b |
| `_aux.onCanMessage(*packet)` inside `CarminatDisplay::recv` | *(removed)* | the display no longer feeds the tracker. Subscribe and feed it yourself, or write your own heuristic |
| hold-Load hard-wired in `Menu::handleKey` | `setMenuHotkey(Key, KeyEdge)` / `clearMenuHotkey()` | same default (Load + Hold), now replaceable — §7b.7c |
| `IVirtualDisplay::pressKey(uint16_t, bool)` | *(gone with the twins)* | it was renamed to `transmitKey` first, so it could not be confused with `AffaDisplayBase::pressKey`, which by default also has a local effect. To impersonate a panel now, call `pressKey(k, e, KeySource::Wire)` on a display — that is the same wire half, and it is public API (§7b.6) |
| `attachMediaRouter` / `setMediaInfo` / `tickMedia` / `onElmUpdate` / `onBtDisconnected` | *(not in the library)* | media, ANCS and ELM stay in the application; drive the library with `setText`/`showMenu` |
| `ISettings` (NVS for menu items) | *(not in the library)* | the application owns persistence; put your NVS write in `MenuItem::onChange` |
| `IsoTp::Reassembler` / `IsoTp::fragment` (global `IsoTp` namespace) | `affa::isotp::*` in `proto/` | unchanged semantics; `int` lengths become `uint8_t` and `MAX_PAYLOAD` becomes `AFFA_MAX_PAYLOAD` (§2.13) |
| `ScreenDecode::*`, `ScreenModel` | `affa::screen::*`, `affa::ScreenModel` in `proto/` | `item0`/`item1` → `row0`/`row1`; `Mode` becomes an `enum class` |
| `vdisplay/IVirtualDisplay`, `VirtualDisplayBase`, `*VirtualDisplay` | *(not in the library)* | briefly `vpanel/IVirtualPanel` etc., then **deleted**: a model of a panel is an application of the protocol, not part of it. `setSelfAck(true)` replaces the ACK half; `isotp::Reassembler` + `affa::screen` replace the decode half, in about thirty lines you own (§2.14) |
| `extern bool _autoTime` | *(gone)* | it was a host global reaching into the display |

Minimal shape of the migrated bring-up:

```cpp
affa::Esp32CanLink   link;
ArduinoClock         clock;                       // your 3-line IClock
affa::CarminatDisplay display(link, clock);

void setup() {
  link.begin(affa::CanPins{.rx = GPIO_NUM_4, .tx = GPIO_NUM_3}, 500000);
  display.onKey(onKey, nullptr);
  display.onComplete(onDone, nullptr);
  display.begin();
}

void loop() {
  display.poll();          // that is the whole integration
}
```

---

## 7b. The boundary principle and the three-layer observation seam

### 7b.1 The boundary principle

One rule settles almost every "library or application?" argument in this codebase.

> **The library owns what the PANEL defines. The application owns what the CAR, the
> RADIO or the USER defines.** Where a policy also happens to be the OEM convention, it
> ships as a DEFAULT THAT CAN BE TURNED OFF — never as something the application cannot
> reach or replace.

| Panel-defined — **library** | Car/radio/user-defined — **application** |
| --- | --- |
| the wire format and ISO-TP framing | which radio is on the bus |
| the sync handshake and function registration | which text strings mean which audio source |
| key **encoding** and decoding (the `0xC0` hold mask, the wheel-code exemption, the per-family key id) | which key opens which screen |
| the menu's wire primitives — `showMenu`'s two rows, the highlight frame, what the scroll-arrow bytes draw — but **not** the state machine above them (§8.2) | what to do about a password prompt |
| the screen decoder | what the menu items are and what they change |
| transliteration for the panel's charset | persistence of anything the user edits |

Three pieces of the extracted code sit on the application side of that line and are
**not** in the library. They are also the proof that this seam is cut in the right
place: each must be reimplementable by an application using only public API, and §7b.7
does exactly that for all three. If one of them could not be expressed cleanly, the
seam would be wrong and would have to move — not be papered over.

1. `sendPasswordSequence()` — a man-in-the-middle trick for one car and one radio.
   → `examples/08_radio_mitm`, §7b.7a.
2. `AuxModeTracker` — heuristic inference of a radio's source from decoded text.
   The library supplies the **mechanism** (a raw subscription); the **policy** moves out
   entirely. The class first became an optional default-off helper and has now been
   deleted; the reverse-engineering it held is a table in `docs/PROTOCOL-NOTES.md` §8,
   which preserves the work without passing it off as universal. → §7b.7b.
3. "hold Load opens the menu" — UI policy that happens to be the OEM convention, so it
   stays as a **configurable default**. → `setMenuHotkey()`, §7b.7c.

### 7b.2 The physical topology, and why keys only ever come in

Everything about keys follows from this. It is confirmed by the project owner and it is
reproduced in the README for the same reason it is here: the protocol's shape is
unintelligible without it.

```
[joystick] --wired--> [PANEL] --CAN 1C1 03 89 hi lo--> [RADIO]
                         ^                                |
                         +------- CAN 0x151 screens ------+
```

The joystick is physically part of the **panel**. Pressing it makes the panel encode and
transmit a key frame. The radio receives that frame and decides what to draw next; it
then sends the next screen. A button pressed on the radio's own front panel produces no
CAN key frame at all — the radio just draws the next screen directly.

**This library's normal role is the RADIO.** Therefore keys only ever come IN. The
library never transmits a key frame in normal operation, and the panel is not a listener
for its own key id — so a key frame we transmit on a two-node bench bus is addressed to
nobody.

### 7b.3 Layer 0 — the tap

```cpp
void onFrame(FrameTap cb, void* ctx);   // FrameTap = void(*)(const Frame&, Direction, void*)
```

Every frame, in and out, unfiltered, in wire order. For sniffers, loggers, and the web
console's frame ring. It never filters and it must never block. One tap; a second call
replaces the first; `nullptr` removes it.

Both directions pass through the same choke point, `observe(f, d)`, so a tap sees the
whole bus in the order it happened rather than two interleaved half-views. RX frames are
observed inside `pumpRx()`; TX frames are observed from `txFrame()`, immediately after
`ICanLink::send()` accepted them — a frame the link **refused** is not observed, because
it never existed on the bus. `Stats::txDropped` and `EventKind::LinkError` are where a
refused frame shows up.

### 7b.4 Layer 1 — filtered raw subscription

Raw-frame subscriptions are not a fallback for a poor event set. They exist because an
application doing something the library never anticipated — the password trick is the
canonical case — needs the bytes, and re-deriving the protocol from a tap to get them
would throw away the library's whole value.

```cpp
SubHandle subscribe(const FrameMatch& m, FrameCb cb, void* ctx);
bool      unsubscribe(SubHandle h);
uint8_t   subscriptions() const;
```

`FrameMatch`, `SubHandle`, `FrameCb` and `Direction` are declared in §2.1. The table is
`AFFA_MAX_SUBSCRIPTIONS` entries (default 8), statically allocated, never grown.
`subscribe()` returns `kNoSub` when the table is full or the match is unsatisfiable
(`dir` with no bits set, or `len > 8`); **check `valid()`** — an ignored return value is
a subscription that silently never fires.

**The matching rule, exactly.** For a frame `f` observed in direction `d`:

```
matches(m, f, d) :=
      (bit(d) & bit(m.dir)) != 0                              // direction
   && ((f.id ^ m.id) & m.idMask) == 0                         // id under mask
   && (m.len == 0 || f.len >= m.len)                          // long enough
   && for i in [0, m.len):
        ((f.data[i] ^ m.data[i]) & m.dataMask[i]) == 0         // bytes under mask
```

Consequences, each of which has bitten someone in a previous life:

* `m.len == 0` means **id only** — the payload is not examined at all. That is the
  correct shape for "feed me everything on `0x151`" (§7b.7b).
* A frame **shorter** than `m.len` never matches, even if every byte it does carry
  agrees. AFFA frames are always 8 bytes, so this only fires on malformed traffic —
  where silence is the right answer.
* A `dataMask[i]` of `0x00` makes `data[i]` irrelevant; the pair `{data, dataMask}` is
  a value/care mask, not a range.
* A **default-constructed** `FrameMatch` matches id `0x000` inbound and nothing else.
  AFFA uses no such id, so a half-filled match is inert rather than a firehose.
  Matching every id is the explicit opt-in `idMask = 0`.

**Dispatch is observational and can never consume.** Subscriptions fire *before* the
library's own handling of the frame, and nothing a callback does prevents that handling:
a subscription on `0x3CF` cannot suppress the sync FSM, and one on `0x151|0x400` cannot
swallow an ACK. The per-frame order inside `pumpRx()` is fixed:

```
tap (Layer 0)  ->  subscriptions (Layer 1)  ->  library consumption  ->  events (Layer 2)
```

Layer 2 fires last because a decoded event is a *conclusion* about the frame, and the
library has to have drawn it first.

Slots are scanned in index order. Registration order is preserved only until a slot is
freed and reused, so **do not depend on the order two subscriptions fire in**; if two of
your callbacks must be ordered relative to each other, that is one callback.

Cost: one linear scan of the table per frame per direction. At 8 slots and the ~4
frames/s of an idle AFFA bus this is unmeasurable; it is still bounded and static, which
is the property that matters. `AFFA_MAX_SUBSCRIPTIONS = 0` compiles the table and the
scan away entirely (§5.3).

### 7b.5 Layer 2 — decoded protocol events

The library already knows what the bytes mean. Hiding that and making every application
re-derive it would be perverse — so the conclusions are published.

```cpp
void onEvent(EventCb cb, void* ctx);    // EventCb = void(*)(const Event&, void*)
```

`EventKind`, `Event`, `LinkErrorKind` and `EventCb` are declared in §2.1. One sink; a
second call replaces the first.

`Event` is a **tagged union**, and the choice is worth defending on a RAM-tight target.
`std::variant` costs an index, alignment padding and a valueless-by-exception state that
a `-fno-exceptions` build cannot even reach; a class hierarchy costs a vtable pointer per
event and forces the event object to outlive the callback. The tagged union is 12 bytes
of POD, built on the `poll()` stack, copied nowhere and allocated never.

| `EventKind` | Union member | Fires when |
| --- | --- | --- |
| `SyncChanged` | `sync{prev, now}` | `setSync()` stored a state word different from the previous one. Never fires on a no-op write. |
| `Registered` | `sync{prev, now}` | the last registration job completed `Ok` and `FuncsReg` latched. Fires in addition to the `SyncChanged` that carries the same bit. |
| `PeerLost` | `sync{prev, now}` | the peer-alive deadline expired in `pumpSync()`. `now` already has `Failed` set and `FuncsReg` cleared. |
| `Key` | `key{key, edge}` | a key was decoded from the wire, **or** `pressKey`/`nav` was called with a source that includes `Local`. Fires after the menu has had the key, so `ev.key` is what arrived, not what was left over. |
| `TxComplete` | `tx{ticket, result}` | exactly the information `CompleteCb` carries, for applications that want one sink instead of five. |
| `LinkError` | `error{kind, count}` | ring overflow, a `send()` the link refused, or the controller's own error counters advancing. `count` is the running total, not a delta. |

Layer 2 fires **in addition to** `KeyCb`, `CompleteCb` and `SyncCb`, never instead of
them. Installing both is legal and delivers both.

No arm of `Event` carries a pointer today — the two that did, `text` and `screen`, went
with `EventKind::RadioText` and `EventKind::ScreenChanged` (§6). Should one return, so
does its rule: a pointer inside an `Event` points at library-internal storage and is valid
**only for the duration of the callback**. Copy what you need. Keeping the pointer
compiles, and works, right up until the next frame arrives.

**Firing context and re-entrancy, stated once for all three layers:** every callback in
every layer fires from the task that called `poll()` — never from the CAN task, never
from an ISR — with the single exception that `pressKey`/`nav` deliver their `Local` half
(and `Wire`'s tap/subscription dispatch) synchronously on the caller's stack. All three
may call back into the library, including render calls, `abortPending()` and
`pressKey()`; none may call `poll()`. The full rules, including
"state first, callbacks second" and the legality of `subscribe()` from inside a
`FrameCb`, are in §4.2 and §4.3 and are not repeated here.

### 7b.6 `KeySource`, `pressKey`, `nav`, and the echo rule

```cpp
enum class KeySource : uint8_t {
  Local = 1,   // as if a key arrived: drive our menu + fire the Key event.
               // Nothing goes on the bus. THE DEFAULT, because in the radio role
               // that is what a key press IS from our side.
  Wire  = 2,   // impersonate the panel: encode and transmit the key frame. Only
               // meaningful when a REAL radio is on the bus and you are driving it.
  Both  = 3,   // both at once. Rarely what you want — see below.
};
Result pressKey(Key k, KeyEdge e, KeySource src = KeySource::Local);
Result nav(NavCommand c,          KeySource src = KeySource::Local);
```

**One function with a source, not two lookalike functions.** An "inject locally" and a
"send on the wire" sitting side by side with near-identical names is a trap: they would
be confused at a call site sooner or later, and the failure is silent in one direction
(a key that did nothing) and loud on a vehicle bus in the other.

**`Local` is the default for both, deliberately.** An earlier draft of this design
defaulted `pressKey` to `Both` on the reasoning that "emulating a real press should look
real from every angle". That was wrong, and the topology in §7b.2 is why: in the radio
role we are the RECEIVER of key frames, so transmitting one does not make the emulation
more faithful — it puts a frame on the bus that nothing is listening for, pollutes the
trace, and could confuse a third node. The reasoning is more useful than the rule; a
future reader will re-propose `Both` otherwise.

**So why does `Wire` exist in the library at all, rather than the application just
building a frame and calling `link.send()`?** Because the key ENCODING is panel-defined
and therefore ours by the boundary principle: the hold mask (`0xC0`), the exemption of
the two encoder codes `0x0101` / `0x0141` from that mask, and the per-family key id
(`0x1C1` on Carminat, `0x0A9` on UpdateList). Making an application hand-assemble
`03 89 <hi> <lo>` with the right mask is exactly the duplication the library exists to
prevent. `Wire` is a key frame ENCODER; only its default was ever in question.

**The caveat that belongs in the README next to "begin() transmits":**
`KeySource::Wire` puts phantom button presses on the bus. On a bench with one panel that
is harmless and is exactly what §7b.7a needs. On a **vehicle** bus it is injecting input
that other modules may act on. Said plainly: do not ship `Wire` in a car unless you know
precisely which module is listening and what it will do.

**The echo rule, and why it is not optional.** A real CAN controller never receives its
own transmissions, but `LoopbackLink` does — so without a rule, `pressKey(..., Both)`
would fire once on hardware and twice on the host, and every host test would be lying
about the target. Therefore:

> **The library tags every frame it transmits with `Frame::fromSelf`, and the RX key
> decoder ALWAYS ignores such frames.** The local effect of a self-sent key is decided
> solely by `KeySource`, never by whether the transport happens to echo.

That makes behaviour identical on `Esp32CanLink` and on `LoopbackLink`, which is the
property that makes the host tests worth anything. `test/test_keysource` runs the same
`pressKey(..., Both)` against both links and asserts the key fires **exactly once** on
each. A frame that arrives through `recv()` with `fromSelf` set is additionally
presented to Layers 0 and 1 as `Direction::Tx`, never as `Rx`, so a `dir = Rx`
subscription means "what the other node actually sent" on every link, echoing or not.

`Result` from `pressKey`/`nav` reports **whether the intent was delivered**, not whether
anything was queued and not whether anything changed on screen (§3):

| Result | Cause |
| --- | --- |
| `Ok` | the requested halves were delivered |
| `NotSupported` | `Local` with no menu compiled in and no `KeyCb` installed; or `Wire` on a panel with no key transmit id (`supports(Feature::KeyTx)` is false); or `Wire` with a `Hold` edge on `RollUp`/`RollDown` |
| `LinkDown` | `Wire`, and `ICanLink::isLive()` was false |
| `SendFailed` | `Wire`, and the link refused the frame |

The `Wire` + `Hold` + wheel-code case is a refusal, not a downgrade: a hold edge on a
wheel code has **no wire representation at all** (§8.3), and silently transmitting the
click form would produce a fine step where the caller asked for a coarse one — a wrong
screen the caller cannot detect. `NavCommand::Increase` and `Decrease` are therefore
reachable only with a source of `Local`.

With `Both`, the `Wire` half is attempted first and a failure there is returned even
though the `Local` half already happened. That is the honest answer: the caller asked
for both.

### 7b.7 The three examples, worked

#### 7b.7a The radio password sequence — `examples/08_radio_mitm`

The extracted `sendPasswordSequence()` watched for one specific decoded payload on
`0x151` and then emulated the remote's `5-3-2-1 + hold` keypresses to defeat a radio's
code prompt, with `delay(1000)` and `delay(200)` in it. Everything about it is
car-and-radio specific; none of it is panel-defined. It leaves the library and comes
back as an example, non-blocking.

The trigger is the second ISO-TP frame of the radio's PIN screen, observed on the bench
as exactly `21 20 20 B0 30 30 30 20` on `0x151` — two spaces, the masked-digit glyph,
then `000 ` (MeganeCAN `src/display/Carminat/CarminatDisplay.cpp`, the branch that
called `sendPasswordSequence()`).

```cpp
// The PIN, and its pacing. Both are radio policy: the library has no opinion about
// either, which is precisely why this file is an example and not a feature.
constexpr uint8_t  kCode[]    = {5, 3, 2, 1};
constexpr uint32_t kArmMs     = 1000;   // the radio is still drawing when we match
constexpr uint32_t kSpacingMs = 200;    // the radio's key debounce, measured

class PinEntry {
 public:
  PinEntry(affa::AffaDisplayBase& d, affa::IClock& c) : _d(d), _c(c) {}

  // Layer 1 callback. Fires inside poll(). It ARMS and returns — it does not press.
  // A callback that waited here would be the old delay(1000) with extra steps, and it
  // would stall the very poll() that has to deliver the ACKs for what it sends.
  static void onPrompt(const affa::Frame&, void* ctx) {
    auto* self = static_cast<PinEntry*>(ctx);
    if (!self->_done) return;                        // already running
    self->_done = false;
    self->_digit = self->_detent = 0;
    self->_nextMs = self->_c.millis() + kArmMs;
  }

  // Called from the application loop, beside poll(). One press per due deadline.
  void tick() {
    if (_done) return;
    if (static_cast<int32_t>(_c.millis() - _nextMs) < 0) return;
    _nextMs = _c.millis() + kSpacingMs;

    if (_detent < kCode[_digit]) {                   // one wheel detent
      _d.pressKey(affa::Key::RollUp, affa::KeyEdge::Click, affa::KeySource::Wire);
      ++_detent;
      return;
    }
    const bool last = (_digit + 1u == sizeof kCode);  // the final Load is a HOLD
    _d.pressKey(affa::Key::Load,
                last ? affa::KeyEdge::Hold : affa::KeyEdge::Click,
                affa::KeySource::Wire);
    _detent = 0;
    if (last) { _done = true; return; }
    ++_digit;
  }

 private:
  affa::AffaDisplayBase& _d;
  affa::IClock&          _c;
  uint32_t _nextMs = 0;
  uint8_t  _digit  = 0, _detent = 0;
  bool     _done   = true;
};
```

Wiring, once, in `setup()`:

```cpp
static const uint8_t kPrompt[8] = {0x21, 0x20, 0x20, 0xB0, 0x30, 0x30, 0x30, 0x20};

affa::FrameMatch m{};
m.id     = 0x151;
m.idMask = 0x7FF;
m.dir    = affa::Direction::Rx;          // what the RADIO sent, never our own echo
memcpy(m.data,     kPrompt, 8);
memset(m.dataMask, 0xFF,    8);          // all eight bytes must match exactly
m.len    = 8;

if (!display.subscribe(m, &PinEntry::onPrompt, &pin).valid())
  AFFA_LOGE("MITM", "subscription table full");   // never ignore this
```

```cpp
void loop() {
  display.poll();
  pin.tick();          // the only "timer" involved, and it belongs to the application
}
```

Four things this demonstrates, and one it deliberately does not.

* **`KeySource::Wire`, not `Local`.** We are impersonating the panel at a real radio.
  A `Local` press would drive our own menu and tell the radio nothing.
* **The echo rule earns its keep here.** On `LoopbackLink` the four transmitted key
  frames come straight back; because they carry `fromSelf`, our own key decoder ignores
  them and our menu does not lurch through four items. The host test of this example
  therefore observes exactly what a bench run observes.
* **The `dataMask` is doing real work.** No decoded event the library publishes would
  have identified this screen; the application needed the bytes, and it got them without
  re-deriving the protocol.
* **The blocking is gone, not moved.** `delay(1000)` and four `delay(200)`s became two
  deadlines on the application's own clock. Meanwhile `poll()` keeps running, the sync
  heartbeat keeps its 1 Hz cadence, and a key arriving mid-sequence is still delivered
  within one poll.
* **What the library did not supply: the timer.** Pacing keypresses at 200 ms is radio
  policy, so the pacing lives in the application, driven by the same `IClock` the
  library uses. That is the seam behaving correctly, not a gap in it.

#### 7b.7b AUX-source detection

The extracted `AuxModeTracker` pattern-matched the text the radio drew — `"AUX"`,
`"RENAULT"`, `"TR 1 CD"`, `"M 1056"`, `"L 1056"`, `"   1056"`, and a leading `"> "` — to
infer which source it was playing. Those patterns describe **a radio**, not a panel.
Policy, therefore application.

**The class is gone**, and so is the `AFFA_ENABLE_AUX_TRACKER` gate. It travelled a full
arc: hard-wired into `CarminatDisplay::recv()`, then extracted to a free-standing
default-off helper, then deleted. The last step is the honest end of the second one — a
default-off class with no test, no example, no caller and nothing in the library depending
on it is not a preserved capability, it is a maintained liability with a `#ifdef` in front
of it.

**The knowledge is preserved, the code is not.** `docs/PROTOCOL-NOTES.md` §8 tabulates all
seven patterns, the 200 ms header/continuation pairing, the `text[0]`-is-not-text index
rule and the `0x59` format-byte threshold, with the reason for each. That is the right
shelf for it: an observation about one Renault radio family, offered to an application
that may act on it, rather than a verdict the library asserts.

**What an application writes instead** is a Layer 1 subscription, roughly twenty lines.
Note it works on **raw frames**, not on decoded text, and that is not a downgrade: the
discriminator includes header byte 6 of the `0x10` frame — the `setText` format byte,
where `>= 0x59` marks plain ASCII and `< 0x59` the radio-digit style — which no
reassembled string carries. The library publishes no decoded-text event (see §6), so
nothing was lost in the move.

```cpp
// Feed it every frame the RADIO sent on the text channel; classify the 0x21 that
// follows a 0x10 within 200 ms. docs/PROTOCOL-NOTES.md §8 has the full table.
static void onRadioFrame(const affa::Frame& f, void* ctx) {
  auto* app = static_cast<App*>(ctx);
  if (f.len < 8) return;                       // short DLCs are real on this bus
  if (f.data[0] == 0x10) { app->keepHeader(f); return; }
  if (f.data[0] != 0x21 || !app->headerFresh()) return;
  app->classify(app->header(), f.data);        // -> AUX / radio / CD / retain
}

affa::FrameMatch m{};
m.id  = 0x151;                    // 0x121 for UpdateList
m.dir = affa::Direction::Rx;      // len stays 0: id only, and never our own echo
if (!display.subscribe(m, &onRadioFrame, &app).valid()) { /* table full */ }
```

`examples/08_radio_mitm` is this, working, with two of the seven patterns implemented.

In the extracted code `CarminatDisplay::recv()` called `_aux.onCanMessage(*packet)`
unconditionally: every consumer paid for the heuristic, nobody could replace it, and the
tracker's verdict was indistinguishable from a protocol fact. Now there is no verdict in
the library at all, and the frames are yours.

#### 7b.7c A custom menu-open hotkey

"Hold Load opens the menu" was hard-wired inside the extracted `Menu::handleKey`. It *is* the OEM
convention for this panel, so it stays — as a default that can be replaced or removed.

```cpp
void setMenuHotkey(Key k, KeyEdge e);        // default: Key::Load, KeyEdge::Hold
void clearMenuHotkey();                      // nothing opens the menu but nav(Open)
bool menuHotkey(Key& k, KeyEdge& e) const;   // false when cleared
```

This governs **opening only**. Once the menu is open, routing keys into it — which key
scrolls, which enters edit mode, which redraws versus which re-highlights — is the
widget's behaviour, and it is fixed rather than configurable (§8.5). It is fixed because
it reproduces the panel's own convention, not because the panel enforces it: the way to
get different routing is to replace `MenuController` or the widget entirely, against the
two unconditional calls (§8.2), not to look for a knob.

Three shapes, all public API:

```cpp
// 1. A different gesture, because this installation's stalk has no comfortable hold.
display.setMenuHotkey(affa::Key::SrcNext, affa::KeyEdge::Hold);

// 2. No gesture at all: the menu opens only when the application says so — from a web
//    console, a BLE remote, a long-press decoded elsewhere, or a test.
display.clearMenuHotkey();
display.nav(affa::NavCommand::Open);         // from the task that owns poll() (§4.5)

// 3. Double-click Load opens it. Pure application policy, expressed in the KeyCb.
//    Note this needs (2) first: with the default hotkey live, hold-Load would still
//    open the menu behind your back.
static void onKey(affa::Key k, affa::KeyEdge e, void* ctx) {
  auto* app = static_cast<App*>(ctx);           // app->display is a CarminatDisplay* — §8.7
  if (k == affa::Key::Load && e == affa::KeyEdge::Click && !app->display->getMenu().isOpen()) {
    const uint32_t now = app->clock.millis();
    if (now - app->lastLoadMs < 400) app->display->nav(affa::NavCommand::Open);
    app->lastLoadMs = now;
    return;
  }
  /* ... the application's other keys ... */
}
```

Shape 3 works because **a closed menu consumes nothing**: with the hotkey cleared, every
key reaches `KeyCb` while the menu is shut (§8.5). Once it is open the menu consumes
`Load` and the wheel, and it should — that is the panel's behaviour and an application
that fought it would produce a screen no Renault driver recognises.

A web console reaching `nav()` from its own HTTP task must go through a queue drained by
the task that owns `poll()`; the library is not internally locked (§4.5).

### 7b.8 Verdict on the seam

All three moved out cleanly, using only `subscribe()`, `onEvent()`, `pressKey()`,
`nav()` and `setMenuHotkey()`. No example needed a library internal, a friend
declaration, or a "just this once" accessor, and none of them blocks.

Two library-side facts were load-bearing, and both are consequences of the boundary
principle rather than conveniences:

* **`KeySource::Wire` had to be in the library.** Without it, §7b.7a would hand-assemble
  `03 89 <hi> <lo>` with the `0xC0` mask and the wheel-code exemption — panel-defined
  knowledge, duplicated in an application, wrong the first time the second panel family
  is used.
* **Layer 1 had to carry a payload mask.** With id-only filtering, §7b.7a would have to
  re-implement enough of the screen format to recognise its trigger.

And one extracted behaviour had to change: the display stopped feeding `AuxModeTracker`,
and the tracker has since been deleted outright (§7b.7b). That is not a regression, it is
the seam — a heuristic about someone else's radio has no business on the protocol layer's
critical path, and once it was off that path nothing in the library needed it at all.

---

## 8. The input seam: does menu navigation belong in the library or the application?

### 8.1 The question, and the answer this section used to give

An OEM panel has a wheel and a button, and the menu it draws is unmistakably wire-shaped:
a fixed 96-byte payload, fixed row offsets, a separate one-frame highlight, a scroll byte
derived from the selection. An earlier revision of this section read that as proof that
**the library owns the menu mechanism**, and said so in those words. `AFFA_ENABLE_MENU`
was on by default to match.

> **That position was overturned on purpose; it was not softened, and this is not a
> clarification of it.** If you have read the old text — here, or the same argument as it
> used to stand in `src/AffaConfig.h` — the sentence "the library owns the menu
> mechanism" is no longer the project's position and no longer describes the code.
> `AFFA_ENABLE_MENU` defaults to **`0`**, the state machine lives in `src/widget/` with no
> panel dependency at all, and `showMenu` + `highlightItem` are what remains
> unconditional. The reversal is recorded at the gate itself (`src/AffaConfig.h`, "OFF BY
> DEFAULT, AND THE REASON IS A CORRECTION"), in `docs/MENU-WIDGET.md` and in the README's
> "The menu is a widget, not the protocol". They agree with this section; nothing else in
> the tree still argues the old line.

The old argument failed on one conflation: **being derived from the wire is not the same
as being the wire.** Every fact in the bullet list below is genuinely panel-defined, and
every one of them is discharged by two calls. What the argument then smuggled in — which
items exist, which is selected, how a window slides over N of them, what a field is, when
Select advances to the next field and when it exits — is nowhere in that list, because the
panel has no opinion about any of it. It was one opinion about how a menu should behave,
shipped as though it were the protocol, and it was also the one part of the library that
behaved unexpectedly on the bench. That is not a coincidence: it was the only place the
library decided something on the application's behalf.

### 8.2 The boundary, as it stands

**The library owns exactly two calls.** They are declared on `IPanel` (§2.4), implemented
in `CarminatDisplay` outside every menu gate, and available on every build regardless of
`AFFA_ENABLE_MENU`:

```cpp
[[nodiscard]] Result showMenu(const char* header, const char* row0,
                              const char* row1, uint8_t scrollIndicator);
[[nodiscard]] Result highlightItem(uint8_t row);   // 0x7E = row 0, 0x7F = row 1
```

Header, two rows, which one is lit, which arrows. That is the whole wire contract, and
these are the facts that make it that and not something larger:

* A Carminat menu screen is a single **96-byte ISO-TP payload on `0x151`** beginning
  `10 5A 21 01 7E 80 00 00 82 FF <scroll>`, with the header at byte 10, row 0 at fixed
  offset 37 preceded by `00 7E`, and row 1 at fixed offset 64 preceded by `01 7F`. The
  panel renders exactly **two rows**. Not a list — a two-row sliding window. Hence
  `showMenu`'s three strings, and hence a `MenuGeometry` whose `rows` is a parameter
  rather than a constant.
* The selection highlight is **a different frame entirely**: `0x151 : 07 29 01 7E|7F
  80 00 00 00`, where `7E` means row 0 and `7F` means row 1. Moving the selection
  inside the visible window costs one frame; moving it outside costs a full 96-byte
  redraw. Hence two calls rather than one — and hence `RenderSlot::Menu` and
  `RenderSlot::Highlight` are separate slots (§3b.4), so a highlight can never coalesce
  away a redraw.
* The scroll-arrow byte is computed from the selection's position in the list:
  `0x0B` bottom arrow only, `0x07` top arrow only, `0x0C` both (§8.6). The panel defines
  what those bytes mean; it does not define who counts the items.
* The row tags, the offsets and the `0x21`/`0x29` command bytes are the panel's, and no
  application should ever have to know them. That is what these two calls buy.

**Everything above those two calls is the application's**, and the library is not in the
way of any of it: which items exist, what they are called, what a field means, what
happens when it changes, whether the change is written to NVS, which gesture opens the
menu, and where the key came from.

**The library ships one opinion about the layer in between, off by default.**
`widget::MenuModel` (§2.12) is the sliding-window state machine — items, fields,
selection, window arithmetic, editing, clamping, the coarse step — with `rows`,
`rowChars` and `wrap` injected as `MenuGeometry` and **no panel header, no CAN and no
`Result` anywhere in it**. It draws through `widget::IMenuRenderer`;
`CarminatMenuRenderer` is the adapter that turns its output into the two calls
above, and `examples/09_menu_widget` drives one identical model onto three different
displays, the third of which touches no AFFA protocol at all. `src/widget/` is therefore
gated on `AFFA_ENABLE_MENU` **alone**, with no panel gate — it compiles on the host with
nothing but the C++17 standard library. Turn it on if it fits; write your own against
`showMenu()` + `highlightItem()` + the decoded `Key` events if it does not; or keep the
state machine and write your own `IMenuRenderer`. See `docs/MENU-WIDGET.md`.

| Panel-defined — **library, unconditional** | Widget — **`AFFA_ENABLE_MENU`, default 0** | Application — **always** |
| --- | --- | --- |
| the 96-byte screen layout, row offsets, row tags | which item is on row 0, which row is lit | what the items are and what they mean |
| the highlight frame and its `7E`/`7F` | when a move is a highlight and when it is a redraw | what a change is worth doing about |
| what `0x07`/`0x0B`/`0x0C` draw | which of them the current window implies | how many items there are |
| ISO-TP framing, `RenderSlot`, coalescing | edit mode, fields, step and coarse step | persistence, validation, units |
| key **encoding** and decoding | the `(Key, KeyEdge)` → intent map (`MenuController`) | which gesture should open a menu at all |

The one row that is genuinely a panel *convention* rather than a panel *definition* — hold
Load opens and closes, click Load activates or steps a field, the wheel scrolls or edits
depending on mode — follows §7b.1's rule for exactly that case: it ships as a **default
that can be turned off**, `setMenuHotkey()` / `clearMenuHotkey()` (§7b.7c), never as
something the application cannot reach.

### 8.3 Why the input must be a seam and not a source

The panel's wheel and buttons are only **one producer** of navigation intent. A web
console, a BLE remote, a steering-wheel key matrix and a unit test are equally valid
producers. So the library exposes the *entry* to key handling, not just the *exit*:

```cpp
Result pressKey(Key k, KeyEdge e, KeySource src = KeySource::Local);
Result nav(NavCommand c,          KeySource src = KeySource::Local);
```

`pressKey`'s `Local` half calls the same `routeKey()` the wire decoder calls. There is
exactly one path, so anything a test or a web page can drive is provably what the panel
drives. That is the whole justification for the seam: without it, "works from the
browser" and "works from the wheel" are two different claims requiring two different
tests.

There is deliberately **no second function** for the wire direction — the source is an
argument, not a name (§7b.6). Both default to `Local`, because in the radio role a key
press is something we RECEIVE; `KeySource::Wire` is for impersonating the panel at a
real radio and is never what menu navigation wants.

There is a concrete, load-bearing reason this is not academic. Look at the wire
decoder: the hold mask `0x80|0x40` is applied to the low byte of the key code **except
for the wheel codes `0x0101` and `0x0141`**, because those already use those bits.
Consequently:

> **A hold edge on `RollUp`/`RollDown` can never arrive from the panel.**
> `NavCommand::Increase` and `NavCommand::Decrease` — the coarse `stepMultiplier` step —
> are reachable *only* through `pressKey`/`nav` with a source that includes `Local`.

The coarse-step feature exists in the menu code and the panel physically cannot reach
it. Either the feature is dead code, or input is a seam. It is a seam. (It is also why
`pressKey(RollUp, Hold, KeySource::Wire)` returns `NotSupported` rather than quietly
transmitting the click form — §7b.6.)

The seam also inherits §3b in full, which is the second reason it is a seam and not a
convenience. `pressKey()` and `nav()` run the *same* `routeKey()` that the wire
decoder runs, so a menu move driven from a web console produces the same coalescing
decisions, the same `RenderSlot` traffic and the same "highlight only vs full redraw"
choice as a wheel detent. One difference, worth knowing and worth not designing around:
a key from the wire is delivered inside `poll()` and so carries L1 (one poll period);
`pressKey()` is called by the application and delivers synchronously on the caller's
stack, so its L1 is zero. Both then feed the same transmit queue, so **L2 is identical**
— the reaction is not faster just because the input was local. Any test that asserts a
frame sequence is therefore valid for both origins, which is the property that makes
the whole library host-testable without a panel.

### 8.4 `NavCommand` → `(Key, KeyEdge)`

`nav()` is a pure mapping followed by `pressKey(k, e, src)`. It performs no state
inspection of its own; the menu's state decides the outcome. The "raw wire code" column
is what the panel would have transmitted, and is therefore also exactly what
`nav(c, KeySource::Wire)` puts on the bus.

| `NavCommand` | `Key` | `KeyEdge` | Raw wire code the panel would have sent |
| --- | --- | --- | --- |
| `Open` | `Key::Load` | `Hold` | `0x1C1 : 03 89 00 C0 …` (`0x00 \| 0xC0`) |
| `Back` | `Key::Load` | `Hold` | identical to `Open` |
| `Select` | `Key::Load` | `Click` | `0x1C1 : 03 89 00 00 …` |
| `Next` | `Key::RollDown` | `Click` | `0x1C1 : 03 89 01 41 …` |
| `Prev` | `Key::RollUp` | `Click` | `0x1C1 : 03 89 01 01 …` |
| `Increase` | `Key::RollDown` | `Hold` | **unreachable from the panel** (§8.3) |
| `Decrease` | `Key::RollUp` | `Hold` | **unreachable from the panel** |

`Open` and `Back` map to the same key and the same edge **on purpose**: the panel has
one hold-Load and it toggles. `nav()` cannot be safer than the hardware. The two names
exist so calling code reads as intent; if you need a guaranteed open, check
`getMenu().isOpen()` first.

`Open` is the one row that also depends on configuration: what actually opens the menu on
a key arriving from the wire is the **menu hotkey**, `Key::Load` + `Hold` by default and
replaceable or removable through `setMenuHotkey()` / `clearMenuHotkey()` (§7b.7c).
`nav(Open)` opens the menu regardless of the hotkey setting — it is an intent, not a
gesture, and clearing the hotkey exists precisely so that `nav(Open)` becomes the only
way in.

`nav()` returns:

* `Result::NotSupported` if `AFFA_ENABLE_MENU` is 0 or the panel has no menu; or if the
  source includes `Wire` and the command is `Increase`/`Decrease`, which have no wire
  representation (§8.3, §7b.6); or if the source includes `Wire` on a panel with no key
  transmit id;
* `Result::LinkDown` / `Result::SendFailed` for the `Wire` half only;
* `Result::Ok` otherwise — meaning *the intent was delivered*, not that anything
  changed on screen. A `Next` at the last item is `Ok` and emits nothing.

### 8.5 `NavCommand` × menu state → behaviour and frames

`W` = the visible two-row window. "full redraw" = one 96-byte `showMenu` payload followed
by one `highlightItem` frame. "highlight only" = the single
`0x151 : 07 29 01 7E|7F 80 00 00 00` frame.

**The frame count of a full redraw depends on the ACK model, and quoting a bare number
here is the trap this document must not set.** Frame 0 carries 8 payload bytes and each
continuation carries 7:

| Peer | Terminates at | Frames | Last PCI |
| --- | --- | --- | --- |
| Real panel (and `AckMode::Declared`) | the declared FF_DL, `payload[1] = 0x5A` = 90 content bytes, reached at 6 + 12×7 | **13** | `0x2C` |
| Self-ACK emulator (`LoopbackLink::setAutoAck`, `AckMode::Done`) | the builder's 96 bytes, 8 + 13×7 = 99 ≥ 96 | **14** | `0x2D` |

Either way a full redraw is ~13× the bus time and ~13× the ACK round-trips of a
highlight, which is why they occupy different `RenderSlot`s and why the menu decides
between them (§8.2). Any golden vector for `showMenu` must be parameterised by ACK model
(`_HW` / `_EMU`) rather than carrying a bare 14 — see `test_carminat_wire`, which asserts
both against the same vector array sliced differently.

| Command | Menu **closed** | Menu **open**, not editing | Menu **open**, editing |
| --- | --- | --- | --- |
| `Open` | Opens. `selectedIndex` and `selectedRow` keep their previous values. **full redraw**. | Same key as `Back` → **closes** (see `Back`). | Same key as `Back` → **closes**. |
| `Back` | Not consumed by the menu; falls through to the application `KeyCb`. **No frames.** | Closes. Fires `Menu::CloseCb` — on Carminat the default is `setText("RENAULT", 0)`, so **one setText transfer**. | Closes, and **clears `editing` and `editingField`** (see the note below). Same frames as above. |
| `Select` | Not consumed; falls through to `KeyCb`. **No frames.** | If `onActivate` is set: calls it, **no frames from the menu itself**. Else if `editable`: enters edit mode → **full redraw** (the row now renders as `*Label: <value>`). Else nothing. | Advances to the next field → **full redraw**. On the last field, leaves edit mode → **full redraw**. |
| `Next` | Not consumed; falls through to `KeyCb`. | At the last item: nothing. Else `selectedIndex++`; if the selection moves from row 0 to row 1 **inside** `W`: **highlight only**. If the window must scroll: **full redraw**. | `value += step` on the current field, clamped to `[min,max]`. Unchanged (already at max, or `readOnly`): **no frames**. Changed: fires `MenuItem::onChange(item, fieldIndex, ctx)` — the one hook, both roles; there is no `Field::onChange` — then **full redraw**. |
| `Prev` | as `Next` | Mirror of `Next` (`selectedIndex--`, row 1 → row 0). At the first item: nothing. | `value -= step`, otherwise identical to `Next`. |
| `Increase` | as `Next` | **Identical to `Next`** — outside edit mode the hold edge is ignored, exactly as the extracted code did. | `value += step * stepMultiplier`, clamped. Otherwise identical to `Next`. |
| `Decrease` | as `Prev` | **Identical to `Prev`**. | `value -= step * stepMultiplier`. |

For a `List` field, "value" is `listIndex`, clamped to `[0, listCount-1]`, no wrap —
matching the extracted behaviour.

> **A defect fixed while porting.** The legacy `Menu::handleKey` closed the menu on
> hold-Load without clearing `editing`. Reopening the menu therefore resumed in edit
> mode on whatever field was live when you left, with no visual difference from a
> fresh open. `MenuModel::close()` now clears `editing` and `editingField`. This is a
> behaviour change and it is intentional.

Keys the menu does not consume (`SrcNext`, `SrcPrev`, `VolUp`, `VolDown`, `Pause`, and
everything while the menu is closed except hold-Load) fall through to the application's
`KeyCb`. `MenuController` routes an active `IPage` first, then the menu, then the
fall-through — unchanged from the extracted code. Since the menu became display-agnostic
the `(Key, KeyEdge)` → intent map is `MenuController::routeKey`'s rather than the menu's,
and that fall-through is its `default:` branch; `MenuModel` has no `Key` vocabulary at all.

> **`nav(NavCommand::Open)` is an intent, not a keystroke, and it does not travel the key
> path.** `AffaDisplayBase::nav()` calls `openMenu()` directly rather than `routeKey()`.
> With an `IPage` pushed, `CarminatDisplay::openMenu()` refuses (page-first routing is
> preserved from the extracted code), so `nav(Open)` returns `Result::NotSupported` and
> **no key is delivered to the page**. The hold-Load **gesture**, by contrast, goes through
> `routeKey()` and therefore does reach the page. Both behaviours are deliberate; the row
> above describes the gesture. The same refusal applies to an empty menu, which never
> opens — an open empty menu would render nothing and swallow the wheel and `Load`.

> **The closing key is CONSUMED.** `MenuController::routeKey` returns true for every key the
> menu acts on while open — `MenuModel::back()` returns true — *including* the hold-`Load`
> that closes it, as the `Open`/`Back` rows above specify. The extracted
> `MenuController::routeKey` returned `_menu.isActive()`
> **after** handling, so in the old code the closing keystroke also fell through to the
> application's `KeyCb`. If an application relied on seeing that key, this is the
> behaviour change to look at.

### 8.6 Scroll indicator, specified

`widget::MenuModel::scrollMask()` derives it from the WINDOW, not from the selection —
`top` is `selectedIndex - selectedRow`, and `rows` is `geometry().rows`:

```
count <= rows            -> 0x00  (no arrows — the whole list fits)
top == 0                 -> 0x0B  (bottom arrow only)
top + rows >= count      -> 0x07  (top arrow only)
otherwise                -> 0x0C  (both)
```

At `rows = 2` — the Carminat menu screen, and the only geometry the extracted code had —
that is the same function as the selection-worded rule it replaced (`selectedIndex == 0 ||
(selectedIndex == 1 && selectedRow == 1)` *is* `top == 0`), so no Carminat behaviour moved.
The window wording is the one to reason with, because it is the only one that stays true at
`rows = 3` or `6`. Note the consequence: the mask depends on where the window is, so item 1
of 3 shows `0x0B` walking down and `0x07` walking back up.

The `count <= rows` case is new: the extracted code indexed `items[topIndex+1]` without a
bounds check and read past the end of a one-item menu.

### 8.7 `widget::MenuModel& getMenu()` and the minimal item-building API

The exact declaration, in `carminat/CarminatDisplay.h`, is:

```cpp
widget::MenuModel& getMenu();                            // non-const only
CarminatMenuRenderer&       menuRenderer();              // the adapter —
const CarminatMenuRenderer& menuRenderer() const;        //   ask it for lastResult()
```

**`getMenu()` returns `affa::widget::MenuModel&`.** It used to return a `Menu&` that was
`carminat/Menu/Menu.h`; that file has been deleted and there is now exactly one menu state
machine in the library. `affa::Menu` still names the returned type — it is a **type alias**
for `affa::widget::MenuModel` declared in `CarminatDisplay.h`, alongside `affa::MenuItem`,
`affa::Field`, `affa::FieldType`, `affa::integerField`, `affa::readOnlyField` and
`affa::listField` — so `affa::Menu& m = display.getMenu();` still compiles and still means
what it meant. Aliases, not a compatibility shim: there is nothing behind them but the one
implementation. Two differences a caller can observe, both spelled out in §2.12:

* `render()` returns **`void`**, not `Result`. Ask `menuRenderer().lastResult()` for the
  panel's verdict — only the layer holding the `IPanel` can answer it.
* a row truncates at the injected `rowChars` (26 on this panel) rather than at
  `AFFA_MENU_ROW_MAX - 1`.

And the boundary that governs all of it: **`showMenu()` and `highlightItem()` are the
protocol-level primitives, and they are unconditional.** They are declared on
`CarminatDisplay` outside every menu gate and work with `AFFA_ENABLE_MENU=0`, which is the
default. `getMenu()`, `MenuModel`, `MenuController`, `IPage` and `nav()` are the **optional**
widget built on top of them — one opinion about menu behaviour, which an application with a
different remote is expected to replace. See `docs/MENU-WIDGET.md`.

> **`getMenu()` is declared on `CarminatDisplay`, NOT on `AffaDisplayBase` or `IDisplay`.**
> The menu is a Carminat capability — `UpdateList` has no menu at all and answers
> `supports(Feature::Menu)` with false — so the accessor lives on the panel that has one
> and the base declares no `MenuModel*` seam. Take a `CarminatDisplay&` (or keep a typed
> pointer beside your base-typed one) wherever you build or inspect the menu; a
> base-typed handle gives you `nav()`, which is the panel-agnostic half of the same
> capability. This section and §7b.7c are written against a `CarminatDisplay&` for that
> reason.

The library hands out an **empty** menu with a header. The application fills it. This
is the complete example of a three-field item — nothing else is needed and no library
internal is touched:

```cpp
static const char* const kBtModes[] = { "OFF", "AUTO", "ON" };

static void onTimeChanged(const affa::MenuItem& it, uint8_t field, void* ctx) {
  auto* app = static_cast<App*>(ctx);
  app->setClock(it.fields[0].value, it.fields[1].value);   // hours, minutes
  app->persist();                                          // NVS lives HERE, not in the library
}

void App::buildMenu(affa::CarminatDisplay& display) {   // NOT AffaDisplayBase& — see above
  if (!display.supports(affa::Feature::Menu)) return;
  affa::Menu& m = display.getMenu();

  affa::MenuItem clock{};
  clock.label      = "Clock";
  clock.fields[0]  = affa::integerField(hh, 0, 23, /*step*/1, /*stepMultiplier*/6);
  clock.fields[1]  = affa::integerField(mm, 0, 59, /*step*/1, /*stepMultiplier*/10);
  clock.fields[2]  = affa::listField(kBtModes, 3, /*index*/1);
  clock.fieldCount = 3;
  clock.separator  = ':';
  clock.onChange   = &onTimeChanged;
  clock.ctx        = this;
  m.addItem(clock);

  affa::MenuItem volts{};
  volts.label      = "Battery";
  volts.fields[0]  = affa::readOnlyField(0, "V");
  volts.fieldCount = 1;
  volts.editable   = false;
  m.addItem(volts);
}

// Later, from anywhere that owns the poll() task:
m.setFieldValue(/*item*/1, /*field*/0, millivolts / 100);   // redraws only if visible
```

Rendered rows follow the extracted format exactly: `Label` when there are no fields,
`Label: v1<sep>v2…` when there are, an `*` prefix on the item being edited, and
`<value>` angle brackets around the field under edit. Row text is transliterated by
`affa::toAscii` on its way to the wire, always, with no way to opt a string out.

### 8.8 Text handling, restated because it bites

```cpp
namespace affa {
// Returns bytes written, not counting the NUL. Always NUL-terminates when
// outSize > 0. Truncation never splits a multi-byte UTF-8 sequence. Unknown
// non-ASCII codepoints become '?'. Idempotent on 7-bit ASCII input.
size_t toAscii(const char* in, char* out, size_t outSize);

// toAscii plus the title cleanup (strips the Apple Music video annotations).
size_t normalizeTitle(const char* in, char* out, size_t outSize);
}
```

The panel charset cannot render UTF-8. Every string that reaches the wire passes
through `toAscii`, inside the library, at the single choke point in each frame builder.
Losing it looks like garbage on the screen, not a compile error — which is why it is
mandatory rather than a courtesy, and why `AFFA_ENABLE_TRANSLITERATION=0` carries the
warning it does. The mapping table (Polish + Cyrillic + Ukrainian) is host-testable and
`test/test_core` tests it, including the truncation-mid-sequence case.
