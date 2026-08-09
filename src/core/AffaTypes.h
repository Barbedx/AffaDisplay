// Every type that crosses a seam in AffaDisplay. Declared once, here, so that no header
// below this one has to invent a frame, a result or an event of its own.
#pragma once
#include "../AffaConfig.h"   // AFFA_TX_COALESCE, for the TxOptions default
#include <cstdint>
#include <cstddef>

namespace affa {

// Portable CAN frame. Deliberately not the driver's CAN_FRAME: this type crosses every
// seam in the library, including the ones compiled for the host.
struct Frame {
  uint32_t id   = 0;
  uint8_t  len  = 0;
  uint8_t  data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  bool     ext  = false;      // extended (29-bit) identifier; AFFA never uses one

  // Set by the library on every frame it hands to ICanLink::send(), and NEVER set by an
  // application. A real CAN controller does not receive its own transmissions, but
  // LoopbackLink does — so without this flag pressKey(..., Both) would fire once on
  // hardware and twice on the host, and every host test would be lying about the target.
  // Self-sent frames are dropped before the auto-ACK, before the ACK matcher AND before
  // the key decoder — all three. Missing the ACK matcher makes a loopback transfer
  // complete after one frame with a bogus success; missing the auto-ACK makes the
  // library acknowledge itself into a storm. Both have been observed on real hardware
  // (the 0x7AF incident, docs/WIRE.md).
  bool     fromSelf = false;
};

// Wire codes. DO NOT RENUMBER — these values are transmitted and received verbatim in
// bytes 2..3 of the key frame.
enum class Key : uint16_t {
  Load     = 0x0000,   // the button at the bottom of the stalk
  SrcNext  = 0x0001,
  SrcPrev  = 0x0002,
  VolUp    = 0x0003,
  VolDown  = 0x0004,
  Pause    = 0x0005,
  RollUp   = 0x0101,   // wheel, one detent up
  RollDown = 0x0141,   // wheel, one detent down
};

// Click vs hold. The panel encodes hold as bits 0x80|0x40 set in the LOW byte of the key
// code — but only for the non-wheel keys, because 0x40 is simultaneously RollDown's
// direction bit and half the hold mask.
enum class KeyEdge : uint8_t { Click = 0, Hold = 1 };

// Names, for anything that shows a key to a person: a serial line, a web console, a test
// harness. Every consumer that displays keys was writing this switch itself, and a switch
// that lives beside the enum cannot fall out of step with it.
inline const char* keyName(Key k) {
  switch (k) {
    case Key::Load:     return "Load";
    case Key::SrcNext:  return "SrcNext";
    case Key::SrcPrev:  return "SrcPrev";
    case Key::VolUp:    return "VolUp";
    case Key::VolDown:  return "VolDown";
    case Key::Pause:    return "Pause";
    case Key::RollUp:   return "RollUp";
    case Key::RollDown: return "RollDown";
  }
  return "?";
}
inline const char* edgeName(KeyEdge e) { return e == KeyEdge::Hold ? "hold" : "click"; }

// Where an emulated key press is to have its effect. A press on the real system is
// transmitted by the PANEL and received by us, so "emulate a key" legitimately means two
// things at once, and an application wants each of them separately at different times.
// One function with a source, not two lookalike functions — see docs/API.md §7.4.
enum class KeySource : uint8_t {
  Local = 1,   // as if a key arrived: drive our menu + fire the Key event. Nothing goes
               // on the bus. THE DEFAULT, because in the radio role that is what a key
               // press IS from our side.
  Wire  = 2,   // impersonate the panel: encode and transmit the key frame. Only
               // meaningful when a REAL radio is on the bus and you are driving it.
  Both  = 3,   // both at once. Rarely what you want.
};
constexpr bool hasSource(KeySource v, KeySource f) noexcept {
  return (static_cast<uint8_t>(v) & static_cast<uint8_t>(f)) != 0;
}

// Values 0..5 keep the numeric identities of the legacy AffaCommon::AffaError so a
// migrating application that logged the raw number sees the same number.
enum class Result : uint8_t {
  Ok           = 0,
  NoSync       = 1,   // link not established (was AffaError::NoSync)
  UnknownFunc  = 2,   // funcId not in this panel's function table
  SendFailed   = 3,   // panel answered something that was neither DONE nor PARTIAL
  Timeout      = 4,   // no ACK within AFFA_ACK_TIMEOUT_MS
  TooLong      = 5,   // payload exceeds AFFA_MAX_PAYLOAD (was StrTooLong)
  QueueFull    = 6,
  NotSupported = 7,   // this panel does not implement this Feature
  BadArgument  = 8,   // null pointer, len == 0, index out of range
  LinkDown     = 9,   // ICanLink::isLive() was false
  Cancelled    = 10,  // job discarded because sync was lost or begin() was re-run
  // 11 was Busy: the "would have re-entered poll()" refusal, which only sendBlocking()
  // could return. Both are gone. The number is NOT reused — a migrating application that
  // logged the raw value must not find 11 meaning something else.
  Aborted      = 12,  // discarded by the APPLICATION before any byte reached the wire:
                      // abortPending(), abortAll(), or superseded by a newer render of
                      // the same RenderSlot. onComplete only — never returned by an
                      // enqueue call, because a call cannot abort itself.
};

// Bit flags, exactly the legacy bit assignment. FuncsReg is dropped whenever Failed is
// raised — registration does not survive a resync, because the panel forgets us too.
enum class SyncState : uint8_t {
  None      = 0x00,
  Failed    = 0x01,
  PeerAlive = 0x02,
  Start     = 0x04,
  FuncsReg  = 0x08,
};

constexpr SyncState operator|(SyncState a, SyncState b) noexcept {
  return static_cast<SyncState>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr SyncState operator&(SyncState a, SyncState b) noexcept {
  return static_cast<SyncState>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
constexpr SyncState operator~(SyncState a) noexcept {
  return static_cast<SyncState>(static_cast<uint8_t>(~static_cast<uint8_t>(a)));
}
inline SyncState& operator|=(SyncState& a, SyncState b) noexcept { a = a | b; return a; }
inline SyncState& operator&=(SyncState& a, SyncState b) noexcept { a = a & b; return a; }
constexpr bool hasFlag(SyncState v, SyncState f) noexcept {
  return (static_cast<uint8_t>(v) & static_cast<uint8_t>(f)) == static_cast<uint8_t>(f);
}

// WHERE THE OPENING HAS GOT TO, as one ordered value instead of nine booleans.
//
// SyncState above is a set of FLAGS, which is the right shape for what an application asks
// ("are we registered?") and the wrong shape for what a person debugging asks ("why is
// nothing rendering?"). The answer to the second question used to be spread across
// `_panelObserved`, `_syncRequestObserved`, `_authRequestObserved`, `_authHelloPending`,
// `_helloPending`, `_unauthControlPending/Issued/Spent` and `_peerChannelSeen`, and it took
// four bugs and most of a week to learn to read them in the right order. This is that order.
//
// IT IS MONOTONIC WITHIN A SESSION and only ever falls back to Silent or Announced — a
// session is lost, never partly lost. Print it on any diagnostic surface: the last four
// protocol bugs were all found by watching a phase that would not advance.
//
// `Ready` MEANS THE GLASS IS ON, not merely that we are registered, and that is deliberate.
// A panel that has not been powered ACKs a screen it never lights: every counter reports
// success and the display stays black, which is a failure mode with no symptom. The library
// sends the family's power-on command itself on the way through `Powering` — see
// AffaDisplayBase::setAutoPower() for the opt-out and for what `Ready` means without it.
//
// AwaitPeerChannel is the one nobody expects and it is measured 4/4: the DISPLAY registers
// its own channel (`1C1 70`, answered `5C1 74`) before the radio registers its functions.
// A bench that stalls here — "waiting for the display's 1C1" — is a panel that never got
// our announce. See docs/WIRE.md.
enum class Phase : uint8_t {
  Silent,            // nothing heard from the panel yet; announcing on a slow timer
  Announced,         // our `BA` is on the wire; awaiting the panel's NEXT request
  HelloPending,      // that request arrived and the announce burst is mid-flight
  AwaitPeerChannel,  // burst done; waiting for the panel to open ITS channel
  Registering,       // our function probes are out, awaiting their ACKs
  Settling,          // the measured quiet interval between registration and any payload
  Powering,          // the family's power-on command is out, awaiting its ACK
  Ready,             // the glass is on: rendering permitted
};

// WHY THE LAST SESSION ENDED. There are exactly four ways to leave FUNCSREG and they call
// for completely different investigations, but the counters look identical from outside: a
// panel that deauthorized us, a panel that went quiet, a controller that restarted under us,
// and our own begin(). The 1 h 36 m soak of 2026-08-04 lost fourteen sessions with every
// driver counter at zero, which rules out the third and says nothing about the first two —
// and nothing recorded which it was, so the question is still open. This records it.
enum class LossReason : uint8_t {
  None,           // no session has been lost since begin()
  PanelVoided,    // a complete `61 11 xx` arrived while we held registrations
  PeerTimeout,    // no ping within AFFA_PEER_TIMEOUT_MS
  LinkRestarted,  // the CAN controller was recovered under us; the panel's view is stale
};

inline const char* lossReasonName(LossReason r) {
  switch (r) {
    case LossReason::None:          return "none";
    case LossReason::PanelVoided:   return "panel voided us (61 11 while registered)";
    case LossReason::PeerTimeout:   return "peer timeout (no 69)";
    case LossReason::LinkRestarted: return "CAN controller restarted";
  }
  return "?";
}

// THE ORDER IS LOAD-BEARING, so comparing is spelled out rather than left to a cast at each
// call site. "Has the opening got at least as far as X" is the question every transmit gate
// in the library asks, and it used to be asked as a pair of booleans that could disagree.
constexpr bool atLeast(Phase a, Phase b) noexcept {
  return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b);
}

// For logs and status pages. Never parse it; it is prose, not protocol.
inline const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Silent:           return "Silent";
    case Phase::Announced:        return "Announced";
    case Phase::HelloPending:     return "HelloPending";
    case Phase::AwaitPeerChannel: return "AwaitPeerChannel";
    case Phase::Registering:      return "Registering";
    case Phase::Settling:         return "Settling";
    case Phase::Powering:         return "Powering";
    case Phase::Ready:            return "Ready";
  }
  return "?";
}

