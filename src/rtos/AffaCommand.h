// One posted render, as data — the portable half of the owned-task mode.
//
// NOTHING IN THIS FILE TOUCHES FreeRTOS. That is deliberate and it is what makes the
// interesting half of src/rtos/ host-testable: a Command is a POD, applyCommand() is the
// dispatch table, and RequestTable is the handle bookkeeping. The only part that needs an
// RTOS is the ring that carries a Command from one task to another, and that lives in
// AffaTask.{h,cpp} behind the AFFA_ENABLE_TASK gate.
//
// WHY A COMMAND AND NOT A MUTEX. Callbacks fire from inside poll() and are explicitly
// permitted to call back into the library (docs/API.md §4.3, "state first, callbacks
// second"). A non-recursive mutex deadlocks on the first such call; a recursive one lets an
// application hold the library locked while it blocks on something else. Copying the call
// into a queue that the owning task drains keeps the single-caller invariant intact and
// needs no lock anywhere.
#pragma once

#include "../AffaConfig.h"
#include "../core/AffaDisplayBase.h"
#include <cstring>

namespace affa {
namespace rtos {

// The handle a posted render returns.
//
// IT IS NOT A TxTicket, AND THE DIFFERENT NAME IS THE POINT. A TxTicket is issued by
// AffaDisplayBase::enqueue(), which for a posted render has not run yet — the command is
// still in the queue. A TxRequest is issued by AffaTask at POST time, and AffaTask maps it
// to the real ticket when the command drains. An application in owned-task mode sees only
// TxRequests: AffaTask::onComplete reports them, so there is one handle space, not two.
//
// Non-zero on acceptance. kNoRequest means REFUSED — never "accepted but untracked".
using TxRequest = uint16_t;
inline constexpr TxRequest kNoRequest = 0;

enum class Op : uint8_t {
  None = 0,
  SetText,
  SetTime,
  SetPower,
  ShowMenu,
  HighlightItem,
  ShowPopupText,
  HidePopup,
  ShowFullscreenText,
  HideFullscreenText,
  ShowConfirmBox,
  ShowInfoPopup,
  HideInfoPopup,
  PressKey,
  AbortPending,
  AbortAll,
  Resync,          // re-runs begin() on the owned task
};

// Copied BY VALUE into the queue: no pointer into a caller's stack ever crosses a task
// boundary. That is why the strings are arrays and not const char*, and why
// AFFA_TASK_ARG_MAX is what a queued render truncates at.
struct Command {
  Op        op  = Op::None;
  TxRequest req = kNoRequest;      // the handle the caller was already given
  char      s0[AFFA_TASK_ARG_MAX] = {0};
  char      s1[AFFA_TASK_ARG_MAX] = {0};
  char      s2[AFFA_TASK_ARG_MAX] = {0};
  uint8_t   a = 0, b = 0, c = 0, d = 0;
};

// Bounded copy into a Command's string slot. Always NUL-terminated; a null source yields
// an empty string, exactly as the render calls treat one.
inline void setArg(char* dst, const char* src) {
  if (!src) { dst[0] = '\0'; return; }
  size_t i = 0;
  for (; i + 1 < AFFA_TASK_ARG_MAX && src[i]; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

// Run ONE command against the display, on the task that owns poll().
//
// Returns the Result the equivalent direct call returned — an ACCEPTANCE verdict, never a
// delivery verdict. `ticket` receives the display ticket the render was enqueued as, or
// kNoTicket for an op that enqueues nothing (PressKey, AbortPending, AbortAll, Resync) or
// for a refusal.
//
// lastEnqueued() IS READ IMMEDIATELY AFTER EACH CALL, which is the documented contract for
// it (docs/API.md §3b.4): the next enqueue overwrites it, including one a widget makes on
// the application's behalf. showInfoPopup enqueues THREE messages and the ticket reported
// is the last of them — the first failure is what the Result carries.
inline Result applyCommand(AffaDisplayBase& d, const Command& c, TxTicket& ticket) {
  ticket = kNoTicket;
  Result r = Result::BadArgument;

  switch (c.op) {
    case Op::SetText:            r = d.setText(c.s0, c.a); break;
    case Op::SetTime:            r = d.setTime(c.s0); break;
    case Op::SetPower:           r = d.setPower(c.a != 0); break;
    case Op::ShowMenu:           r = d.showMenu(c.s0, c.s1, c.s2, c.a); break;
    case Op::HighlightItem:      r = d.highlightItem(c.a); break;
    case Op::ShowPopupText:      r = d.showPopupText(c.s0, c.a, c.b, c.c); break;
    case Op::HidePopup:          r = d.hidePopup(); break;
    case Op::ShowFullscreenText: r = d.showFullscreenText(c.s0, c.s1, c.s2); break;
    case Op::HideFullscreenText: r = d.hideFullscreenText(); break;
    case Op::ShowConfirmBox:     r = d.showConfirmBox(c.s0, c.s1, c.s2); break;
    case Op::ShowInfoPopup:      r = d.showInfoPopup(c.s0, c.s1, c.s2); break;
    case Op::HideInfoPopup:      r = d.hideInfoPopup(); break;

    // Not a render: nothing is enqueued, so there is no ticket and never will be. The
    // Local half fires KeyCb synchronously, on this task, from inside this call.
    case Op::PressKey:
      r = d.pressKey(static_cast<Key>(static_cast<uint16_t>((c.a << 8) | c.b)),
                     c.c ? KeyEdge::Hold : KeyEdge::Click,
                     static_cast<KeySource>(c.d ? c.d : 1));
      return r;

    case Op::AbortPending: d.abortPending(); return Result::Ok;
    case Op::AbortAll:     d.abortAll();     return Result::Ok;
    case Op::Resync:       d.begin();        return Result::Ok;
    case Op::None:         return Result::BadArgument;
  }

  if (r == Result::Ok) ticket = d.lastEnqueued();
  return r;
}

// TxTicket -> TxRequest, for the completion callback.
//
// Sized by the TRANSMIT queue, not the command queue: a ticket exists only between
// enqueue() and completion, and AFFA_TX_QUEUE_DEPTH bounds how many can be outstanding at
// once. bind() therefore cannot fail in a correctly sized build, and when it does the
// completion is reported with kNoRequest rather than dropped.
template <uint8_t N>
class RequestTable {
 public:
  bool bind(TxTicket t, TxRequest r) {
    if (t == kNoTicket || r == kNoRequest) return false;
    // A ticket can be REISSUED to a different request only in the sense that the old one
    // completed first (coalescing completes the superseded ticket before returning the new
    // one), so a duplicate here is a stale entry and taking its slot is correct.
    for (uint8_t i = 0; i < N; ++i) {
      if (_e[i].ticket == t) { _e[i].req = r; return true; }
    }
    for (uint8_t i = 0; i < N; ++i) {
      if (_e[i].ticket == kNoTicket) { _e[i].ticket = t; _e[i].req = r; return true; }
    }
    return false;
  }

  // Look up and FREE. A ticket completes exactly once, so holding the entry afterwards
  // would leak the slot — which is how a fixed table becomes a slow failure.
  TxRequest take(TxTicket t) {
    for (uint8_t i = 0; i < N; ++i) {
      if (_e[i].ticket == t) {
        const TxRequest r = _e[i].req;
        _e[i].ticket = kNoTicket;
        _e[i].req    = kNoRequest;
        return r;
      }
    }
    return kNoRequest;
  }

  uint8_t used() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < N; ++i) if (_e[i].ticket != kNoTicket) ++n;
    return n;
  }
  static constexpr uint8_t capacity() { return N; }
  void clear() { for (uint8_t i = 0; i < N; ++i) _e[i] = Entry{}; }

 private:
  struct Entry {
    TxTicket  ticket = kNoTicket;
    TxRequest req    = kNoRequest;
  };
  Entry _e[N];
};

}  // namespace rtos
}  // namespace affa
