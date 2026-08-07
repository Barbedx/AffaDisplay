// AffaTx — the transmit queue, ISO-TP segmentation, flow control and retries.
//
// Split out of AffaDisplayBase.cpp by step 7 of docs/REFACTOR-PLAN.md; see AffaObserve.cpp
// for the four-way division. handleAckFrame() is here rather than in AffaSync.cpp because
// an `0x74` or a `30 01 00` is ISO-TP flow control addressed to the job at the head of this
// queue — it belongs to the transfer, not to the handshake.
//
// THE INVARIANT THIS FILE EXISTS TO KEEP: a job leaves the queue in exactly one place,
// finishJob(), and `started` is the single authority for "has this touched the bus". Every
// bug in here has come from inferring one of those from queue position or TxState instead.
#include "AffaBaseInternal.h"
#include "AffaDisplayBase.h"

#include <cstring>

namespace affa {
using namespace basedetail;

bool AffaDisplayBase::handleAckFrame(const Frame& f) {
  const uint16_t base =
      static_cast<uint16_t>(f.id & ~static_cast<uint32_t>(_profile.replyFlag));

  // An ACK arriving for a function that is not waiting is dropped silently. That is what
  // stops a stale late ACK from completing the wrong ticket.
  if (_tx != TxState::WaitAck || _qCount == 0 || _queue[0].funcId != base) return true;

  if (f.len >= 1 && f.data[0] == kAckDone) { creditAck(true); return true; }

  // ISO-TP flow control, parsed rather than constant-matched. This corpus only ever shows
  // `30 01 00` (CTS, BS=1, STmin=0), but an FC that differed in BS or STmin used to fall
  // through to SendFailed — and retryable() deliberately refuses to retry that, so the
  // transfer died on a frame that was in fact perfectly legal.
  if (f.len >= 3 && (f.data[0] & 0xF0) == kAckPartial0) {
    switch (f.data[0] & 0x0F) {
      case 0x0:                       // ContinueToSend
        creditAck(false);
        return true;
      case 0x1:                       // Wait — the panel is not ready; hold, do not fail
        _ackDeadlineMs = _clock.millis() + AFFA_ACK_TIMEOUT_MS;
        return true;
      default:                        // Overflow/abort, or a reserved FS
        AFFA_LOGW(kTag, "ISO-TP FC abort on 0x%03X: FS=0x%02X",
                  static_cast<unsigned>(f.id), static_cast<unsigned>(f.data[0] & 0x0F));
        finishJob(Result::SendFailed);
        return true;
    }
  }

  AFFA_LOGW(kTag, "ACK on 0x%03X rejected: 0x%02X", static_cast<unsigned>(f.id),
            static_cast<unsigned>(f.len ? f.data[0] : 0));
  finishJob(Result::SendFailed);
  return true;
}

// ---------------------------------------------------------------------------
// Transmit FSM
// ---------------------------------------------------------------------------

// THE ONE PLACE AN ARBITRARY TASK TOUCHES THIS OBJECT, so it is the one place that needs an
// atomic. Every other field is written by the poll owner alone.
//
// Post-increment semantics, so the value returned is the one that was there — the same
// numbering the pre-2.0 non-atomic version produced, which is what keeps the golden tests
// comparing against literal ticket numbers valid.
//
// The wrap is folded into the modulo rather than branched on, because a compare-and-branch
// between the fetch and the store is exactly the race this exists to avoid: two callers could
// both read 0xFFFF and both write 1. `(v - 1) % 0xFFFF + 1` maps the free-running counter
// onto 1..0xFFFF with no zero and no shared state between the two steps.
TxTicket AffaDisplayBase::nextTicket() {
  const uint32_t v = __atomic_fetch_add(&_nextTicket, 1u, __ATOMIC_RELAXED);
  return static_cast<TxTicket>((v - 1u) % 0xFFFFu + 1u);
}

// ---------------------------------------------------------------------------
// The task boundary
// ---------------------------------------------------------------------------

// True when this call is already on the task that owns poll(), OR when nobody has claimed
// ownership — the caller-owned mode, where the contract is "one task, by convention" and
// there is nothing to cross.
//
// AffaTask claims ownership with a placeholder BEFORE its task exists, so during that window
// this answers false for everybody and renders are posted rather than applied. That is the
// safe direction: they drain on the owned task's first poll.
bool AffaDisplayBase::onPollOwner() const {
  return _pollOwnerFn == nullptr || _pollOwnerFn() == _pollOwner;
}

#if AFFA_DISPATCH_DEPTH > 0
// Copy the finished render into the ring and hand the caller its ticket. NEVER BLOCKS.
//
// A refusal here means the ring was full, which is the one case where a foreign caller gets
// a synchronous "no" for a reason that is not its own mistake. It is reported as QueueFull —
// the same answer the transmit queue gives for the same condition — because from the
// caller's side they are the same event: there was no room for this render.
Submitted AffaDisplayBase::post(TxTicket t, uint16_t funcId, const uint8_t* data, uint16_t len,
                                const TxOptions& opt, const uint8_t* ext, uint16_t prefixLen) {
  DispatchItem it;
  it.ticket    = t;
  it.op        = DispatchOp::None;      // a payload
  it.funcId    = funcId;
  it.opt       = opt;
  it.ext       = ext;
  it.prefixLen = prefixLen;
  it.len       = len;
  if (data && prefixLen) std::memcpy(it.data, data, prefixLen);

  if (!_dispatch.push(it)) {
    // _lastResult is deliberately NOT written here: it is the poll owner's field, and a
    // foreign task scribbling on it would corrupt the one thing an owned-task callback
    // reads. The reason travels back in the return value, which is the whole point of
    // Submitted.
    AFFA_LOGW(kTag, "dispatch ring full (%u slots) — render %u refused",
              static_cast<unsigned>(AFFA_DISPATCH_DEPTH), static_cast<unsigned>(t));
    return Submitted::refused(Result::QueueFull);
  }
  return Submitted::accepted(t);
}

// Drain what other tasks posted, on the owning task, inside poll(). See poll() for why it
// runs where it does.
void AffaDisplayBase::pumpDispatch() {
  DispatchItem it;
  // BOUNDED BY THE RING, NOT BY "until empty". A producer posting faster than we drain must
  // not be able to hold this task inside the loop — the pumpTx() below it is what puts bytes
  // on the wire, and the pumpRx() above it is what delivers keys.
  for (uint16_t i = 0; i < AFFA_DISPATCH_DEPTH; ++i) {
    if (!_dispatch.pop(it)) return;

    if (it.isPayload()) {
      // The ticket was minted by the CALLING task and the caller already holds it. Minting
      // another here is the two-handle-space bug 2.0 deleted.
      const Submitted s = admit(it.ticket, it.funcId, it.prefixLen ? it.data : nullptr,
                                it.len, it.opt, it.ext, it.prefixLen);
      // A posted render that the queue refuses NOW reports through onComplete with its
      // reason, because the caller was told "accepted" and is owed exactly one verdict.
      if (!s) completeTicket(it.ticket, s.result);
      continue;
    }

    switch (it.op) {
      case DispatchOp::PressKey:
        (void)pressKey(static_cast<Key>(static_cast<uint16_t>((it.a << 8) | it.b)),
                       it.c ? KeyEdge::Hold : KeyEdge::Click,
                       static_cast<KeySource>(it.d ? it.d : 1));
        break;
      case DispatchOp::AbortPending: (void)abortPending(); break;
      case DispatchOp::AbortAll:     (void)abortAll();     break;
      case DispatchOp::Resync:       (void)begin();        break;
      case DispatchOp::None:         break;                // unreachable: isPayload()
    }
  }
}

uint32_t AffaDisplayBase::dispatchDropped() const { return _dispatch.dropped(); }
uint8_t  AffaDisplayBase::dispatchQueued()  const {
  return static_cast<uint8_t>(_dispatch.size());
}
#else
Submitted AffaDisplayBase::post(TxTicket, uint16_t, const uint8_t*, uint16_t,
                                const TxOptions&, const uint8_t*, uint16_t) {
  // AFFA_DISPATCH_DEPTH == 0: there is no ring, so there is no cross-task path. onPollOwner()
  // is the only caller and it answers true whenever no owner is registered, so this is
  // reachable only in a build that registered an owner AND compiled the ring out.
  return Submitted::refused(Result::NotSupported);
}
void     AffaDisplayBase::pumpDispatch() {}
uint32_t AffaDisplayBase::dispatchDropped() const { return 0; }
uint8_t  AffaDisplayBase::dispatchQueued()  const { return 0; }
#endif

bool AffaDisplayBase::registrationQueued() const {
  for (uint8_t i = 0; i < _qCount; ++i)
    if (_queue[i].kind == JobKind::Registration) return true;
  return false;
}

bool AffaDisplayBase::hasQueuedControl() const {
  for (uint8_t i = 0; i < _qCount; ++i) {
    const TxJob& j = _queue[i];
    if (j.kind == JobKind::Payload && j.slot == RenderSlot::Control) return true;
  }
  return false;
}

bool AffaDisplayBase::reassertQueued() const {
  for (uint8_t i = 0; i < _qCount; ++i)
    if (_queue[i].kind == JobKind::Reassert) return true;
  return false;
}

uint8_t AffaDisplayBase::dropUnstartedReasserts() {
  uint8_t n = 0;
  uint8_t i = 0;
  while (i < _qCount) {
    // A started restore already has a frame on the wire and must retain the same
    // frame-boundary rule as every other job.  An unstarted one is purely an old internal
    // intention, so a fresh application setPower() is authoritative and replaces it.
    if (_queue[i].kind == JobKind::Reassert && !_queue[i].started) {
      removeJob(i);
      ++n;
      continue;
    }
    ++i;
  }
  return n;
}

bool AffaDisplayBase::queueRegistrations() {
  if (_passive || hasFlag(_sync, SyncState::FuncsReg) || registrationQueued()) return true;
  // WE REGISTER AFTER THE DISPLAY DOES, ON EVERY PATH. The gate lives here rather than at
  // the pumpSync() call site because there are four callers, and three of them are the LAZY
  // path — enqueue(), the pumpTx() head-of-queue check and the cached-control restore. A
  // build that renders would otherwise register without ever having seen the display's own
  // `1C1 70`, which is exactly the application-driven ordering this rule exists to remove.
  // [CAP] 4/4: the display's 1C1 precedes our 151 by 60.69-61.34 ms, every time.
  if (_profile.registerAfterHello && !_peerChannelSeen) return false;
  if (_qCount + _funcCount > AFFA_TX_QUEUE_DEPTH) return false;

  static const uint8_t kReg[1] = { kRegisterByte };
  TxOptions ro;
  ro.coalesce = false;
  uint8_t at = 0;
  while (at < _qCount && _queue[at].started) ++at; // never move a WaitAck head
  for (uint8_t i = 0; i < _funcCount; ++i)
    pushJob(_funcIds[i], kReg, 1, JobKind::Registration, kNoTicket, ro,
            static_cast<uint8_t>(at + i));
  return true;
}

int AffaDisplayBase::findCoalescable(uint16_t funcId, RenderSlot s) const {
  if (s == RenderSlot::None) return -1;
  for (uint8_t i = 0; i < _qCount; ++i) {
    const TxJob& j = _queue[i];
    if (j.started) continue;              // TxJob::started is the single authority
    if (j.kind != JobKind::Payload) continue;
    if (!j.coalesce) continue;
    if (j.slot != s) continue;
    if (j.funcId != funcId) continue;     // the slot alone is not enough: on Carminat,
                                          // showMenu / setText / highlightItem all go out
                                          // on 0x151
    return static_cast<int>(i);
  }
  return -1;
}

uint8_t AffaDisplayBase::insertIndexFor(Priority p) const {
  if (p == Priority::Normal) return _qCount;
  uint8_t i = 0;
  // After the started job (never split a message on the wire) and after every queued
  // registration (the panel rejects a payload sent before its function is registered,
  // and the resulting SendFailed looks exactly like a wire-format bug).
  while (i < _qCount && (_queue[i].started || _queue[i].kind == JobKind::Registration)) ++i;
  return i;
}

void AffaDisplayBase::pushJob(uint16_t funcId, const uint8_t* d, uint16_t len, JobKind kind,
                              TxTicket t, const TxOptions& opt, uint8_t at,
                              const uint8_t* ext, uint16_t prefixLen) {
  if (_qCount >= AFFA_TX_QUEUE_DEPTH) return;      // callers check capacity first
  if (at > _qCount) at = _qCount;
  for (uint8_t i = _qCount; i > at; --i) _queue[i] = _queue[i - 1];
  ++_qCount;

  TxJob& j = _queue[at];
  j.funcId     = funcId;
  j.ticket     = t;
  j.kind       = kind;
  j.slot       = opt.slot;
  j.prio       = opt.priority;
  j.coalesce   = opt.coalesce;
  j.reassertAfterSession = opt.reassertAfterSession;
  j.started    = false;
  j.abandon    = false;
  j.len        = len;
  j.sent       = 0;
  j.frameIndex = 0;
  j.tries      = 0;
  j.readyAtMs  = _clock.millis();          // startable immediately; a retry moves it out
  j.holdUntilMs = _clock.millis() + AFFA_TX_HOLD_MS;
  j.ext        = ext;
  // A borrowed payload is never copied — that is the entire point — and `data` is left as
  // it was. Only the copying path may touch it, and only enqueue() reaches here with
  // len <= AFFA_MAX_PAYLOAD already enforced.
  j.prefixLen  = ext ? prefixLen : len;
  if (j.prefixLen) std::memcpy(j.data, d, j.prefixLen);
}

void AffaDisplayBase::removeJob(uint8_t index) {
  if (index >= _qCount) return;
  for (uint8_t i = index; i + 1 < _qCount; ++i) _queue[i] = _queue[i + 1];
  --_qCount;
}

Submitted AffaDisplayBase::enqueue(uint16_t funcId, const uint8_t* data, uint8_t len,
                                   TxOptions opt) {
  // PERMANENT REJECTIONS, and only these. Every one of them is a programming error the
  // caller can fix and no amount of waiting will: a null buffer, a payload past the
  // transport ceiling, an id this panel does not own. Rejecting is the whole point.
  //
  // THEY ARE ALSO THE ONLY CHECKS THAT NEED NO LIBRARY STATE — `_funcIds` is fixed at
  // construction — which is why they stay above the ticket mint. Step 4 of
  // docs/REFACTOR-2.0.md puts the cross-task branch immediately below this block, so a
  // foreign caller still gets these three answers synchronously, on its own task.
  if (!data || len == 0)      { _lastResult = Result::BadArgument; return Submitted::refused(Result::BadArgument); }
  if (len > AFFA_MAX_PAYLOAD) { _lastResult = Result::TooLong;     return Submitted::refused(Result::TooLong); }
  if (!knownFunc(funcId))     { _lastResult = Result::UnknownFunc; return Submitted::refused(Result::UnknownFunc); }

  // MINTED HERE, ABOVE EVERY STATE-DEPENDENT CHECK, and that placement is the whole point
  // of 2.0's handle model: the caller must be able to be handed its ticket without waiting
  // for the task that owns the queue to do anything. A ticket burned on a refusal below
  // costs nothing — the counter wraps at 0xFFFF and no refusal returns one.
  const TxTicket t = nextTicket();

  // THE TASK BOUNDARY, AND IT IS ONE LINE BECAUSE EVERY RENDER IN THIS LIBRARY FUNNELS HERE.
  // Above this point nothing has been read but the caller's arguments and a table fixed at
  // construction; below it, every decision reads state the poll owner mutates. So this is
  // where the call stops and the bytes travel instead. docs/REFACTOR-2.0.md §2.
  if (!onPollOwner()) return post(t, funcId, data, len, opt, nullptr, len);

  return admit(t, funcId, data, len, opt, nullptr, len);
}

// The state-dependent tail, against a ticket that was minted by whoever called. Shared by
// the three enqueue entry points and by the drain — which is what makes a posted render and
// a direct one the same code rather than two implementations that can disagree.
Submitted AffaDisplayBase::admit(TxTicket t, uint16_t funcId, const uint8_t* data,
                                 uint16_t len, const TxOptions& optIn, const uint8_t* ext,
                                 uint16_t prefixLen) {
  TxOptions opt = optIn;

  // The sole current durable control is Carminat power.  If a recovery replay is still
  // waiting in the queue, it represents an OLDER requested state.  Discard it before the
  // capacity check so a full queue cannot make a stale replay win over a new ON/OFF call.
  // A started replay is intentionally left alone: it has already reached the panel and is
  // ordered ahead of this new request on the real wire.
  if (opt.reassertAfterSession && opt.slot == RenderSlot::Control)
    (void)dropUnstartedReasserts();

  // A LINK THAT IS MERELY NOT READY IS NOT A REJECTION ANY MORE.
  //
  // Before 0.3.0 this returned NoSync or LinkDown here and the application owned the
  // recovery: notice the failure, keep the value, watch for sync, re-issue. Every consumer
  // wrote that loop and at least one wrote it without a backoff, which on a two-node bus
  // spins at loop rate and drives our own controller toward BUS_OFF (docs/API.md §3.1).
  //
  // So the job is ACCEPTED and HELD instead, for up to AFFA_TX_HOLD_MS, and pumpTx() starts
  // it when the link is usable. Latest-value-wins coalescing means the held job is always
  // the newest value for its slot, so what eventually lands is current rather than stale.
  // AFFA_TX_HOLD_MS = 0 restores the old behaviour exactly.
  const uint32_t now = _clock.millis();
  if (AFFA_TX_HOLD_MS == 0 && !linkReady()) {
    const Result r = _link.isLive() ? Result::NoSync : Result::LinkDown;
    _lastResult = r;
    return Submitted::refused(r);
  }

  // Lazy function registration, byte-identical to the legacy wire order: the first send
  // after a resync walks the WHOLE function table in declaration order and sends a 1-byte
  // 0x70 to each, then latches FUNCSREG, then the payload.
  //
  // Only when the link is up: registering into a dead bus would burn the queue slots the
  // held payload needs, and pumpTx() splices the burst in when the link returns anyway.
  const bool needReg = !_passive && linkReady() &&
                       !hasFlag(_sync, SyncState::FuncsReg) && !registrationQueued();

  // A BORROWED PAYLOAD IS NEVER COALESCABLE and the two guards say so independently:
  // enqueueExternal/enqueueSplit force opt.coalesce false AND refuse a RenderSlot, and
  // findCoalescable() returns -1 for RenderSlot::None. Both, because the failure the pair
  // prevents is a memcpy from a null `data` into a queue slot.
  int ci = -1;
  if (opt.coalesce && !ext) ci = findCoalescable(funcId, opt.slot);

  const uint8_t newSlots =
      static_cast<uint8_t>((ci >= 0 ? 0 : 1) + (needReg ? _funcCount : 0));
  if (_qCount + newSlots > AFFA_TX_QUEUE_DEPTH) {
    _lastResult = Result::QueueFull;
    return Submitted::refused(Result::QueueFull);
  }

  if (needReg) {
    (void)queueRegistrations();         // capacity was reserved above; the helper places
                                         // the wire-ordered probes ahead of held payloads
  }

  if (ci >= 0) {
    // Latest value wins: replace the payload in place, keeping the superseded job's queue
    // position so ordering relative to OTHER slots is what the application asked for.
    const TxTicket old = _queue[ci].ticket;
    TxJob& j = _queue[ci];
    std::memcpy(j.data, data, len);
    j.len      = len;
    // A coalesced job is always fully inline. Restoring both is defensive — a borrowed job
    // refuses a RenderSlot and so can never be found by findCoalescable() — but the failure
    // it would prevent is a stale pointer framing a screen that no longer exists.
    j.ext       = nullptr;
    j.prefixLen = len;
    j.ticket   = t;
    j.coalesce = opt.coalesce;
    j.reassertAfterSession = opt.reassertAfterSession;
    // A NEW VALUE GETS A NEW RETRY BUDGET, BUT NOT A NEW WIRE. `tries` resets — this is a
    // different render and it deserves its own attempts — while readyAtMs is left alone,
    // because the backoff protects the panel and the panel does not care that we changed
    // our mind. Resetting both would let a repainting application defeat the backoff.
    j.tries       = 0;
    j.holdUntilMs = now + AFFA_TX_HOLD_MS;

    if (opt.priority == Priority::Urgent && j.prio == Priority::Normal) {
      // A promotion cannot be silently ignored, so the entry moves to the urgent
      // insertion point as well.
      TxJob tmp = j;
      tmp.prio = Priority::Urgent;
      removeJob(static_cast<uint8_t>(ci));
      const uint8_t at = insertIndexFor(Priority::Urgent);
      for (uint8_t i = _qCount; i > at; --i) _queue[i] = _queue[i - 1];
      ++_qCount;
      _queue[at] = tmp;
    }

    _lastResult   = Result::Ok;
    completeTicket(old, Result::Aborted);   // state first, callback second
    return Submitted::accepted(t);
  }

  pushJob(funcId, data, len, JobKind::Payload, t, opt, insertIndexFor(opt.priority), ext,
          prefixLen);
  _lastResult   = Result::Ok;
  return Submitted::accepted(t);
}

Submitted AffaDisplayBase::enqueueExternal(uint16_t funcId, const uint8_t* data, uint16_t len,
                                          TxOptions opt) {
  if (!data || len == 0)               { _lastResult = Result::BadArgument; return Submitted::refused(Result::BadArgument); }
  if (len > AFFA_MAX_EXTERNAL_PAYLOAD) { _lastResult = Result::TooLong;     return Submitted::refused(Result::TooLong); }
  if (!knownFunc(funcId))              { _lastResult = Result::UnknownFunc; return Submitted::refused(Result::UnknownFunc); }
  // Both would re-copy a payload into a slot that does not own its storage. Refuse rather
  // than silently ignore: a caller that asked for latest-value-wins and did not get it
  // would see stale screens and no error.
  //
  // THE TEST IS THE SLOT, NOT TxOptions::coalesce — that flag DEFAULTS TO TRUE
  // (AFFA_TX_COALESCE), so testing it would reject every ordinary call. Coalescing only
  // ever fires for a real RenderSlot; RenderSlot::None is the raw-enqueue slot and never
  // coalesces, which is exactly what a borrowed payload wants.
  if (opt.slot != RenderSlot::None || opt.reassertAfterSession) {
    _lastResult = Result::BadArgument;
    return Submitted::refused(Result::BadArgument);
  }
  opt.coalesce = false;                 // belt and braces: nothing may replace these bytes

  const TxTicket t = nextTicket();      // see enqueue(): above every state-dependent check

  // THE BORROWED BYTES CROSS AS A POINTER, and that needs no new rule: the contract already
  // says they must stay valid and unchanged until the ticket completes, which outlives the
  // dispatch by definition.
  if (!onPollOwner()) return post(t, funcId, nullptr, len, opt, data, /*prefixLen=*/0);

  return admit(t, funcId, nullptr, len, opt, data, /*prefixLen=*/0);
}

Submitted AffaDisplayBase::enqueueSplit(uint16_t funcId, const uint8_t* prefix, uint8_t prefixLen,
                                       const uint8_t* body, uint16_t bodyLen, TxOptions opt) {
  const uint32_t total = static_cast<uint32_t>(prefixLen) + bodyLen;
  if (!prefix || !body || prefixLen == 0 || bodyLen == 0) {
    _lastResult = Result::BadArgument;
    return Submitted::refused(Result::BadArgument);
  }
  // The prefix is copied into TxJob::data, so it is bounded by that buffer and not by the
  // external ceiling — a caller that got this wrong would silently overrun a queue slot.
  if (prefixLen > AFFA_MAX_PAYLOAD || total > AFFA_MAX_EXTERNAL_PAYLOAD) {
    _lastResult = Result::TooLong;
    return Submitted::refused(Result::TooLong);
  }
  if (!knownFunc(funcId)) {
    _lastResult = Result::UnknownFunc;
    return Submitted::refused(Result::UnknownFunc);
  }
  if (opt.slot != RenderSlot::None || opt.reassertAfterSession) {
    _lastResult = Result::BadArgument;
    return Submitted::refused(Result::BadArgument);
  }
  opt.coalesce = false;

  const TxTicket t = nextTicket();      // see enqueue(): above every state-dependent check

  // The prefix is COPIED and the body is BORROWED, on this path and across the dispatch
  // alike — which is what lets a sixteen-byte header built on a web handler's stack be
  // prepended to an image in flash without either being copied whole.
  const uint16_t total16 = static_cast<uint16_t>(total);
  if (!onPollOwner()) return post(t, funcId, prefix, total16, opt, body, prefixLen);

  return admit(t, funcId, prefix, total16, opt, body, prefixLen);
}

// A failure worth another attempt: exactly the two that mean NOBODY ANSWERED.
//
//   Timeout   the panel did not answer within AFFA_ACK_TIMEOUT_MS — it was busy, asleep,
//             mid-reassembly, or our frames never arrived
//   LinkDown  the controller was not usable at that instant — bus-off, recovering
//
// SendFailed is deliberately NOT retryable, and the distinction is the whole point:
// it means the panel ANSWERED and the answer was neither DONE nor PARTIAL. That is a
// disagreement about the CONTENT, and re-sending byte-identical content to a panel that has
// just rejected it will get the same answer three more times — while burying the one
// diagnostic that says the builder is wrong under a retry storm. Silence is transient;
// rejection is not.
//
// NotSupported / BadArgument / TooLong / UnknownFunc are the caller's, and retrying them is
// a loop. Aborted / Cancelled are DELIBERATE — the application asked for them — and
// retrying one would be the library overruling the caller.
bool AffaDisplayBase::retryable(Result r) {
  return r == Result::Timeout || r == Result::LinkDown;
}

void AffaDisplayBase::armRetry(TxJob& job, uint32_t now, bool torn, bool extendHold) {
  // Doubling, capped. `tries` has already been incremented, so the first backoff is
  // AFFA_TX_RETRY_MS exactly.
  uint32_t back = AFFA_TX_RETRY_MS;
  for (uint8_t i = 1; i < job.tries && back < AFFA_TX_RETRY_MAX_MS; ++i) back <<= 1;
  if (back > AFFA_TX_RETRY_MAX_MS) back = AFFA_TX_RETRY_MAX_MS;

  // A TORN TRANSFER OWES THE PANEL SILENCE. Its reassembler is holding our first frames and
  // waiting for continuations; the next thing it should hear is nothing, long enough to
  // give up, rather than a fresh first frame landing inside the old message.
  if (torn) back += AFFA_TX_DIRTY_QUIET_MS;

  job.readyAtMs = now + back;
  job.sent       = 0;
  job.frameIndex = 0;      // MANDATORY: a stale continuation counter would make the retry's
  job.started    = false;  // first frame start at 0x2n instead of its own byte 0
  job.abandon    = false;
  // The hold window is re-armed for a job that actually got a hearing and failed — it is
  // being actively worked, not sitting unwanted. It is deliberately NOT re-armed for a link
  // fault: that path does not spend a retry either, so the hold window is the only thing
  // bounding it, and extending it would make "wait for a usable link" mean "for ever".
  if (extendHold) job.holdUntilMs = now + AFFA_TX_HOLD_MS;
}

// Passive mode never handshakes, so "ready" there is only "is there a controller".
bool AffaDisplayBase::linkReady() const {
  if (!_link.isLive()) return false;
  if (_passive) return true;
  if (!openingReleased()) return false;
  return !hasFlag(_sync, SyncState::Failed);
}

void AffaDisplayBase::pumpTx() {
  const uint32_t now = _clock.millis();

  // WaitAck is the ONLY state in which a job may be abandoned. That is exactly what
  // "abandoned at a frame boundary, never mid-frame" means: the CAN frame already handed
  // to the link is always transmitted whole, and the next frame of that job is simply
  // never built.
  if (_tx == TxState::WaitAck) {
    if (_selfAck && _selfAckPending != SelfAck::None) {
      const SelfAck a = _selfAckPending;
      _selfAckPending = SelfAck::None;
      creditAck(a == SelfAck::Done);
    } else if (!_link.isLive()) {
      finishJob(Result::LinkDown);
    } else if (expired(now, _ackDeadlineMs)) {
      const bool abandon = (_qCount > 0) && _queue[0].abandon;
      finishJob(abandon ? Result::Aborted : Result::Timeout);
    }
  }

  if (_tx == TxState::WaitAck) return;

  // A completed durable control (Carminat power) is library-owned desired state. It must
  // bring the panel back to that state after re-registration even when the application has
  // been quiet, and it must do so before held time/text work. A newer queued Control payload
  // already expresses a better desired value, so it wins and suppresses the stale replay.
  if (_cachedControl.valid && _cachedControl.pending && linkReady()) {
    if (!hasFlag(_sync, SyncState::FuncsReg)) {
      (void)queueRegistrations();
    } else if (expired(now, _nextPayloadMs) && !hasQueuedControl() && !reassertQueued() &&
               _qCount < AFFA_TX_QUEUE_DEPTH) {
      TxOptions internal;
      internal.slot = RenderSlot::Control;
      internal.priority = Priority::Urgent;
      internal.coalesce = false;
      pushJob(_cachedControl.funcId, _cachedControl.data, _cachedControl.len,
              JobKind::Reassert, kNoTicket, internal, insertIndexFor(Priority::Urgent));
    }
  }

  if (_qCount == 0) { _tx = TxState::Idle; return; }

  // ---- the three gates in front of the head job -----------------------------
  // HEAD-OF-LINE, DELIBERATELY. A job waiting out a backoff stalls the queue behind it
  // rather than being skipped, because skipping would reorder renders the application
  // issued in sequence — and ordering is a contract (docs/API.md §3b.4). Priority::Urgent
  // still overtakes: a retrying job has started == false, so insertIndexFor() puts an
  // urgent one in front of it.
  if (!expired(now, _queue[0].readyAtMs)) return;

  // 2. The link is not usable. The head is HELD, not failed — until its hold window runs
  //    out, at which point it is given up as Cancelled rather than left there for ever.
  if (!linkReady()) {
    if (expired(now, _queue[0].holdUntilMs)) {
      AFFA_LOGW(kTag, "held %d ms without a usable link, giving up",
                static_cast<int>(AFFA_TX_HOLD_MS));
      finishJob(_link.isLive() ? Result::NoSync : Result::LinkDown, /*allowRetry=*/false);
    }
    return;
  }

  // The post-registration quiet interval is profile data, not a blocking delay. It applies
  // equally to a restored power state and to application work, but never to the 0x70 probes
  // themselves.
  if (_queue[0].kind != JobKind::Registration && !expired(now, _nextPayloadMs)) return;

  // 3. The panel has forgotten us — a resync happened while this job was queued, or the job
  //    was enqueued before the link came up. The lazy registration burst goes in FRONT of
  //    it, exactly as enqueue() would have done had the link been up at the time. Without
  //    this, a held payload would go out to a panel that rejects it and the SendFailed
  //    would look exactly like a wire-format bug.
  if (!_passive && _queue[0].kind == JobKind::Payload &&
      !hasFlag(_sync, SyncState::FuncsReg) && !registrationQueued()) {
    // THE HOLD WINDOW BINDS HERE TOO. Gate 2 checks it only when the link is down — but a
    // payload can sit behind a registration that fails and re-splices for ever on a link
    // that is perfectly "ready" (the 61-11-01 storm: every probe times out, dropped probes
    // re-splice on the next pass, ~10 s per lap, indefinitely). Bench, 2026-07-29:
    // inFlight climbing into the thousands while timeouts tracked it one for one. A head
    // job that is not yet started and has outlived its hold window is given up as NoSync —
    // the same verdict, at the same age, that gate 2 would have delivered.
    if (!_queue[0].started && expired(now, _queue[0].holdUntilMs)) {
      AFFA_LOGW(kTag, "registration never completed within the hold window — giving up");
      finishJob(Result::NoSync, /*allowRetry=*/false);
      return;
    }
    if (!queueRegistrations()) return;
    // Fall through: the head is now the first registration probe.
  }

  TxJob& job = _queue[0];

  // Frame 0 carries EIGHT raw payload bytes with no PCI added by the transport — the 0x10
  // at the head of a screen payload is payload byte 0, built by the caller. Continuation
  // frames carry seven, prefixed with 0x20 | (num & 0x0F).
  Frame f;
  f.id  = job.funcId;
  f.len = kPacketLength;
  uint8_t i = 0;
  if (job.frameIndex > 0) f.data[i++] = isoTpCf(job.frameIndex);
  // Build against a local cursor. `job.sent` is protocol state, not a staging cursor: a
  // locally full controller has not accepted any bytes, so it stays unchanged until the
  // link accepts this frame.
  // Inline prefix first, then the borrowed body. For every job that is not a split send one
  // of the two halves is empty, so this is the ordinary path with one predictable branch —
  // not a special case bolted on beside it.
  uint16_t proposedSent = job.sent;
  while (i < kPacketLength && proposedSent < job.len) {
    f.data[i++] = (proposedSent < job.prefixLen) ? job.data[proposedSent]
                                                 : job.ext[proposedSent - job.prefixLen];
    ++proposedSent;
  }
  const bool more = (proposedSent < job.len);
  const uint8_t filler = packetFiller();
  while (i < kPacketLength) f.data[i++] = filler;

  // MARK IT STARTED BEFORE THE SEND, NOT AFTER. txFrame() calls observe(), which runs the
  // frame tap and every Direction::Tx subscription — application code, which docs/API.md
  // §4.2/§4.3 explicitly permit to call enqueue(), any render, abortPending() and
  // abortAll(). Every one of those decisions keys off TxJob::started, so with the flag set
  // afterwards a Tx callback saw this job as preemptable while its first frame was already
  // on the wire: abortPending() dropped it and `job` then pointed at the job that shifted
  // into its place, a coalescing render overwrote its payload mid-ISO-TP with j.sent
  // already past zero, and an Urgent enqueue spliced itself in at index 0 and inherited
  // the WaitAck. Set first, and the send failure path below clears it again via
  // finishJob().
  // Offer before committing. A transient local queue-full is not a wire failure and must
  // not consume a byte, start a job, emit a Tx tap, or spend a protocol retry. The bounded
  // ready deadline makes a busy driver benign even when poll() is called in a tight loop.
  const TxDisposition offered = txFrame(f, /*observeAccepted=*/false);
  if (offered == TxDisposition::Busy) {
    job.readyAtMs = now + AFFA_TX_RETRY_MS;
    return;
  }

  // A hard refusal by a link that says it is down is LinkDown, not SendFailed. The
  // distinction decides whether this job is held for recovery; a live hard refusal remains
  // the existing terminal driver verdict for legacy boolean links.
  if (offered != TxDisposition::Accepted) {
    finishJob(_link.isLive() ? Result::SendFailed : Result::LinkDown);
    return;
  }

  // Commit only AFTER acceptance, but BEFORE the Tx observation callback. Callbacks may
  // enqueue, coalesce, abortPending or abortAll, and all of them must see this frame as an
  // in-flight, non-preemptable job. Set WaitAck before observe for the same reason; no
  // reference into _queue is used after observe because callbacks may move the queue.
  job.sent    = proposedSent;
  job.started = true;
  _ackDeadlineMs = now + AFFA_ACK_TIMEOUT_MS;
  _tx            = TxState::WaitAck;
  if (_selfAck) _selfAckPending = more ? SelfAck::Partial : SelfAck::Done;
  f.fromSelf = true;
  observe(f, Direction::Tx);
}

void AffaDisplayBase::creditAck(bool done) {
  if (_qCount == 0) { _tx = TxState::Idle; return; }
  TxJob& job = _queue[0];

  if (done) {
    // DONE WHILE BYTES REMAIN IS SUCCESS, NOT A SHORT WRITE. The panel ends the transfer
    // as soon as it holds the number of content bytes the first frame DECLARED, which is
    // fewer than showMenu's builder holds — so this is the normal path on every single
    // menu render, not an edge case. Reporting SendFailed here would make the menu look
    // permanently broken.
    finishJob(Result::Ok);
    return;
  }

  if (job.sent >= job.len) {
    // PARTIAL after the last frame: the panel wants more and there is none.
    finishJob(Result::SendFailed);
    return;
  }
  if (job.abandon) { finishJob(Result::Aborted); return; }

  ++job.frameIndex;
  _tx = TxState::SendingFrame;
}

void AffaDisplayBase::finishJob(Result r, bool allowRetry) {
  if (_qCount == 0) { _tx = TxState::Idle; return; }

  // ---- retry, before anything is torn down --------------------------------
  // THE APPLICATION NEVER SEES A TRANSIENT FAILURE IT COULD HAVE RETRIED ITSELF. It sees
  // one verdict per ticket: the value that finally landed, or the failure that survived
  // AFFA_TX_MAX_RETRIES attempts. Before 0.3.0 every one of these reached onComplete and
  // the consumer owned the loop — see docs/API.md §3.1 for what that cost.
  // A LINK FAULT DOES NOT SPEND AN ATTEMPT. LinkDown means the controller was not usable at
  // that instant — bus-off, mid-recovery, TX gated for an OTA — and the job never got a
  // hearing. Counting it as a try lets a flapping bus exhaust the budget in seconds and
  // report a failure for a message the panel was never asked about. Measured on the rig:
  // 134 controller flaps in twelve minutes, 8% of the time not RUNNING, and 45 renders
  // given up as LinkDown that had nothing wrong with them.
  //
  // It is bounded by the HOLD window instead, which is the right bound: "wait for a usable
  // link, but not for ever" is exactly what holdUntilMs already means, and unlike the retry
  // counter it is not extended each time round.
  const bool linkFault = (r == Result::LinkDown);
  if (allowRetry && AFFA_TX_MAX_RETRIES > 0 && retryable(r) &&
      (linkFault || _queue[0].tries < AFFA_TX_MAX_RETRIES)) {
    TxJob& j = _queue[0];
    const bool torn = (j.sent > 0);     // bytes were already on the wire: the panel is
                                        // holding a partial and wants silence, not a retry
    if (!linkFault) ++j.tries;
    armRetry(j, _clock.millis(), torn, /*extendHold=*/!linkFault);
    _tx             = TxState::Idle;
    _selfAckPending = SelfAck::None;
    AFFA_LOGD(kTag, "retry %u/%d for 0x%03X after %u%s",
              static_cast<unsigned>(j.tries), AFFA_TX_MAX_RETRIES,
              static_cast<unsigned>(j.funcId), static_cast<unsigned>(r),
              torn ? " (torn, quiet first)" : "");
    return;
  }

  TxJob& job = _queue[0];
  const JobKind  kind   = job.kind;
  const TxTicket ticket = job.ticket;
  const uint16_t funcId = job.funcId;   // read before removeJob() shifts the array under
                                        // `job`, which is a reference into it
  // enqueueExternal() refuses reassertAfterSession, so a borrowed job can never reach the
  // cache — but the ceiling is asserted here too rather than trusted, because the copy
  // below is into a fixed AFFA_MAX_PAYLOAD stack buffer and job.len is now 16-bit.
  const RenderSlot slot = job.slot;
  const bool cacheAfterAck =
      job.reassertAfterSession && !job.ext && job.len <= AFFA_MAX_PAYLOAD;
  const uint8_t cachedLen = static_cast<uint8_t>(cacheAfterAck ? job.len : 0);
  uint8_t cachedData[AFFA_MAX_PAYLOAD] = {0};
  if (cacheAfterAck) std::memcpy(cachedData, job.data, cachedLen);
  (void)funcId;                         // its only consumer is a log line, which
                                        // AFFA_ENABLE_LOG=0 compiles away entirely

  // Unconditionally, including on the abandoned path: an abandoned ISO-TP sequence that
  // left a continuation counter behind would corrupt the NEXT message's first frame,
  // which would then start at a stray 0x2n instead of its own byte 0.
  job.frameIndex = 0;
  job.sent       = 0;
  job.started    = false;
  job.abandon    = false;

  removeJob(0);
  _tx             = TxState::Idle;
  _selfAckPending = SelfAck::None;

  if (kind == JobKind::Registration) {
    if (r == Result::Ok) {
      if (!registrationQueued()) {
        _nextPayloadMs = _clock.millis() + _profile.payloadAfterRegistrationMs;
        setSync(_sync | SyncState::FuncsReg);
        // THE WHOLE TABLE IS ACKED. Not Ready yet: the captured radio waits ~400 ms before
        // its first payload, and a render inside that window is a screen the panel takes
        // and does not draw. poll() promotes Settling -> Ready when the interval expires.
        enterPhase(Phase::Settling);

        // AND THE GLASS GETS LIT — QUEUED HERE, A FULL QUIET INTERVAL BEFORE IT CAN GO OUT,
        // AND THAT IS THE POINT. `03 52 09` must precede any render, so it has to be IN THE
        // QUEUE before the application's held work becomes eligible, not raced against it
        // when the gate opens. Queued here it inherits the same `_nextPayloadMs` gate as
        // every other payload and simply leaves first.
        //
        // See setAutoPower(). The three ways it declines are all "somebody else already has
        // an opinion, or there is nothing to have an opinion about":
        //   * a desired power state is already queued or cached — including a deliberate
        //     OFF, which must not be undone by a session the application did not ask for
        //   * the family has no power command, so setPower() returns NotSupported
        //   * the build turned it off
        // Any of those leaves `_autoPowerTicket` clear and the phase goes straight to Ready.
        const bool appOwnsPower =
            _cachedControl.valid || hasQueuedControl() || reassertQueued();
        if (_autoPower && !_passive && !appOwnsPower && supports(Feature::Power)) {
          // The ticket comes straight back from the call now. It used to be fetched from
          // _lastEnqueued immediately afterwards, which worked only because nothing could
          // enqueue between the two statements — true here, and a rule the rest of the
          // library had to keep in its head everywhere else.
          _autoPowerTicket = setPower(true).ticket;
        }
      }
    } else {
      // IT NO LONGER TAKES THE PAYLOADS WITH IT. The legacy affa3_send propagated a failed
      // registration to its caller and this did the same — failAllQueued(r) — which meant a
      // panel that was briefly unreachable destroyed every render behind it and the
      // application had to notice and re-issue them.
      //
      // Now: the dead probes are dropped and the payloads stay HELD. pumpTx() splices a
      // fresh registration burst in front of them the next time the link is ready, and if
      // that never happens their own hold windows give up on them. Bounded, and the
      // application is not part of the loop.
      AFFA_LOGW(kTag, "registration of 0x%03X failed (%u) — payloads held for retry",
                static_cast<unsigned>(funcId), static_cast<unsigned>(r));
      dropRegistrations();
    }
    return;                        // registration jobs carry kNoTicket and are invisible
  }

  if (kind == JobKind::Reassert) {
    // The cached value itself remains valid; a terminal failed internal replay merely
    // waits for the next genuine session loss/reregistration instead of spinning forever.
    _cachedControl.pending = false;
    return;
  }

  if (cacheAfterAck && r == Result::Ok) {
    _cachedControl.valid = true;
    _cachedControl.pending = false;
    _cachedControl.funcId = funcId;
    _cachedControl.len = cachedLen;
    std::memcpy(_cachedControl.data, cachedData, cachedLen);
  }

  // THE PANEL SAID YES. Not "we enqueued it" and not "we put it on the wire" — a render is
  // only on the glass once the transfer has been acknowledged, so that is where this is set.
  //
  // RenderSlot::Control IS EXCLUDED, and the test that caught it is the reason: setPower is a
  // payload with a slot but it draws nothing, so after the opening lastRendered() reported
  // Control and an application asking "is my text still on the glass?" got no for ever.
  // Registration and reassert jobs are not screens either.
  if (r == Result::Ok && kind == JobKind::Payload &&
      slot != RenderSlot::None && slot != RenderSlot::Control) {
    _lastRendered   = slot;
    _lastRenderedMs = _clock.millis();
  }

  completeTicket(ticket, r);
}

void AffaDisplayBase::completeTicket(TxTicket t, Result r) {
  if (t == kNoTicket) return;
  _lastCompleted = t;
  _lastResult    = r;

  // THE GLASS IS ON — or it is not going to be, and Ready is still the right answer.
  //
  // This is the edge that ends Phase::Powering, and it fires on ANY terminal result, not
  // only on Ok. A power command the panel refused leaves the display dark, which is bad; a
  // library that answers "not Ready" for ever because of it is worse, because then nothing
  // renders and the application has no way to find out why. The failure is on the ticket
  // and in the log; the phase moves on.
  if (t == _autoPowerTicket) {
    _autoPowerTicket = kNoTicket;
    if (_phase == Phase::Powering) {
      if (r != Result::Ok)
        AFFA_LOGW(kTag, "auto power-on failed (%u) — the glass may be dark",
                  static_cast<unsigned>(r));
      enterPhase(Phase::Ready);
    }
  }

  // One completion, one callback. The Layer 2 TxComplete event that used to follow carried
  // byte-for-byte the same (ticket, result) pair to a second sink nobody installed.
  if (_cplCb) {
    CbTimer g(*this, CbKind::Complete);
    _cplCb(t, r, _cplCtx);
  }
}

void AffaDisplayBase::failAllQueued(Result r) {
  TxTicket t[AFFA_TX_QUEUE_DEPTH];
  uint8_t n = 0;
  for (uint8_t i = 0; i < _qCount; ++i) {
    if (_queue[i].kind == JobKind::Payload && _queue[i].ticket != kNoTicket)
      t[n++] = _queue[i].ticket;
  }
  _qCount         = 0;
  _tx             = TxState::Idle;
  _selfAckPending = SelfAck::None;
  for (uint8_t i = 0; i < n; ++i) completeTicket(t[i], r);
}

uint8_t AffaDisplayBase::dropUnstarted(Result r) {
  TxTicket t[AFFA_TX_QUEUE_DEPTH];
  uint8_t n = 0;
  uint8_t i = 0;
  // The queue is mutated FIRST and the callbacks fire second, so a nested abortPending()
  // from inside one of those CompleteCb invocations finds nothing and returns 0.
  while (i < _qCount) {
    TxJob& j = _queue[i];
    if (!j.started && j.kind == JobKind::Payload) {
      if (j.ticket != kNoTicket) t[n++] = j.ticket;
      removeJob(i);
      continue;
    }
    ++i;
  }
  for (uint8_t k = 0; k < n; ++k) completeTicket(t[k], r);
  return n;
}

uint8_t AffaDisplayBase::dropRegistrations() {
  uint8_t n = 0;
  uint8_t i = 0;
  while (i < _qCount) {
    // A STARTED registration is left alone for the same reason any started job is: its
    // first frame is already on the wire and removing it would leave the FSM waiting for an
    // ACK against a job that is no longer at the head.
    if (_queue[i].kind == JobKind::Registration && !_queue[i].started) {
      removeJob(i);
      ++n;
      continue;
    }
    ++i;
  }
  return n;
}

// Zero is reserved for "never stamped", so a job created before the first session cannot
// accidentally compare equal to a live epoch after a wrap.
void AffaDisplayBase::advanceSessionEpoch() {
  if (++_sessionEpoch == 0) _sessionEpoch = 1;
}

// A new panel session voids the protocol ACK the head job is currently waiting for: the
// panel has forgotten our registrations, so a `74` arriving now answers a question nobody
// is still asking. Registration probes are discarded outright; an application payload is
// rewound to its first byte and held until the new registration completes, because its
// bytes are what the application still wants on the glass.
void AffaDisplayBase::invalidateInFlightForSession(uint32_t now) {
  advanceSessionEpoch();
  dropRegistrations();

  if (_qCount > 0 && _queue[0].started) {
    TxJob& j = _queue[0];
    if (j.kind == JobKind::Registration) {
      removeJob(0);
    } else {
      // Torn mid-transfer: the panel is owed silence before the same funcId speaks again.
      j.started     = false;
      j.sent        = 0;
      j.frameIndex  = 0;
      j.readyAtMs   = now + AFFA_TX_DIRTY_QUIET_MS;
      j.holdUntilMs = now + AFFA_TX_HOLD_MS;
    }
    _tx             = TxState::Idle;
    _selfAckPending = SelfAck::None;
    _ackDeadlineMs  = now;
  }

  // Nothing left in the queue belongs to the session that just died.
  for (uint8_t i = 0; i < _qCount; ++i) _queue[i].sessionEpoch = 0;
}

uint8_t AffaDisplayBase::abortPending() { return dropUnstarted(Result::Aborted); }

bool AffaDisplayBase::abortAll() {
  dropUnstarted(Result::Aborted);
  if (_qCount > 0 && _queue[0].started) { _queue[0].abandon = true; return true; }
  return false;
}

bool AffaDisplayBase::pending(RenderSlot s) const {
  for (uint8_t i = 0; i < _qCount; ++i)
    if (!_queue[i].started && _queue[i].kind == JobKind::Payload && _queue[i].slot == s)
      return true;
  return false;
}

} // namespace affa