// Optional capabilities. supports() answers before you call; an unsupported call returns
// Result::NotSupported. The legacy IDisplay gave these silently no-op bodies returning
// NoError, so calling one on a panel that could not do it looked exactly like success.
enum class Feature : uint8_t {
  Text,         // setText
  Time,         // setTime
  Power,        // setPower
  Menu,         // getMenu / showMenu / highlightItem / nav
  Popup,        // showPopupText / hidePopup
  Fullscreen,   // showFullscreenText
  ConfirmBox,   // showConfirmBox
  InfoPopup,    // showInfoPopup
  NavBitmap,    // showNavBitmap / navTick — the Carminat 48x48 pane on 0x1F1
  KeyTx,        // this panel family has a key-transmit id, so pressKey(..., Wire) can
                // put a frame on the bus
};

// Navigation intent. Mapped to (Key, KeyEdge) by AffaDisplayBase::nav().
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

// WHAT EVERY RENDER CALL RETURNS: the handle and the reason, together, in four bytes.
//
// It replaces three spellings of the same answer. Before 2.0 a render returned a bare
// `Result` and the caller had to fetch the handle separately from `lastEnqueued()`, whose
// contract was "READ IT IMMEDIATELY: the next enqueue overwrites it, including a render the
// menu makes for you" — a documented footgun that existed only because the ticket was not
// returned. Meanwhile the owned task issued a THIRD handle (`TxRequest`) and kept a
// translation table, because a posted render could not return a ticket that had not been
// minted yet.
//
// 2.0 mints the ticket at SUBMIT time, from an atomic counter, on whichever task called.
// So the handle is always available to return, the same value reaches `onComplete()` in
// both threading modes, and `lastEnqueued()` and `TxRequest` are both gone.
//
// `ticket` is the authority. A non-zero ticket means ACCEPTED, and `result` is Ok; zero
// means refused, and `result` says why. It is never "accepted but untracked".
//
// THE VERDICT IS ACCEPTANCE, NEVER DELIVERY — "was it queued?", not "did the panel show
// it?". Delivery arrives later through onComplete(), keyed by this same ticket.
//
// [[nodiscard]] IS ON THE TYPE, not on each of the twenty-odd declarations that return it.
// That is deliberate: a render whose verdict is dropped is a screen that silently never
// appears, which is the legacy failure this library was extracted to stop repeating, and
// putting the attribute here means a render added in 2027 inherits the guarantee without
// its author having to know about it. To ignore one on purpose, say so: `(void)…;`.
struct [[nodiscard]] Submitted {
  TxTicket ticket = kNoTicket;
  Result   result = Result::Ok;

