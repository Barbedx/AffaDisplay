#include "AffaDisplayBase.h"
#include <cstring>

namespace affa {
namespace {
constexpr const char* kTag = "AFFA";
}

AffaDisplayBase::AffaDisplayBase(ICanLink& link, IClock& clock, const SyncProfile& profile,
                                 const uint16_t* funcIds, uint8_t funcCount)
    : _link(link), _clock(clock), _profile(profile),
      _funcIds(funcIds), _funcCount(funcCount) {}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool AffaDisplayBase::begin() {
  failAllQueued(Result::Cancelled);

  _tx             = TxState::Idle;
  _selfAckPending = SelfAck::None;
  _sync           = SyncState::Failed;   // written directly: begin() is a reset, not a
                                         // transition, and firing SyncCb from inside
                                         // setup() would surprise every application
  const uint32_t now = _clock.millis();
  _nextSyncMs     = now;                 // the first heartbeat leaves on the first poll()
  _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;
  _ackDeadlineMs  = now;
  _lastCompleted  = kNoTicket;
  _lastEnqueued   = kNoTicket;
  _lastResult     = Result::Ok;
  _lastOverflow   = _link.stats().ringOverflow;
  _begun          = true;

  AFFA_LOGI(kTag, "begin: syncId=0x%03X reply=0x%03X funcs=%u",
            static_cast<unsigned>(_profile.syncId),
            static_cast<unsigned>(_profile.syncReplyId),
            static_cast<unsigned>(_funcCount));
  return true;
}

void AffaDisplayBase::poll() {
  // WRONG TASK: do nothing, and count it. In library-owned mode (src/rtos/) the owned task
  // is the only legal caller; an application that ALSO calls poll() — a leftover line in
  // loop(), most likely — would otherwise drive this FSM concurrently with the owned task
  // and corrupt it in a way that presents as a panel that occasionally draws garbage.
  // Silence would be the worst answer, so Status::foreignPolls surfaces it.
  if (_pollOwnerFn && _pollOwnerFn() != _pollOwner) { ++_foreignPolls; return; }

  // A nested poll() does nothing. This is a guard, not a feature — a callback that wants
  // the library pumped is a callback that should have returned.
  if (_inPoll) return;
  _inPoll = true;

  // The order is the contract. Do not reorder, and do not add an early-out that can skip
  // a step: key delivery latency is bounded by the poll period ALONE precisely because
  // pumpTx() is never entered before pumpRx() has returned.
  pumpRx();
  pumpSync();
  pumpTx();
  onPoll();

  _inPoll = false;
}

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

bool AffaDisplayBase::txFrame(Frame f) {
  // The stamp is applied to our copy, never to the caller's buffer: a panel builder may
  // reuse its frame struct, and a stray fromSelf on a frame we later treat as inbound
  // would be silently ignored by the whole receive path.
  f.fromSelf = true;
  if (!_link.send(f)) {
    ++_txDropCount;
    reportLinkError(LinkErrorKind::TxDropped, _txDropCount);
    return false;
  }
  // A frame the link REFUSED is deliberately not observed: it never existed on the bus.
  observe(f, Direction::Tx);
  return true;
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

void AffaDisplayBase::pumpRx() {
  Frame f;
  uint16_t drained = 0;

  // Drain to empty, not one frame per call: a burst that arrived between two polls is
  // delivered in full on the next one, in arrival order, so a key that arrived behind an
  // ACK is still delivered in that same poll.
  while (_link.recv(f)) {
    ++drained;
    const Direction d = f.fromSelf ? Direction::Tx : Direction::Rx;
    observe(f, d);

    // Echo rule: a self-sent frame is dropped before the auto-ACK, before the ACK matcher
    // AND before the key decoder. Missing any one of the three is a real, observed
    // failure — the library ACKing its own 0x3AF on 0x7AF, and a loopback transfer
    // completing after one frame with a bogus success.
    if (f.fromSelf) continue;

    if (f.id == _profile.syncReplyId) { handleSyncFrame(f); continue; }
    if ((f.id & _profile.replyFlag) != 0) { handleAckFrame(f); continue; }

    // Auto-ACK precedes the panel hook, which is the legacy order: Carminat acknowledged
    // 0x1C1 before it validated the key bytes. Never for the sync ids (that channel has
    // no ACK semantics) and never for an id we transmit on (an inbound frame there is an
    // echo or another node's traffic, and we owe it nothing).
    if (!_passive && !isOurTxId(static_cast<uint16_t>(f.id)) && shouldAutoAck(f))
      sendGenericAck(static_cast<uint16_t>(f.id));

#if AFFA_ENABLE_ISOTP_RX
    pumpText(f);
#endif
    onFrame(f);
  }

  // Only worth a status read when frames actually arrived: an overflow means we were too
  // slow, which means traffic was flowing.
  if (drained) {
    const uint32_t ov = _link.stats().ringOverflow;
    if (ov != _lastOverflow) {
      _lastOverflow = ov;
      reportLinkError(LinkErrorKind::RingOverflow, ov);
      AFFA_LOGW(kTag, "RX ring overflow, total %lu — poll() too slow or ring too small",
                static_cast<unsigned long>(ov));
    }
  }
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

bool AffaDisplayBase::handleSyncFrame(const Frame& f) {
  if (_passive) return true;              // a real radio owns the handshake
  if (f.len < 1) return true;

  if (f.data[0] == kSyncRequestByte0 && f.len >= 2 && f.data[1] == kSyncRequestByte1) {
    for (uint8_t i = 0; i < _profile.helloCount; ++i) {
      Frame h;
      h.id  = _profile.syncId;
      h.len = kPacketLength;
      std::memcpy(h.data, _profile.hello[i], kPacketLength);
      txFrame(h);
    }
    SyncState s = _sync & ~SyncState::Failed;
    // len >= 3 before touching data[2]: short DLCs are real on this channel (the OEM
    // corpus holds 0x3CF with DLC 1 and DLC 2), and the legacy shim read uninitialised
    // stack there, which would latch Start at random. data[2] is the panel's filler on
    // every capture we hold; Start has never actually been observed.
    if (f.len >= 3 && f.data[2] == kSyncStartFlag) s |= SyncState::Start;
    setSync(s, EventKind::SyncChanged);
    return true;
  }

  if (f.data[0] == kSyncPeerAlive) {
    // PeerAlive is a transient the watchdog consumes on the next heartbeat, not a state
    // an application cares about, so it is set directly and fires no event: it toggles
    // at the panel's ~1 Hz ping rate and would be pure noise on the event sink.
    //
    // The legacy code called tick() from here, which emitted an extra heartbeat per ping
    // and re-armed the watchdog from the PEER rather than from the loop. Both are gone:
    // exactly one heartbeat leaves per AFFA_SYNC_INTERVAL_MS, paced by pumpSync().
    _sync |= SyncState::PeerAlive;
    return true;
  }

  AFFA_LOGT(kTag, "unknown sync byte 0x%02X on 0x%03X", static_cast<unsigned>(f.data[0]),
            static_cast<unsigned>(_profile.syncReplyId));
  return true;
}

bool AffaDisplayBase::handleAckFrame(const Frame& f) {
  const uint16_t base =
      static_cast<uint16_t>(f.id & ~static_cast<uint32_t>(_profile.replyFlag));

  // An ACK arriving for a function that is not waiting is dropped silently. That is what
  // stops a stale late ACK from completing the wrong ticket.
  if (_tx != TxState::WaitAck || _qCount == 0 || _queue[0].funcId != base) return true;

  if (f.len >= 1 && f.data[0] == kAckDone) { creditAck(true); return true; }
  if (f.len >= 3 && f.data[0] == kAckPartial0 && f.data[1] == kAckPartial1 &&
      f.data[2] == kAckPartial2) {
    creditAck(false);
    return true;
  }

  AFFA_LOGW(kTag, "ACK on 0x%03X rejected: 0x%02X", static_cast<unsigned>(f.id),
            static_cast<unsigned>(f.len ? f.data[0] : 0));
  finishJob(Result::SendFailed);
  return true;
}

void AffaDisplayBase::sendGenericAck(uint16_t id) {
  Frame a;
  a.id      = static_cast<uint32_t>(id) | _profile.replyFlag;
  a.len     = kPacketLength;
  a.data[0] = kAckDone;
  const uint8_t filler = packetFiller();
  for (uint8_t i = 1; i < kPacketLength; ++i) a.data[i] = filler;
  txFrame(a);
}

bool AffaDisplayBase::isOurTxId(uint16_t id) const {
  if (id == _profile.syncId || id == _profile.syncReplyId) return true;
  return knownFunc(id);
}

bool AffaDisplayBase::knownFunc(uint16_t id) const {
  for (uint8_t i = 0; i < _funcCount; ++i) if (_funcIds[i] == id) return true;
  return false;
}

// ---------------------------------------------------------------------------
// Sync FSM — ONE copy, parameterised by SyncProfile
// ---------------------------------------------------------------------------

void AffaDisplayBase::pumpSync() {
  if (_passive) return;                   // the radio owns the handshake

  const uint32_t now = _clock.millis();
  if (!expired(now, _nextSyncMs)) return;

  // `= now + interval`, never `+= interval`: a caller that stalled for ten seconds must
  // not produce a catch-up burst of ten heartbeats.
  _nextSyncMs = now + AFFA_SYNC_INTERVAL_MS;

  Frame alive;
  alive.id      = _profile.syncId;
  alive.len     = kPacketLength;
  alive.data[0] = _profile.aliveByte;
  alive.data[1] = 0x00;                   // a literal 0x00 in BOTH families, NOT the
                                          // filler: UpdateList sends 79 00 81 81 …
  for (uint8_t i = 2; i < kPacketLength; ++i) alive.data[i] = _profile.filler;
  txFrame(alive);

  if (hasFlag(_sync, SyncState::Failed) || hasFlag(_sync, SyncState::Start)) {
    Frame req;
    req.id      = _profile.syncId;
    req.len     = kPacketLength;
    req.data[0] = _profile.requestByte;
    req.data[1] = _profile.requestArg;
    for (uint8_t i = 2; i < kPacketLength; ++i) req.data[i] = _profile.filler;
    txFrame(req);

    _sync &= ~SyncState::Start;
    // Re-arm the peer window on leaving this branch, so a link that has just been asked
    // to resynchronise gets a full AFFA_PEER_TIMEOUT_MS to answer rather than inheriting
    // whatever was left of the previous one.
    _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;
    // The legacy delay(100) that lived here is DELETED, not replaced. It had no wire
    // meaning: the request is idempotent and the panel answers when it feels like it.
  } else if (hasFlag(_sync, SyncState::PeerAlive)) {
    _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;
    _sync &= ~SyncState::PeerAlive;
  } else if (expired(now, _peerDeadlineMs)) {
    // THIS IS THE DEFECT THIS LIBRARY EXISTS TO MAKE IMPOSSIBLE. Legacy decremented a
    // `static int8_t timeout = SYNC_TIMEOUT` once per tick() CALL, which means "five
    // seconds" only if the caller happens to tick at exactly 1 Hz. From a free-running
    // loop it expired in milliseconds, tore FUNCSREG down and restarted the handshake
    // forever. It is a wall-clock deadline here, and it always will be.
    AFFA_LOGW(kTag, "peer lost: no 0x%02X ping within %d ms",
              static_cast<unsigned>(kSyncPeerAlive), static_cast<int>(AFFA_PEER_TIMEOUT_MS));
    setSync(SyncState::Failed, EventKind::PeerLost);   // every other bit, FuncsReg
                                                       // included, is dropped
    _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;
    // Anything queued was addressed to a panel that has now forgotten us.
    dropUnstarted(Result::Cancelled);
  }
}

void AffaDisplayBase::setSync(SyncState s, EventKind extra) {
  if (s == _sync) return;
  const SyncState prev = _sync;
  _sync = s;                                  // state first, callbacks second
  if (_syncCb) _syncCb(s, _syncCtx);

  Event ev;
  ev.kind      = EventKind::SyncChanged;
  ev.sync.prev = prev;
  ev.sync.now  = s;
  emit(ev);

  if (extra != EventKind::SyncChanged) {
    ev.kind = extra;
    emit(ev);
  }

  // The handshake bits are logged only when they CHANGE. Logging them every pass buried
  // the one transition that mattered under a second of identical lines.
  AFFA_LOGI(kTag, "sync 0x%02X -> 0x%02X", static_cast<unsigned>(prev),
            static_cast<unsigned>(s));
}

// ---------------------------------------------------------------------------
// Transmit FSM
// ---------------------------------------------------------------------------

TxTicket AffaDisplayBase::nextTicket() {
  const TxTicket t = _nextTicket;
  _nextTicket = (_nextTicket == 0xFFFF) ? 1 : static_cast<TxTicket>(_nextTicket + 1);
  return t;
}

bool AffaDisplayBase::registrationQueued() const {
  for (uint8_t i = 0; i < _qCount; ++i)
    if (_queue[i].kind == JobKind::Registration) return true;
  return false;
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

void AffaDisplayBase::pushJob(uint16_t funcId, const uint8_t* d, uint8_t len, JobKind kind,
                              TxTicket t, const TxOptions& opt, uint8_t at) {
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
  j.started    = false;
  j.abandon    = false;
  j.len        = len;
  j.sent       = 0;
  j.frameIndex = 0;
  std::memcpy(j.data, d, len);
}

void AffaDisplayBase::removeJob(uint8_t index) {
  if (index >= _qCount) return;
  for (uint8_t i = index; i + 1 < _qCount; ++i) _queue[i] = _queue[i + 1];
  --_qCount;
}

TxTicket AffaDisplayBase::enqueue(uint16_t funcId, const uint8_t* data, uint8_t len,
                                  TxOptions opt) {
  _lastEnqueued = kNoTicket;

  if (!data || len == 0)          { _lastResult = Result::BadArgument;  return kNoTicket; }
  if (len > AFFA_MAX_PAYLOAD)     { _lastResult = Result::TooLong;      return kNoTicket; }
  if (!knownFunc(funcId))         { _lastResult = Result::UnknownFunc;  return kNoTicket; }
  if (!_link.isLive())            { _lastResult = Result::LinkDown;     return kNoTicket; }
  if (!_passive && hasFlag(_sync, SyncState::Failed)) {
    _lastResult = Result::NoSync;
    return kNoTicket;
  }

  // Lazy function registration, byte-identical to the legacy wire order: the first send
  // after a resync walks the WHOLE function table in declaration order and sends a 1-byte
  // 0x70 to each, then latches FUNCSREG, then the payload.
  const bool needReg = !_passive && !hasFlag(_sync, SyncState::FuncsReg) &&
                       !registrationQueued();

  int ci = -1;
  if (opt.coalesce) ci = findCoalescable(funcId, opt.slot);

  const uint8_t newSlots =
      static_cast<uint8_t>((ci >= 0 ? 0 : 1) + (needReg ? _funcCount : 0));
  if (_qCount + newSlots > AFFA_TX_QUEUE_DEPTH) {
    _lastResult = Result::QueueFull;
    return kNoTicket;
  }

  if (needReg) {
    static const uint8_t kReg[1] = { kRegisterByte };
    TxOptions ro;                       // slot None, Normal, coalesce false: nothing may
    ro.coalesce = false;                // replace a registration and nothing, not even
    for (uint8_t i = 0; i < _funcCount; ++i)   // Urgent, may overtake one
      pushJob(_funcIds[i], kReg, 1, JobKind::Registration, kNoTicket, ro, _qCount);
  }

  const TxTicket t = nextTicket();

  if (ci >= 0) {
    // Latest value wins: replace the payload in place, keeping the superseded job's queue
    // position so ordering relative to OTHER slots is what the application asked for.
    const TxTicket old = _queue[ci].ticket;
    TxJob& j = _queue[ci];
    std::memcpy(j.data, data, len);
    j.len      = len;
    j.ticket   = t;
    j.coalesce = opt.coalesce;

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

    _lastEnqueued = t;
    _lastResult   = Result::Ok;
    completeTicket(old, Result::Aborted);   // state first, callback second
    return t;
  }

  pushJob(funcId, data, len, JobKind::Payload, t, opt, insertIndexFor(opt.priority));
  _lastEnqueued = t;
  _lastResult   = Result::Ok;
  return t;
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
  if (_qCount == 0) { _tx = TxState::Idle; return; }
  if (!_link.isLive()) { finishJob(Result::LinkDown); return; }

  TxJob& job = _queue[0];

  // Frame 0 carries EIGHT raw payload bytes with no PCI added by the transport — the 0x10
  // at the head of a screen payload is payload byte 0, built by the caller. Continuation
  // frames carry seven, prefixed with 0x20 | (num & 0x0F).
  Frame f;
  f.id  = job.funcId;
  f.len = kPacketLength;
  uint8_t i = 0;
  if (job.frameIndex > 0) f.data[i++] = isoTpCf(job.frameIndex);
  while (i < kPacketLength && job.sent < job.len) f.data[i++] = job.data[job.sent++];
  const bool more = (job.sent < job.len);
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
  job.started = true;         // from here this job is no longer preemptable

  if (!txFrame(f)) { finishJob(Result::SendFailed); return; }

  _ackDeadlineMs = now + AFFA_ACK_TIMEOUT_MS;
  _tx            = TxState::WaitAck;

  // Bench self-ACK stays in WaitAck for exactly one pass so the frame sequence is
  // identical to a real send.
  if (_selfAck) _selfAckPending = more ? SelfAck::Partial : SelfAck::Done;
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

void AffaDisplayBase::finishJob(Result r) {
  if (_qCount == 0) { _tx = TxState::Idle; return; }

  TxJob& job = _queue[0];
  const JobKind  kind   = job.kind;
  const TxTicket ticket = job.ticket;
  const uint16_t funcId = job.funcId;   // read before removeJob() shifts the array under
                                        // `job`, which is a reference into it
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
      if (!registrationQueued()) setSync(_sync | SyncState::FuncsReg, EventKind::Registered);
    } else {
      // The payload behind it completes with the registration's own failure, exactly as
      // the legacy affa3_send propagated it to its caller.
      AFFA_LOGW(kTag, "registration of 0x%03X failed (%u)",
                static_cast<unsigned>(funcId), static_cast<unsigned>(r));
      failAllQueued(r);
    }
    return;                        // registration jobs carry kNoTicket and are invisible
  }

  completeTicket(ticket, r);
}

void AffaDisplayBase::completeTicket(TxTicket t, Result r) {
  if (t == kNoTicket) return;
  _lastCompleted = t;
  _lastResult    = r;
  if (_cplCb) _cplCb(t, r, _cplCtx);

  Event ev;
  ev.kind      = EventKind::TxComplete;
  ev.tx.ticket = t;
  ev.tx.result = r;
  emit(ev);
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

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

void AffaDisplayBase::decodeKey(uint16_t raw, Key& out, KeyEdge& edge) {
  if (raw == kKeyRollUp || raw == kKeyRollDown) {
    // The encoder detents are EXEMPT from hold masking. 0x40 is simultaneously RollDown's
    // direction bit and half the hold mask, so masking 0x0141 would rewrite it to 0x0101
    // and every wheel-down detent would be reported as a wheel-up.
    out  = static_cast<Key>(raw);
    edge = KeyEdge::Click;
    return;
  }
  edge = ((raw & 0x00FFu) & kKeyHoldMask) ? KeyEdge::Hold : KeyEdge::Click;
  // 0xFF3F, written out, not `~kKeyHoldMask`: the latter is correct only by accident of
  // integer promotion (~(uint8_t)0xC0 is 0xFFFFFF3F, so the high byte survives).
  out  = static_cast<Key>(raw & kKeyCodeMask);
}

bool AffaDisplayBase::decodeKeyFrame(const Frame& f, Key& out, KeyEdge& edge) {
  if (f.len < 4) return false;
  if (f.data[0] != kKeyFrameByte0 || f.data[1] != kKeyFrameByte1) return false;
  decodeKey(static_cast<uint16_t>((f.data[2] << 8) | f.data[3]), out, edge);
  return true;
}

void AffaDisplayBase::routeKey(Key k, KeyEdge e) {
  bool consumed = false;

#if AFFA_ENABLE_MENU
  if (_hotkeyOn && k == _hotkey && e == _hotkeyEdge && !menuOpen()) consumed = openMenu();
  if (!consumed) consumed = routeKeyToMenu(k, e);
#endif

  // Keys the menu did not consume fall through to the application.
  if (!consumed && _keyCb) _keyCb(k, e, _keyCtx);

  // The event fires whether or not the menu consumed the key, and always AFTER the menu
  // has had it, so ev.key is what arrived rather than what was left over.
  Event ev;
  ev.kind     = EventKind::Key;
  ev.key.key  = k;
  ev.key.edge = e;
  emit(ev);
}

Result AffaDisplayBase::transmitKey(Key k, KeyEdge e) {
  const uint16_t id = keyTxId();
  if (id == 0) return Result::NotSupported;

  const uint16_t raw = static_cast<uint16_t>(k);
  if (e == KeyEdge::Hold && (raw == kKeyRollUp || raw == kKeyRollDown)) {
    // A refusal, not a downgrade. 0x0101|0xC0 and 0x0141|0xC0 are both 0x01C1, so a held
    // detent has no distinguishable wire representation; transmitting the click form
    // instead would produce a fine step where the caller asked for a coarse one, which is
    // a wrong screen the caller cannot detect.
    return Result::NotSupported;
  }
  if (!_link.isLive()) return Result::LinkDown;

  Frame f;
  f.id      = id;
  f.len     = kPacketLength;
  f.data[0] = kKeyFrameByte0;
  f.data[1] = kKeyFrameByte1;
  f.data[2] = static_cast<uint8_t>(raw >> 8);
  f.data[3] = static_cast<uint8_t>(raw & 0xFF);
  if (e == KeyEdge::Hold) f.data[3] |= kKeyHoldMask;
  // Bytes 4..7 stay literal zero and are NOT packetFiller(): that is what the capture of
  // our own emulated key frame shows.

  return txFrame(f) ? Result::Ok : Result::SendFailed;
}

Result AffaDisplayBase::pressKey(Key k, KeyEdge e, KeySource src) {
  Result wire = Result::Ok;

  // The Wire half is attempted FIRST and its failure is returned even though the Local
  // half has already happened. That is the honest answer: the caller asked for both.
  if (hasSource(src, KeySource::Wire)) wire = transmitKey(k, e);

  if (hasSource(src, KeySource::Local)) {
    bool observable = (_keyCb != nullptr) || (_evCb != nullptr);
#if AFFA_ENABLE_MENU
    observable = observable || _hotkeyOn || menuOpen();
#endif
    routeKey(k, e);
    if (!observable && wire == Result::Ok) return Result::NotSupported;
  }
  return wire;
}

Result AffaDisplayBase::nav(NavCommand c, KeySource src) {
#if !AFFA_ENABLE_MENU
  (void)c; (void)src;
  return Result::NotSupported;
#else
  // Increase/Decrease are a coarse step on a held detent, and a held detent has no wire
  // representation at all — which is also the reason input has to be a seam rather than a
  // source: the coarse-step feature exists in the menu and the panel physically cannot
  // reach it.
  if (hasSource(src, KeySource::Wire) &&
      (c == NavCommand::Increase || c == NavCommand::Decrease))
    return Result::NotSupported;

  Key     k = Key::Load;
  KeyEdge e = KeyEdge::Click;
  switch (c) {
    case NavCommand::Open:     k = Key::Load;     e = KeyEdge::Hold;  break;
    case NavCommand::Back:     k = Key::Load;     e = KeyEdge::Hold;  break;
    case NavCommand::Select:   k = Key::Load;     e = KeyEdge::Click; break;
    case NavCommand::Next:     k = Key::RollDown; e = KeyEdge::Click; break;
    case NavCommand::Prev:     k = Key::RollUp;   e = KeyEdge::Click; break;
    case NavCommand::Increase: k = Key::RollDown; e = KeyEdge::Hold;  break;
    case NavCommand::Decrease: k = Key::RollUp;   e = KeyEdge::Hold;  break;
  }

  if (c == NavCommand::Open) {
    // Open is an INTENT, not a gesture: it opens the menu regardless of the hotkey
    // setting. Clearing the hotkey exists precisely so that this becomes the only way in.
    Result wire = Result::Ok;
    if (hasSource(src, KeySource::Wire)) wire = transmitKey(k, e);
    if (hasSource(src, KeySource::Local) && !openMenu()) return Result::NotSupported;
    return wire;
  }

  return pressKey(k, e, src);
#endif
}

#if AFFA_ENABLE_MENU
void AffaDisplayBase::setMenuHotkey(Key k, KeyEdge e) {
  _hotkey = k; _hotkeyEdge = e; _hotkeyOn = true;
}
void AffaDisplayBase::clearMenuHotkey() { _hotkeyOn = false; }
bool AffaDisplayBase::menuHotkey(Key& k, KeyEdge& e) const {
  if (!_hotkeyOn) return false;
  k = _hotkey; e = _hotkeyEdge;
  return true;
}
#endif

// ---------------------------------------------------------------------------
// Options and observation
// ---------------------------------------------------------------------------

void AffaDisplayBase::setPollOwner(void* owner, TaskIdFn fn) {
  _pollOwner   = owner;
  _pollOwnerFn = fn;
}
uint32_t AffaDisplayBase::foreignPolls() const { return _foreignPolls; }
bool     AffaDisplayBase::begun() const { return _begun; }

void AffaDisplayBase::setPassive(bool on) { _passive = on; }
bool AffaDisplayBase::passive() const { return _passive; }
void AffaDisplayBase::setSelfAck(bool on) { _selfAck = on; }

SyncState AffaDisplayBase::syncState() const { return _sync; }
bool      AffaDisplayBase::synced() const { return !hasFlag(_sync, SyncState::Failed); }
bool      AffaDisplayBase::registered() const { return hasFlag(_sync, SyncState::FuncsReg); }
bool      AffaDisplayBase::busy() const { return _qCount > 0; }
Result    AffaDisplayBase::lastResult() const { return _lastResult; }
TxTicket  AffaDisplayBase::lastTicket() const { return _lastCompleted; }
TxTicket  AffaDisplayBase::lastEnqueued() const { return _lastEnqueued; }
uint8_t   AffaDisplayBase::queued() const {
  return (_qCount > 0) ? static_cast<uint8_t>(_qCount - 1) : 0;
}
Stats     AffaDisplayBase::stats() const { return _link.stats(); }

// ---------------------------------------------------------------------------
// Rendering: one default body, and it is NOT a silent no-op
// ---------------------------------------------------------------------------
// The legacy IDisplay gave these silently no-op bodies returning AffaError::NoError, so
// calling one on a panel that could not do it looked exactly like success. Ask
// supports(Feature) before you call, and check the Result when you do.

Result AffaDisplayBase::setText(const char*, uint8_t) { return Result::NotSupported; }
Result AffaDisplayBase::setTime(const char*) { return Result::NotSupported; }
Result AffaDisplayBase::setPower(bool) { return Result::NotSupported; }
Result AffaDisplayBase::showMenu(const char*, const char*, const char*, uint8_t) {
  return Result::NotSupported;
}
Result AffaDisplayBase::highlightItem(uint8_t) { return Result::NotSupported; }
Result AffaDisplayBase::showPopupText(const char*, uint8_t, uint8_t, uint8_t) {
  return Result::NotSupported;
}
Result AffaDisplayBase::hidePopup() { return Result::NotSupported; }
Result AffaDisplayBase::showFullscreenText(const char*, const char*, const char*) {
  return Result::NotSupported;
}
Result AffaDisplayBase::hideFullscreenText() { return Result::NotSupported; }
Result AffaDisplayBase::showConfirmBox(const char*, const char*, const char*) {
  return Result::NotSupported;
}
Result AffaDisplayBase::showInfoPopup(const char*, const char*, const char*) {
  return Result::NotSupported;
}
Result AffaDisplayBase::hideInfoPopup() { return Result::NotSupported; }

} // namespace affa
