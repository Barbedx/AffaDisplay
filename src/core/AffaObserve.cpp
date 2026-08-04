// AffaObserve — the observation seam, and the choke point every frame passes through.
//
// Split out of a 2156-line AffaDisplayBase.cpp by step 7 of docs/REFACTOR-PLAN.md. One
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
void AffaDisplayBase::onEvent(EventCb cb, void* ctx)  { _evCb = cb;  _evCtx = ctx; }
#if AFFA_ENABLE_ISOTP_RX
void AffaDisplayBase::onText(TextCb cb, void* ctx)    { _textCb = cb; _textCtx = ctx; }
#endif

SubHandle AffaDisplayBase::subscribe(const FrameMatch& m, FrameCb cb, void* ctx) {
#if AFFA_MAX_SUBSCRIPTIONS > 0
  if (!cb) return kNoSub;
  if (m.len > 8) return kNoSub;
  if (static_cast<uint8_t>(m.dir) == 0) return kNoSub;   // matches nothing, ever
  for (uint8_t i = 0; i < AFFA_MAX_SUBSCRIPTIONS; ++i) {
    if (_subs[i].used) continue;
    _subs[i].m    = m;
    _subs[i].cb   = cb;
    _subs[i].ctx  = ctx;
    _subs[i].used = true;
    // Handle = (slot + 1) | generation << 8. The +1 keeps zero meaning "no handle", and
    // the generation is what stops a stale handle from unsubscribing the slot's next
    // owner — the silent failure mode of a bare index.
    SubHandle h;
    h.v = static_cast<uint16_t>((i + 1) | (static_cast<uint16_t>(_subs[i].gen) << 8));
    return h;
  }
  AFFA_LOGW(kTag, "subscription table full (%d slots)", AFFA_MAX_SUBSCRIPTIONS);
  return kNoSub;
#else
  (void)m; (void)cb; (void)ctx;
  return kNoSub;
#endif
}

bool AffaDisplayBase::unsubscribe(SubHandle h) {
#if AFFA_MAX_SUBSCRIPTIONS > 0
  if (!h.valid()) return false;
  const uint8_t idx = static_cast<uint8_t>((h.v & 0xFF) - 1);
  const uint8_t gen = static_cast<uint8_t>(h.v >> 8);
  if (idx >= AFFA_MAX_SUBSCRIPTIONS) return false;
  if (!_subs[idx].used || _subs[idx].gen != gen) return false;
  _subs[idx].used = false;
  _subs[idx].cb   = nullptr;
  ++_subs[idx].gen;
  return true;
#else
  (void)h;
  return false;
#endif
}

uint8_t AffaDisplayBase::subscriptions() const {
#if AFFA_MAX_SUBSCRIPTIONS > 0
  uint8_t n = 0;
  for (uint8_t i = 0; i < AFFA_MAX_SUBSCRIPTIONS; ++i) if (_subs[i].used) ++n;
  return n;
#else
  return 0;
#endif
}

void AffaDisplayBase::observe(const Frame& f, Direction d) {
  if (_tap) _tap(f, d, _tapCtx);

#if AFFA_MAX_SUBSCRIPTIONS > 0
  // Snapshot which slots were occupied on entry. subscribe() and unsubscribe() are legal
  // from inside a FrameCb, and this is what makes both cases well-defined: a slot freed
  // mid-walk is skipped rather than mis-dispatched, and a slot added mid-walk is not
  // delivered the frame currently being dispatched.
  uint32_t live = 0;
  for (uint8_t i = 0; i < AFFA_MAX_SUBSCRIPTIONS; ++i)
    if (_subs[i].used) live |= (1u << i);

  for (uint8_t i = 0; i < AFFA_MAX_SUBSCRIPTIONS; ++i) {
    if ((live & (1u << i)) == 0) continue;
    if (!_subs[i].used) continue;                      // unsubscribed by an earlier cb
    const FrameMatch& m = _subs[i].m;
    if ((static_cast<uint8_t>(d) & static_cast<uint8_t>(m.dir)) == 0) continue;
    if (((f.id ^ m.id) & m.idMask) != 0) continue;
    if (m.len != 0) {
      if (f.len < m.len) continue;                     // short frames never match
      bool ok = true;
      for (uint8_t b = 0; b < m.len; ++b) {
        if (((f.data[b] ^ m.data[b]) & m.dataMask[b]) != 0) { ok = false; break; }
      }
      if (!ok) continue;
    }
    _subs[i].cb(f, _subs[i].ctx);
  }
#else
  (void)d;
#endif
}

void AffaDisplayBase::emit(const Event& ev) {
  if (_evCb) _evCb(ev, _evCtx);
}

void AffaDisplayBase::reportLinkError(LinkErrorKind k, uint32_t count) {
  Event ev;
  ev.kind        = EventKind::LinkError;
  ev.error.kind  = k;
  ev.error.count = count;
  emit(ev);
}

TxDisposition AffaDisplayBase::txFrame(Frame f, bool observeAccepted) {
  // The stamp is applied to our copy, never to the caller's buffer: a panel builder may
  // reuse its frame struct, and a stray fromSelf on a frame we later treat as inbound
  // would be silently ignored by the whole receive path.
  f.fromSelf = true;
  const TxDisposition disposition = _link.trySend(f);
  if (disposition == TxDisposition::Rejected) {
    ++_txDropCount;
    reportLinkError(LinkErrorKind::TxDropped, _txDropCount);
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
  if (ok) _textCb(out, _textCtx);
}
#endif

} // namespace affa