  // `if (!display.setText("HI")) …` — explicit, so a Submitted cannot silently decay to a
  // bool in arithmetic or in a comparison against a Result.
  explicit operator bool() const noexcept { return ticket != kNoTicket; }
  bool ok() const noexcept { return ticket != kNoTicket; }

  // The refusal, spelled once. Every render call in the library ends in one of these two.
  static Submitted refused(Result r) noexcept { return Submitted{kNoTicket, r}; }
  static Submitted accepted(TxTicket t) noexcept { return Submitted{t, Result::Ok}; }
};

// What a queued message is FOR, so a newer one can supersede an older one that has not
// started yet. Latest value wins, per slot. None opts out of coalescing entirely: a None
// message never replaces anything and is never replaced.
enum class RenderSlot : uint8_t {
  Text,        // setText
  Clock,       // setTime
  Menu,        // showMenu (the 96-byte screen)
  Highlight,   // highlightItem — deliberately NOT Menu: a highlight must not replace a
               // pending full redraw, and vice versa
  Popup,       // showPopupText / hidePopup — never coalesces against Text: the popup is
               // a non-destructive overlay and the screen underneath keeps redrawing
  Fullscreen,  // showFullscreenText
  ConfirmBox,  // showConfirmBox
  InfoPopup,   // showInfoPopup
  Control,     // setPower and other non-rendering control payloads
  None,        // raw enqueue(); never coalesced
};

