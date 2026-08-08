// AffaObserve — the observation seam, and the choke point every frame passes through.
//
// Split out of a 2156-line AffaDisplayBase.cpp by step 7 of docs/API.md §7. One
// class, four translation units: AffaDisplayBase.cpp (lifecycle, poll() orchestration,
// receive drain, keys, public surface), AffaSync.cpp (the opening FSM), AffaTx.cpp (queue,
// ISO-TP segmentation, flow control, retries) and this one.
//
// WHY txFrame() IS HERE AND NOT IN AffaTx.cpp. It is not "the transmit path" — it is the
// single point at which a frame becomes visible, in EITHER direction, and it is inseparable
// from observe() for that reason. A sniffer sees the whole bus in wire order precisely
// because these two sit next to each other and nothing else touches the link.
//
// WHY pumpText() IS HERE. It is inbound text ANOTHER node drew on the panel — the sniff
// seam, costed by AFFA_ENABLE_ISOTP_RX. It reassembles and delivers; it never renders.
#include "AffaBaseInternal.h"
#include "AffaDisplayBase.h"

#include <cstring>

namespace affa {
using namespace basedetail;

// ---------------------------------------------------------------------------
// Observation seam
// ---------------------------------------------------------------------------

void AffaDisplayBase::setLogSink(ILogSink* s) { detail::setSink(s); }
void AffaDisplayBase::onKey(KeyCb cb, void* ctx)      { _keyCb = cb;  _keyCtx = ctx; }
void AffaDisplayBase::onComplete(CompleteCb cb, void* ctx) { _cplCb = cb; _cplCtx = ctx; }
void AffaDisplayBase::onSync(SyncCb cb, void* ctx)    { _syncCb = cb; _syncCtx = ctx; }
void AffaDisplayBase::onFrame(FrameTap cb, void* ctx) { _tap = cb;   _tapCtx = ctx; }
#if AFFA_ENABLE_ISOTP_RX
void AffaDisplayBase::onText(TextCb cb, void* ctx)    { _textCb = cb; _textCtx = ctx; }
#endif

// THE WHOLE SEAM, and it is now what it always should have been: hand the frame to the
// tap. The subscription table that used to be walked twice here, for every frame, in both
// directions, dispatched to nobody in nineteen shipped examples — see the comment above
// Direction in AffaTypes.h.
void AffaDisplayBase::observe(const Frame& f, Direction d) {
  if (!_tap) return;
  // THE MOST EXPENSIVE PLACE TO BE SLOW in the whole library: this runs for every frame in
  // both directions, so a tap that takes a millisecond costs a millisecond per frame on a
  // bus carrying hundreds a second. It is also the one people write first.
  CbTimer t(*this, CbKind::FrameTap);
  _tap(f, d, _tapCtx);
}

// ---------------------------------------------------------------------------
// Callback attribution
// ---------------------------------------------------------------------------

#if AFFA_CALLBACK_BUDGET_MS > 0
void AffaDisplayBase::noteCallback(CbKind k, uint32_t ms) {
  if (ms < AFFA_CALLBACK_BUDGET_MS) return;
  ++_cbOverruns;

  // THE WORST, NOT THE LAST. A 400 ms callback that fired once during boot is the
  // interesting one, and keeping only the most recent would let a hundred 20 ms ones bury
  // it before anybody looked.
  const uint32_t now = _clock.millis();
  if (ms > _slowestCbMs) {
    _slowestCbMs   = ms;
    _slowestCb     = k;
    _slowestCbAtMs = now;
  }

  // One line per second at most. A callback that overruns every iteration would otherwise
  // produce exactly the log storm that hides the first occurrence — which is the one that
  // says what changed.
  if (expired(now, _cbLogMs)) {
    _cbLogMs = now + 1000;
    AFFA_LOGW(kTag, "%s blocked the poll task for %lu ms (budget %u) — overrun #%lu",
              cbName(k), static_cast<unsigned long>(ms),
              static_cast<unsigned>(AFFA_CALLBACK_BUDGET_MS),
              static_cast<unsigned long>(_cbOverruns));
  }
}

CbKind   AffaDisplayBase::slowestCallback()     const { return _slowestCb; }
uint32_t AffaDisplayBase::slowestCallbackMs()   const { return _slowestCbMs; }
uint32_t AffaDisplayBase::slowestCallbackAtMs() const { return _slowestCbAtMs; }
uint32_t AffaDisplayBase::callbackOverruns()    const { return _cbOverruns; }
void     AffaDisplayBase::resetCallbackPeak() {
  _slowestCb = CbKind::None; _slowestCbMs = 0; _slowestCbAtMs = 0;
}
#else
void     AffaDisplayBase::noteCallback(CbKind, uint32_t) {}
CbKind   AffaDisplayBase::slowestCallback()     const { return CbKind::None; }
uint32_t AffaDisplayBase::slowestCallbackMs()   const { return 0; }
uint32_t AffaDisplayBase::slowestCallbackAtMs() const { return 0; }
uint32_t AffaDisplayBase::callbackOverruns()    const { return 0; }
void     AffaDisplayBase::resetCallbackPeak() {}
#endif

TxDisposition AffaDisplayBase::txFrame(Frame f, bool observeAccepted) {
  // The stamp is applied to our copy, never to the caller's buffer: a panel builder may
  // reuse its frame struct, and a stray fromSelf on a frame we later treat as inbound
  // would be silently ignored by the whole receive path.
  f.fromSelf = true;
  const TxDisposition disposition = _link.trySend(f);
  if (disposition == TxDisposition::Rejected) {
    // COUNTED, NOT ANNOUNCED. This used to also build a Layer 2 LinkError event. The count
    // is what anybody ever read, and it is on Stats where a status page can find it.
    ++_txDropCount;
    return disposition;
  }
  // A Busy or Rejected frame is deliberately not observed: it never existed on the bus.
  if (disposition == TxDisposition::Accepted && observeAccepted) observe(f, Direction::Tx);
  return disposition;
}

#if AFFA_ENABLE_ISOTP_RX
// Reached only for a frame that came off the wire (fromSelf is dropped above), so this
// never decodes our own renders back into onText.
void AffaDisplayBase::pumpText(const Frame& f) {
  const uint16_t id = textRxId();
  if (!_textCb || id == 0 || f.id != id) return;
  if (!_textAsm.onFrame(f)) return;                 // not an ISO-TP data frame

  const uint8_t* p   = _textAsm.buffer();
  const uint8_t  len = _textAsm.len();
  if (len < 2) return;

  // COMPLETION IS THE DECLARED LENGTH, not a frame count: payload[1] is the content length
  // and payload[0..1] are not content, so the message ends at 2 + p[1]. Emitting per
  // appended frame instead would deliver the same screen once per continuation.
  //
  // The second arm is the ceiling: Reassembler stops appending at AFFA_MAX_PAYLOAD, so a
  // message declaring more than that never satisfies the first and would otherwise be
  // dropped in silence. We deliver what we have — short, which the decoders reject on
  // length if it is too short to mean anything.
  const uint16_t need = static_cast<uint16_t>(2u + p[1]);
  if (len < need && len < AFFA_MAX_PAYLOAD) return;

  char out[AFFA_TEXT_MAX];
  const bool ok = decodeText(p, len, out, sizeof(out));
  // Reset BEFORE the callback, never after: the callback may render, and a render may
  // re-enter poll() in an application that pumps from one. It must not find a transfer
  // this call has already consumed.
  _textAsm.reset();
  if (ok) {
    CbTimer t(*this, CbKind::Text);
    _textCb(out, _textCtx);
  }
}
#endif

} // namespace affa