// Urgent jumps the queue ahead of Normal work. It never splits a message that is already
// on the wire, and it never overtakes a pending function-registration job — the panel
// rejects payloads sent before registration completes, and the resulting SendFailed looks
// exactly like a wire-format bug.
enum class Priority : uint8_t { Normal = 0, Urgent = 1 };

// Named, so the three trailing arguments cannot be swapped at the call site.
struct TxOptions {
  RenderSlot slot     = RenderSlot::None;
  Priority   priority = Priority::Normal;
  bool       coalesce = (AFFA_TX_COALESCE != 0);  // per-message opt-out
  // A successfully ACKed payload with this flag becomes desired session state. If the
  // panel later loses registration, the base restores it internally before held text/time.
  // It is intentionally opt-in: only a true durable control such as display power should
  // survive a session reset; ordinary render frames must not reappear unexpectedly.
  bool       reassertAfterSession = false;
};

// Free-running counters. Each field is a plain uint32_t updated by a single writer, so a
// torn read is impossible on a 32-bit target and harmless on the host.
struct Stats {
  uint32_t rxFrames     = 0;  // frames pushed into the RX ring by the driver callback
  uint32_t txFrames     = 0;  // frames accepted by the driver
  uint32_t txDropped    = 0;  // send() refused: TX gate closed, or driver said no
  uint32_t ringOverflow = 0;  // RX ring was full — frames LOST. Non-zero means poll() is
                              // not called often enough, or the ring is too small.
  uint32_t txErr        = 0;  // controller TX error counter (driver status, read-only)
  uint32_t rxErr        = 0;  // controller RX error counter
  uint32_t txFailed     = 0;  // controller TX failure count (arbitration/ack loss)
};

// ---------------------------------------------------------------------------
// The observation seam — ONE LAYER, as of 2.0.
// ---------------------------------------------------------------------------
// There were three. Layer 1 was a filtered subscription table (`subscribe()` /
// `FrameMatch` / `SubHandle`) and Layer 2 was a decoded event sink (`onEvent()` /
// `Event` / `EventKind`). Both are gone, and the evidence is why:
//
//   * across nineteen shipped examples, neither was called ONCE. Layer 0 was called
//     by seven of them.
//   * the one external consumer's written feedback names `onFrame()` Layer 0 as
//     "exactly right" and asks for a ring built ON it. It does not mention Layers 1
//     or 2 at all — what it wants is less API, not more.
//   * Layer 1 was not free. It walked an AFFA_MAX_SUBSCRIPTIONS table TWICE for every
//     frame in BOTH directions, plus ~256 B of static RAM, to dispatch to nobody.
//   * everything Layer 1 did is three lines inside a Layer 0 tap: compare the id, look
//     at the bytes. Everything Layer 2 reported is already in KeyCb, CompleteCb, SyncCb
//     and Status.
//
// The rule that survives them is the one that mattered: a callback SHIPS WITH ITS
// EMITTER. Layer 2 once declared `RadioText` and `ScreenChanged` with nothing
// constructing either, so the API advertised two events that could not arrive.

// Which way a frame went. A tap that cannot tell inbound from outbound is useless to a
// sniffer — 0x151 carries both directions.
//
// A frame arriving through ICanLink::recv() with Frame::fromSelf set is our own
// transmission coming back off a link that echoes. It is presented to the tap as
// Direction::Tx, never as Rx, so a tap that filters on Rx sees what the other node
// actually sent, on every link, echoing or not.
enum class Direction : uint8_t { Rx = 1, Tx = 2, Both = 3 };

// EVERY frame, in and out, unfiltered, in wire order. One tap; a second call replaces it.
// On the path of every frame — keep it to a ring push, and never render from it.
using FrameTap = void (*)(const Frame& f, Direction d, void* ctx);

// WHICH CALLBACK, when one of them blocks.
//
// A callback that blocks blocks the library — it runs inside poll(), on the poll owner's
// task, by design, because that is what bounds key latency by the poll period alone. It
// cannot be prevented. What it CAN be is attributable.
//
// Until 2.0 the only evidence was `Status::pollLateMaxUs`: "an iteration took 340 ms". That
// says something is wrong and nothing about where. The three incidents in
// docs/NOTES.md §3 all presented as "the panel is frozen" with every error
// counter at zero, and the search each time was the whole application. This is the missing
// word in that sentence.
enum class CbKind : uint8_t {
  None = 0,
  Key,        // onKey       — fires inside pumpRx, before any transmit pumping
  Sync,       // onSync      — a state-word change
  Complete,   // onComplete  — a render's delivery verdict
  FrameTap,   // onFrame     — EVERY frame, both directions; the easiest one to make slow
};

// A Result as text. Next to the enum because every consumer that logs one would otherwise
// write this switch, and a switch written twice drifts once: 01_bringup carried a copy that
// predated Result::Aborted.
inline const char* resultName(Result r) {
  switch (r) {
    case Result::Ok:           return "Ok";
    case Result::NoSync:       return "NoSync";
    case Result::UnknownFunc:  return "UnknownFunc";
    case Result::SendFailed:   return "SendFailed";
    case Result::Timeout:      return "Timeout";
    case Result::TooLong:      return "TooLong";
    case Result::QueueFull:    return "QueueFull";
    case Result::NotSupported: return "NotSupported";
    case Result::BadArgument:  return "BadArgument";
    case Result::LinkDown:     return "LinkDown";
    case Result::Cancelled:    return "Cancelled";
    case Result::Aborted:      return "Aborted";
  }
  return "?";
}

inline const char* cbName(CbKind k) {
  switch (k) {
    case CbKind::None:     return "none";
    case CbKind::Key:      return "KeyCb";
    case CbKind::Sync:     return "SyncCb";
    case CbKind::Complete: return "CompleteCb";
    case CbKind::FrameTap: return "FrameTap";
  }
  return "?";
}

} // namespace affa
