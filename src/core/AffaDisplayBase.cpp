#include "AffaDisplayBase.h"
#include <cstring>

namespace affa {
namespace {
constexpr const char* kTag = "AFFA";
constexpr uint8_t kBootstrapBusyRetries = 1;
// A lost 5C1 costs the whole session, so this one is retried harder than ordinary control
// traffic — and at most two may ever be owed at once (§4.2, one ACK per received frame).
constexpr uint8_t kGenericAckBusyRetries = 3;
constexpr uint8_t kGenericAckMaxOwed     = 2;
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
  advanceSessionEpoch();

  _tx             = TxState::Idle;
  _selfAckPending = SelfAck::None;
  _sync           = SyncState::Failed;   // written directly: begin() is a reset, not a
                                         // transition, and firing SyncCb from inside
                                         // setup() would surprise every application
  const uint32_t now = _clock.millis();
  _nextSyncMs     = now;                 // proactive profiles may heartbeat on the first poll;
                                         // panel-initiated profiles remain silent until auth
  _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;
  _ackDeadlineMs  = now;
  _nextHelloMs    = now;                 // pacing floors, not schedules: a re-begin() must
  _nextPongMs     = now;                 // not inherit a stale deadline across a clock wrap
  _helloIndex             = 0;
  _nextPayloadMs          = now;
  _panelObserved          = false;       // profiles that wait for their panel start silent
  _syncRequestObserved    = false;       // a bare 69 cannot open a Carminat session
  _authRequestObserved    = false;       // `01` bootstrap cannot authorize app traffic
  _authHelloPending       = false;
  _helloPending           = false;
  _unauthControlPending   = false;
  _unauthControlIssued    = false;
  _unauthControlSpent     = false;
  _unauthControlStage     = BootstrapStage::None;
  _unauthControlBusyRetries = 0;
  _nextUnauthControlMs    = now;
  // One full interval of politeness before we announce: give a panel that is merely slow
  // to boot the chance to speak first, exactly as it does in the captures.
  _nextAnnounceMs         = now + _profile.announceWhenSilentMs;
  _peerChannelSeen        = false;
  _genericAckPending      = false;
  _genericAckId           = 0;
  _genericAckBusyRetries  = 0;
  _genericAckOwed         = 0;
  _nextGenericAckMs       = now;
  _cachedControl           = CachedControl{};
  _lastCompleted  = kNoTicket;
  _lastEnqueued   = kNoTicket;
  _lastResult     = Result::Ok;
  _lastOverflow   = _link.stats().ringOverflow;
  _lastRxMs       = now;                 // the deaf watchdog measures from here, and stays
  _rxHeard        = false;               // disarmed until a frame actually arrives
  _rxStalled      = false;
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
  pumpLink();
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

    // A frame off the wire feeds the RUNNING-but-deaf watchdog: re-arm it and release the
    // latch. Echoes deliberately do not count — a link that hears only itself is deaf.
    _lastRxMs  = _clock.millis();
    _rxHeard   = true;
    _rxStalled = false;

    if (f.id == _profile.syncReplyId) { handleSyncFrame(f); continue; }
    if ((f.id & _profile.replyFlag) != 0) { handleAckFrame(f); continue; }

    // Auto-ACK precedes the panel hook, which is the legacy order: Carminat acknowledged
    // 0x1C1 before it validated the key bytes. Never for the sync ids (that channel has
    // no ACK semantics) and never for an id we transmit on (an inbound frame there is an
    // echo or another node's traffic, and we owe it nothing).
    // THE CONTROL ACK IS A REFLEX, NOT A PRIVILEGE. The panel's own channel registration
    // (`1C1 70 -> 5C1 74`) is answered in 0.25-0.48 ms in every OEM capture, and it lands
    // BETWEEN B0#1 and B0#2 — before any authorization phase has finished. It was
    // previously gated on `_syncRequestObserved`, which is safe only because the captured
    // panels all happen to lead with `61 11`; a panel that leads with `1C1 70` would go
    // unanswered for ever. Scope comes from shouldAutoAck() (exactly 0x1C1) and from
    // isOurTxId(), not from the handshake phase. This ACK is deliberately separate from
    // linkReady(): our registration, power, text and time stay locked behind the opening.
    if (!_passive && !isOurTxId(static_cast<uint16_t>(f.id)) && shouldAutoAck(f)) {
      // The display opening its own channel is the gate for opening ours. See pumpSync().
      if (f.len >= 1 && f.data[0] == kRegisterByte) _peerChannelSeen = true;
      sendGenericAck(static_cast<uint16_t>(f.id));
    }

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
    // Carminat keeps two stages separate. Every full `61 11 xx` gets the proven hello
    // response, but only `00` releases registration and payload traffic. `01` asks for
    // one control bootstrap; it never becomes an authorization shortcut.
    if (_profile.requireAuthRequest) {
      // A short 61 11 has no byte 2 on the wire. Do not read stale storage or answer it.
      if (f.len < 3) return true;

      const uint32_t now = _clock.millis();
      const bool firstSyncRequest = !_syncRequestObserved;
      _panelObserved = true;
      _syncRequestObserved = true;
      _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;

      // Two doors into the same room. The profile's good byte is one; the other is a START
      // request that arrives after our own BA, which the OEM captures show being answered
      // with the identical B0 burst. `_unauthControlIssued` is precisely "BA has gone out",
      // so a `01` on an empty bus still gets only discovery. See SyncProfile.
      const bool bootstrapAnswered = _profile.helloAfterBootstrapRequest &&
                                     _unauthControlIssued &&
                                     f.data[2] == kSyncStartFlag;
      if (f.data[2] == _profile.authRequestByte2 || bootstrapAnswered) {
        // ANY complete `61 11 xx` that arrives once we already hold registrations says the
        // panel forgot us: "your registration is void". In every OEM capture a registered
        // display stops sending `61 11` ENTIRELY — so one arriving here is real loss, never
        // chatter, and the value of xx is irrelevant to that.
        //
        // THIS USED TO READ `bootstrapAnswered && ...`, which restricted the teardown to
        // `61 11 01` and let `61 11 00` fall straight through: needsHelloBeforeAuth then
        // computed false and NOTHING happened. Measured on the bench — the panel sent
        // `61 11 00` twenty-six times, then fifteen more, while we cheerfully kept pushing
        // fullscreen rows at it. A deauthorized panel asking to re-open must stop the
        // application traffic, not be talked over.
        if (hasFlag(_sync, SyncState::FuncsReg)) {
          AFFA_LOGW(kTag, "61 11 %02X while registered - the panel voided us; reopening",
                    static_cast<unsigned>(f.data[2]));
          setSync(SyncState::Failed, EventKind::SyncChanged);
          invalidateInFlightForSession(now);
          _authRequestObserved = false;
          _authHelloPending    = false;
          _helloPending        = false;
          _helloIndex          = 0;
          _nextHelloMs         = now;
        }
        // The good request may arrive while the previous hello burst is still inside its
        // rate floor. Keep application TX closed until the hello that answers THIS phase
        // has actually been emitted.
        // Our BA has to be on the wire before the burst means anything. The first request
        // arms it and is answered with nothing else; the panel asks again on its own ~104 ms
        // timer and THAT one opens the session. See SyncProfile::helloRequiresAnnounce.
        // FAIL OPEN, NEVER WEDGE. If the announce is still pending or can still be armed,
        // hold the burst back for it. But once the one-shot is SPENT without ever reaching
        // the wire — a Rejected BA, or a busy budget burned through — `_unauthControlIssued`
        // stays false for ever, and gating on it alone would refuse the hello on every
        // subsequent request while nothing short of a link reset could clear it. A session
        // opened without the announce is merely less faithful; a session that can never open
        // is broken, so the unreachable case falls through to the ordinary path.
        if (_profile.helloRequiresAnnounce && !_unauthControlIssued) {
          const bool announceStillPossible =
              _unauthControlPending || !_unauthControlSpent;
          armUnauthControl(now);
          if (announceStillPossible) return true;
          AFFA_LOGW(kTag, "announce unreachable; opening without it rather than stalling");
        }
        const bool needsHelloBeforeAuth =
            !_authRequestObserved || _authHelloPending || hasFlag(_sync, SyncState::Failed);
        _authRequestObserved = true;
        if (needsHelloBeforeAuth) _authHelloPending = true;
        // Do not cancel a discovery pair armed by a preceding 01/69 in this same RX
        // drain. The display may send 00 immediately after it; that still owns exactly
        // one B9 -> BA transaction, never a new retry stream.
        if (firstSyncRequest || needsHelloBeforeAuth)
          _nextSyncMs = now + syncIntervalMs();
        if (needsHelloBeforeAuth) queueHello(now);
        return true;
      }

      // A non-good request tears down any old registration. We still reply with hello:
      // that is the legacy Carminat wire sequence, and it lets the panel progress to 00.
      const bool wasAuthorized = _authRequestObserved || _authHelloPending ||
                                 !hasFlag(_sync, SyncState::Failed);
      if (wasAuthorized) {
        _authRequestObserved = false;
        _authHelloPending = false;
        _helloPending = false;
        _helloIndex = 0;
        _nextHelloMs = now;
        _genericAckPending = false;
        _genericAckBusyRetries = 0;
        setSync(SyncState::Failed, EventKind::SyncChanged);
        invalidateInFlightForSession(now);
        dropRegistrations();
        for (uint8_t i = 0; i < _qCount; ++i) {
          if ((_queue[i].kind == JobKind::Payload || _queue[i].kind == JobKind::Reassert) &&
              !_queue[i].started)
            _queue[i].holdUntilMs = now + AFFA_TX_HOLD_MS;
        }
        // A new display START after an authorized session earns one fresh discovery
        // transaction. A duplicate START while already failed does not.
        _unauthControlPending = false;
        _unauthControlIssued = false;
        _unauthControlSpent = false;
        _unauthControlStage = BootstrapStage::None;
        _unauthControlBusyRetries = 0;
      }
      if (_profile.helloOnNonAuthRequest) queueHello(now);

      // Only the observed START spelling gets the single control pair. Unknown xx values
      // are hello-only; retransmitted 01 frames cannot rebuild a BA-per-second stream.
      if (f.data[2] == kSyncStartFlag && _profile.oneShotResyncOnStart)
        armUnauthControl(now);
      return true;
    }

    const uint32_t now = _clock.millis();
    const bool firstSyncRequest = !_syncRequestObserved;
    _panelObserved = true;
    _syncRequestObserved = true;
    _authRequestObserved = true;          // retained for generic profiles; they have no gate
    // Do not append a B9 to the three-frame hello response.  The first paced heartbeat is
    // a full interval later; this keeps the opening exchange exactly hello -> registration.
    if (firstSyncRequest) _nextSyncMs = now + syncIntervalMs();
    // A 61 11 is as conclusive a panel signal as a 69 for session startup.  Re-arm the
    // wall-clock watchdog here so a display that takes a moment to start its 1 Hz pings
    // cannot be declared lost immediately after it just asked us to authorize.
    _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;
    // THE ANSWER IS PACED, THE STATE IS NOT. An unacknowledged panel repeats this request
    // back to back at line rate — 1472 frames/s measured on the bench — and answering each
    // one with helloCount frames is ~4400 transmit attempts per second, far beyond what the
    // TX queue can drain between the panel's line-rate bursts. That fills the
    // TWAI transmit queue permanently, blocks the poll task inside sendFrame(), and starves
    // every render behind it, which is what a panel frozen in half-finished authorisation
    // actually looks like from the outside. See AFFA_HELLO_MIN_MS.
    queueHello(now);
    SyncState s = _sync & ~SyncState::Failed;

    // For profiles without an explicit auth-byte gate, data[2] == 0x01 says registration
    // is void, and under-reacting to it is how a link deadlocks until somebody power-cycles
    // the rig. Carminat rejects 01 above; it never reaches this compatibility path.
    //
    // The legacy compatibility path latched Start here — which re-arms the 0xBA sync
    // request — and LEAVES FUNCSREG SET. So we go on believing we are registered, the
    // lazy registration in pumpTx() never runs again, and the panel goes on asking. At line
    // rate: 1472 frames/s measured on the bench 2026-07-29, for hours, which is what a
    // panel that has given up on us looks like.
    //
    // So Start also drops the registrations, exactly as a peer-alive timeout does. The next
    // render re-runs the 0x70 probe over every funcId from index 0, which is the thing the
    // panel is actually asking for and is a frame we can send. See docs/PROTOCOL.md §3.3.
    //
    // len >= 3 before touching data[2]: short DLCs are real on this channel — the OEM corpus
    // holds 0x3CF at DLC 1 and DLC 2 — and the legacy shim read uninitialised stack there,
    // latching Start at random.
    if (f.len >= 3 && f.data[2] == kSyncStartFlag) {
      // GUARDED ON FuncsReg, NOT on the edge of Start, and the difference matters.
      //
      // Start is cleared once per AFFA_SYNC_INTERVAL_MS by pumpSync(), so an edge-triggered
      // drop would still re-fire about once a second — and registration is not free. It is
      // a 0x70 probe per funcId, each waiting on an ACK, and a funcId the panel does not
      // answer costs a full AFFA_ACK_TIMEOUT_MS. Re-dropping every second would cut across
      // the very pass it is trying to provoke, which is a livelock, not a fix.
      //
      // So drop only what there is to drop: if we still BELIEVE we are registered while the
      // panel says we are not, that belief is the bug and it goes. If FuncsReg is already
      // clear, a registration is either pending or in flight and must be left alone.
      if (hasFlag(_sync, SyncState::FuncsReg)) {
        AFFA_LOGW(kTag, "panel reports registration void (61 11 01) — re-registering");
        dropRegistrations();
        s &= ~SyncState::FuncsReg;
        // The re-registration this provokes takes time the held renders were never
        // budgeted for. Without a fresh window, a payload that aged politely while
        // REGISTERED would be given up as NoSync by pumpTx()'s hold check before the
        // re-registration it now depends on has even been spliced. Same re-arm as the
        // peer-timeout and post-recovery paths — the three places a registration is
        // voided must age the queue identically.
        for (uint8_t i = 0; i < _qCount; ++i) {
          if ((_queue[i].kind == JobKind::Payload || _queue[i].kind == JobKind::Reassert) &&
              !_queue[i].started)
            _queue[i].holdUntilMs = now + AFFA_TX_HOLD_MS;
        }
      }
      s |= SyncState::Start;
    }
    setSync(s, EventKind::SyncChanged);
    return true;
  }

  if (f.data[0] == kSyncPeerAlive) {
    const uint32_t now = _clock.millis();
    const bool firstPanelFrame = !_panelObserved;
    _panelObserved = true;

    // The Carminat panel's `69` is not authorization. It may nevertheless be the very
    // first display message in the real capture, so profiles that opt in get one bounded
    // B9 -> BA discovery transaction here. A bare ping still cannot schedule B0, register
    // functions, or release a render; only the later good 61 11 request can do that.
    if (_profile.requireAuthRequest && !_syncRequestObserved) {
      if (_profile.oneShotResyncOnPeerAlive) armUnauthControl(now);
      return true;
    }

    if (firstPanelFrame) _nextSyncMs = now + syncIntervalMs();
    _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;
    // PeerAlive is a transient the watchdog consumes on the next heartbeat, not a state
    // an application cares about, so it is set directly and fires no event: it toggles
    // at the panel's ~1 Hz ping rate and would be pure noise on the event sink.
    _sync |= SyncState::PeerAlive;

    // ANSWER THE PING, where the profile says the family does. The legacy Carminat driver
    // called tick() from exactly this branch, so its B9 followed the panel's 69 within
    // milliseconds — a pong, on the wire, whether or not the panel reads it as one. That
    // driver is the one proven against a real panel, and this is the LAST wire-visible
    // difference between it and this library (MegaOpen/docs/DISPLAY-INIT-SPEC.md §5).
    //
    // What deliberately does NOT come back from the legacy path: the watchdog stays armed
    // from OUR loop (pumpSync consumes PeerAlive), and no BA rides along. Only the alive
    // frame, and only at AFFA_PING_REPLY_MIN_MS — an unacknowledged panel repeats this
    // ping at line rate, and a reply per copy would be the hello storm's twin.
    if (_profile.replyToPing && !_unauthControlPending) {
      if (expired(now, _nextPongMs)) {
        // Rate-limit unsuccessful offers too: a no-ACK listener can repeat 69 at line
        // rate while the local TX queue is full. The next panel ping retries after the
        // same bounded floor, but a Busy offer is never claimed as an emitted pong.
        const TxDisposition pong = sendAlive();
        _nextPongMs = now + (pong == TxDisposition::Accepted
                                  ? AFFA_PING_REPLY_MIN_MS
                                  : AFFA_TX_RETRY_MS);
        // A pong is the same B9 as the paced heartbeat. Consume the cadence when it was
        // accepted so a 69 arriving on the heartbeat boundary cannot yield B9 twice in
        // one poll pass.
        if (pong == TxDisposition::Accepted) _nextSyncMs = now + syncIntervalMs();
      }
    }
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

void AffaDisplayBase::sendGenericAck(uint16_t id) {
  const uint32_t now = _clock.millis();
  // ONE ACK PER FRAME. The panel acknowledges each `1C1` individually — 12/12 in the OEM
  // captures — so a second frame arriving while the first ACK is still stuck behind a busy
  // controller owes a second `74`, not nothing. The debt is capped rather than unbounded: a
  // storm of the same frame must not become a control-frame stream of our own.
  if (_genericAckPending && _genericAckId == id) {
    if (_genericAckOwed < kGenericAckMaxOwed) ++_genericAckOwed;
    return;
  }

  Frame a;
  a.id      = static_cast<uint32_t>(id) | _profile.replyFlag;
  a.len     = kPacketLength;
  a.data[0] = kAckDone;
  const uint8_t filler = packetFiller();
  for (uint8_t i = 1; i < kPacketLength; ++i) a.data[i] = filler;
  const TxDisposition sent = txFrame(a);
  if (sent == TxDisposition::Accepted) {
    _genericAckPending = false;
    _genericAckOwed = 0;
    _genericAckBusyRetries = 0;
    return;
  }

  // Only a locally full TX queue is a reason to retry this exact acknowledgement. A hard
  // rejection means the controller/bus is already down and pumpLink() will invalidate the
  // session; replaying an old 5C1 after that reset would be worse than dropping it.
  if (sent == TxDisposition::Busy) {
    _genericAckPending = true;
    _genericAckId = id;
    _genericAckOwed = 1;
    _genericAckBusyRetries = 0;
    _nextGenericAckMs = now + AFFA_TX_RETRY_MS;
  } else {
    _genericAckPending = false;
    _genericAckOwed = 0;
  }
}

void AffaDisplayBase::pumpGenericAck(uint32_t now) {
  if (!_genericAckPending || !expired(now, _nextGenericAckMs)) return;

  Frame a;
  a.id      = static_cast<uint32_t>(_genericAckId) | _profile.replyFlag;
  a.len     = kPacketLength;
  a.data[0] = kAckDone;
  const uint8_t filler = packetFiller();
  for (uint8_t i = 1; i < kPacketLength; ++i) a.data[i] = filler;

  const TxDisposition sent = txFrame(a);
  if (sent == TxDisposition::Accepted) {
    _genericAckBusyRetries = 0;
    if (_genericAckOwed > 0) --_genericAckOwed;
    if (_genericAckOwed == 0) {
      _genericAckPending = false;
      return;
    }
    _nextGenericAckMs = now;          // the next owed 74 goes out on the following poll
    return;
  }
  if (sent == TxDisposition::Busy && _genericAckBusyRetries < kGenericAckBusyRetries) {
    ++_genericAckBusyRetries;
    _nextGenericAckMs = now + AFFA_TX_RETRY_MS;
    return;
  }

  // Giving up on a 5C1 costs the whole session, so say so. Never turn a persistently busy
  // controller into an unbounded control-frame stream, but never drop this one silently.
  AFFA_LOGW(kTag, "5C1 control ACK for 0x%03X abandoned after %u attempts",
            static_cast<unsigned>(_genericAckId),
            static_cast<unsigned>(_genericAckBusyRetries) + 1u);
  _genericAckPending = false;
  _genericAckOwed = 0;
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
// Link recovery FSM — the last thing an application had to own
// ---------------------------------------------------------------------------

AffaDisplayBase::LinkHealth AffaDisplayBase::linkHealth() const { return _health; }

void AffaDisplayBase::pumpLink() {
  const uint32_t now = _clock.millis();

  bool down = !_link.healthy();

#if AFFA_RX_STALL_MS
  // THE RUNNING-BUT-DEAF CASE, which healthy() cannot see: state RUNNING, no bus-off, and
  // reception simply stopped (bench, 2026-07-29 — rxErr pinned 129, rxFrames frozen, three
  // days and a power cycle). isLive(), not healthy(), on purpose here: an application that
  // shut the TX gate or dropped to listen-only has chosen one-sidedness, and silence is
  // then expected, not a fault. One latch per silence; pumpRx() re-arms on a real frame.
  const bool live = _link.isLive();
  if (live && !_wasLive) {
    // Eligibility rising edge — the gate just opened, or listen-only just ended. Whatever
    // silence accumulated while we were deliberately one-sided was the application's
    // choice, not evidence of deafness: the watchdog measures from HERE, a full fresh
    // window, instead of latching instantly off a stale timestamp.
    _lastRxMs = now;
  }
  _wasLive = live;
  if (_rxStalled && !live) {
    // The application chose one-sidedness AFTER the latch. The silence is theirs now, and
    // holding the latch would keep the backoff force-restarting the driver underneath a
    // deliberately gated app — the exact thing the healthy()/isLive() split forbids.
    _rxStalled = false;
  }
  if (!down && _rxHeard && live && expired(now, _lastRxMs + AFFA_RX_STALL_MS)) {
    _rxStalled = true;
    _rxHeard   = false;
    AFFA_LOGW(kTag, "controller RUNNING but nothing received for %lu ms — treating as down",
              static_cast<unsigned long>(now - _lastRxMs));
  }
  down = down || _rxStalled;
#endif

  // healthy(), NOT isLive(), for the controller-down half: the difference is the software
  // TX gate, and an application that shut it on purpose — mid-OTA, or running the
  // is-it-us-or-the-bus test — must not have the driver torn down underneath it as a
  // reward. See ICanLink::healthy().
  if (!down) {
    if (_health.downSince) {                       // it came back — the normal case, and it
      _health.downMs += now - _health.downSince;   // is the one that must be cheap
      _health.downSince = 0;
      _health.nextTryMs = 0;
      _recoverBackoffMs = 0;
      AFFA_LOGI(kTag, "link live again (flap %lu, %lu ms down in total)",
                static_cast<unsigned long>(_health.flaps),
                static_cast<unsigned long>(_health.downMs));
    }
    return;
  }

  // ---- the link is down -----------------------------------------------------
  if (_health.downSince == 0) {
    _health.downSince = now;
    ++_health.flaps;
    // The FIRST attempt is a full AFFA_LINK_RECOVER_MS away, not immediate. Direct TWAI
    // needs a short bus-free interval after BUS_OFF before recover() can restart it; a
    // second lifecycle transition during that interval is how a clean recovery becomes a
    // wedge. Let the driver finish its cheap path first.
    _recoverBackoffMs = AFFA_LINK_RECOVER_MS;
    _health.nextTryMs = now + AFFA_LINK_RECOVER_MS;
  }

#if AFFA_LINK_RECOVER_MS
  if (!expired(now, _health.nextTryMs)) return;

  // Arm the NEXT deadline before attempting, not after. recover() may block for the whole
  // of a driver restart, and computing the next deadline from a post-call clock read would
  // make the backoff measure "gap between attempts" instead of "time since we started
  // trying" — which on a slow restart silently doubles every interval.
  _recoverBackoffMs = (_recoverBackoffMs >= AFFA_LINK_RECOVER_MAX_MS / 2)
                          ? static_cast<uint32_t>(AFFA_LINK_RECOVER_MAX_MS)
                          : _recoverBackoffMs * 2;
  _health.nextTryMs = now + _recoverBackoffMs;

  AFFA_LOGW(kTag, "link down %lu ms — asking it to recover (attempt %lu)",
            static_cast<unsigned long>(now - _health.downSince),
            static_cast<unsigned long>(_health.recoveries + _health.failures + 1));

  // `_rxStalled` rides along as `force`: a stall means the controller may well report
  // RUNNING, and recover()'s "already running, nothing to do" early-out — right for the
  // bus-off race — would otherwise return success without the reinstall the stall needs.
  if (!_link.recover(_rxStalled)) {
    // A link with no recover() of its own lands here every time, which is correct and is
    // why the default returns false: the backoff walks out to its ceiling and the counter
    // says plainly that nothing is working. It must NOT be mistaken for success, or the
    // backoff would reset on every attempt and spin.
    ++_health.failures;
    return;
  }

  ++_health.recoveries;
  AFFA_LOGI(kTag, "link recovered after %lu ms",
            static_cast<unsigned long>(now - _health.downSince));

#if AFFA_RX_STALL_MS
  // A completed recovery answers the stall: the driver has been reinstalled, which is all
  // this watchdog can buy. Release the latch so the link reports up; it does NOT re-arm —
  // only a real received frame does that — so a bus that stays silent costs exactly this
  // one restart and then quiet, never a reinstall per backoff for ever.
  //
  // The baseline resets for EVERY successful recovery, not only stall-triggered ones. A
  // bus-off outage skips the stall check the whole time it is down, so _lastRxMs still
  // holds its pre-outage value here — and without this reset the latch would fire on the
  // very next pass, granting the just-restarted controller zero listening time and, on a
  // genuinely silent bus, a second pointless reinstall at the already-armed backoff.
  _rxStalled = false;
  _rxHeard   = false;
  _lastRxMs  = now;
#endif

  // THE CONTROLLER IS NEW, SO THE HANDSHAKE IS VOID. A restarted driver has lost nothing of
  // ours — the queue, the tickets and the hold windows are all still here — but the PANEL
  // has been talking to a node that stopped answering, and whatever registration it had for
  // us is worthless. Tear the handshake down to the state begin() leaves it in and let
  // pumpSync() run it again from the top; pumpTx() splices the fresh 0x70 burst in front of
  // the held renders, exactly as it does after a peer loss.
  //
  // The queued renders deliberately SURVIVE. They are what the application asked to be on
  // the glass, they are already bounded by their own hold windows, and dropping them here
  // would put the "re-issue it yourself" burden straight back where this release took it
  // from.
  setSync(SyncState::Failed | SyncState::Start, EventKind::PeerLost);
  dropRegistrations();
  // A newly installed controller must not resurrect an old Carminat session by itself.
  // The panel owns the opening message; wait for its next 61 11 and keep this node silent
  // until then.  Other profiles retain their existing proactive behavior.
  if (_profile.waitForPanel) { _panelObserved = false; _nextAnnounceMs = _clock.millis() + _profile.announceWhenSilentMs; }
  _syncRequestObserved  = false;
  _authRequestObserved  = false;
  _authHelloPending     = false;
  _helloPending         = false;
  _helloIndex           = 0;
  _unauthControlPending = false;
  _unauthControlIssued  = false;
  _unauthControlSpent   = false;
  _nextUnauthControlMs  = now;
  _peerChannelSeen      = false;   // a new session means the display re-opens its channel
  _genericAckPending    = false;
  _genericAckBusyRetries = 0;
  _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;
  _nextSyncMs     = now;                      // handshake on the very next poll()
  _nextHelloMs    = now;
  _nextPayloadMs  = now;
  _nextPongMs     = now;
  for (uint8_t i = 0; i < _qCount; ++i) {
    if ((_queue[i].kind == JobKind::Payload || _queue[i].kind == JobKind::Reassert) &&
        !_queue[i].started)
      _queue[i].holdUntilMs = now + AFFA_TX_HOLD_MS;
  }
#endif  // AFFA_LINK_RECOVER_MS
}

// ---------------------------------------------------------------------------
// Sync FSM — ONE copy, parameterised by SyncProfile
// ---------------------------------------------------------------------------

TxDisposition AffaDisplayBase::sendAlive() {
  Frame alive;
  alive.id      = _profile.syncId;
  alive.len     = kPacketLength;
  alive.data[0] = _profile.aliveByte;
  alive.data[1] = 0x00;                   // a literal 0x00 in BOTH families, NOT the
                                          // filler: UpdateList sends 79 00 81 81 …
  for (uint8_t i = 2; i < kPacketLength; ++i) alive.data[i] = _profile.filler;
  return txFrame(alive);
}

TxDisposition AffaDisplayBase::sendSyncRequest() {
  Frame req;
  req.id      = _profile.syncId;
  req.len     = kPacketLength;
  req.data[0] = _profile.requestByte;
  req.data[1] = _profile.requestArg;
  for (uint8_t i = 2; i < kPacketLength; ++i) req.data[i] = _profile.filler;
  return txFrame(req);
}

uint32_t AffaDisplayBase::syncIntervalMs() const {
  return _profile.syncIntervalMs ? _profile.syncIntervalMs : AFFA_SYNC_INTERVAL_MS;
}

// The display's `61 11 01` (and, where profiled, its first bare `69`) earns exactly ONE
// B9 -> BA discovery transaction. Arming is idempotent: a panel repeating its request at
// 104 ms — or at line rate — must not turn the one-shot into a BA stream. The latch that
// makes it one-shot is `_unauthControlIssued`, set in pumpSync() only once the pair has
// actually been accepted by the link.
void AffaDisplayBase::armUnauthControl(uint32_t now) {
  if (_unauthControlPending || _unauthControlIssued || _unauthControlSpent) return;
  _unauthControlSpent       = true;   // consumed on arm, not on success
  _unauthControlPending     = true;
  _unauthControlStage       = BootstrapStage::None;
  _unauthControlBusyRetries = 0;
  _nextUnauthControlMs      = now;
}

// The Carminat START / 01 bootstrap: exactly B9 then BA, once, after RX has drained. It is
// neither a periodic failure retry nor authorization.
//
// STAGED, AND THE STAGE IS THE POINT. B9 and BA are two frames, not one transaction: once
// B9 has physically left, retrying the "pair" would put a second B9 on a bus that already
// has one. `_unauthControlStage` records how far the pair actually got, so a locally busy
// controller resumes at the frame it failed on.
//
// EVERY STAGE IS BOUNDED. A permanently undeliverable bootstrap must cost at most one offer
// plus kBootstrapBusyRetries per frame — a panel repeating `01`/`69` at its own cadence
// cannot re-arm the budget, because armUnauthControl() refuses while this is still pending.
// Without that bound a stuck controller turns the CAN task into an infinite B9/BA producer.
void AffaDisplayBase::pumpUnauthControl(uint32_t now) {
  if (!expired(now, _nextUnauthControlMs)) return;

  // Straight to the request on profiles that keep B9 for an established session.
  if (_unauthControlStage == BootstrapStage::None && !_profile.bootstrapAliveFrame)
    _unauthControlStage = BootstrapStage::Alive;

  if (_unauthControlStage == BootstrapStage::None) {
    const TxDisposition sent = sendAlive();
    if (sent == TxDisposition::Accepted) {
      _unauthControlStage       = BootstrapStage::Alive;
      _unauthControlBusyRetries = 0;
    } else if (sent == TxDisposition::Busy &&
               _unauthControlBusyRetries < kBootstrapBusyRetries) {
      ++_unauthControlBusyRetries;
      _nextUnauthControlMs = now + AFFA_TX_RETRY_MS;
      return;
    } else {
      // Rejected, or the retry budget is gone. The panel repeats its request on its own
      // timer; the recovery path owns a controller this broken.
      _unauthControlPending     = false;
      _unauthControlStage       = BootstrapStage::None;
      _unauthControlBusyRetries = 0;
      return;
    }
  }

  if (_unauthControlStage == BootstrapStage::Alive) {
    const TxDisposition sent = sendSyncRequest();
    if (sent == TxDisposition::Busy &&
        _unauthControlBusyRetries < kBootstrapBusyRetries) {
      ++_unauthControlBusyRetries;
      _nextUnauthControlMs = now + AFFA_TX_RETRY_MS;
      return;                        // ONLY the BA retries — the B9 is already on the wire
    }
    _unauthControlPending     = false;
    _unauthControlBusyRetries = 0;
    if (sent != TxDisposition::Accepted) {
      _unauthControlStage = BootstrapStage::None;
      return;
    }
    // BA is what the display answers with `61 11 xx`, so this is the moment the one-shot is
    // spent and the moment a later `01` becomes a full request. See helloAfterBootstrapRequest.
    _unauthControlStage   = BootstrapStage::Request;
    _unauthControlIssued  = true;
    _nextSyncMs           = now + syncIntervalMs();
    _peerDeadlineMs       = now + AFFA_PEER_TIMEOUT_MS;
  }
}

void AffaDisplayBase::queueHello(uint32_t now) {
  // A listener with no CAN ACK can repeat 61 11 at line rate. One sequence is enough;
  // do not restart an already staged B0#1/B0#2/B0#3 exchange from each duplicate.
  if (_helloPending) return;

  _helloPending = true;
  _helloIndex   = 0;

  // The floor prevents a generic legacy profile from turning a request storm into a hello
  // storm. A fresh session reset re-arms _nextHelloMs to now, so it does not inherit an old
  // floor. The profile's first-frame delay is then applied on top of that start point.
  const uint32_t start = expired(now, _nextHelloMs) ? now : _nextHelloMs;
  _nextHelloMs = start + _profile.helloFirstDelayMs;
}

void AffaDisplayBase::pumpHello(uint32_t now) {
  if (!_helloPending || !expired(now, _nextHelloMs)) return;

  // Zero gap retains historical profiles' immediate raw burst. A nonzero profile gap
  // submits exactly one frame per poll, allowing the panel's high-priority 1C1 -> 5C1
  // exchange to run between AFFA3 B0 fragments without any delay() or busy wait.
  do {
    Frame h;
    h.id  = _profile.syncId;
    h.len = kPacketLength;
    std::memcpy(h.data, _profile.hello[_helloIndex], kPacketLength);

    const TxDisposition offered = txFrame(h);
    if (offered != TxDisposition::Accepted) {
      // Nothing was accepted, so retry THIS frame, not a fresh burst. The auth gate remains
      // closed and no application payload can overtake an incomplete hello.
      _nextHelloMs = now + AFFA_TX_RETRY_MS;
      return;
    }

    ++_helloIndex;
    if (_helloIndex < _profile.helloCount) {
      if (_profile.helloFrameGapMs != 0) {
        _nextHelloMs = now + _profile.helloFrameGapMs;
        return;
      }
      // Legacy zero-gap profile: continue in this one poll, as the old raw burst did.
      continue;
    }

    const uint32_t helloFloor = _profile.helloMinMs ? _profile.helloMinMs : AFFA_HELLO_MIN_MS;
    _nextHelloMs = now + helloFloor;
    _helloIndex = 0;
    _helloPending = false;

    // The good 00 authorizes registration and rendering only after the final announce
    // frame has been accepted by the nonblocking CAN link.
    if (_authHelloPending) {
      _authHelloPending = false;
      setSync((_sync & ~SyncState::Failed & ~SyncState::Start), EventKind::SyncChanged);
    }

    // Registration is queued from pumpSync(), not here: it additionally waits for the
    // display's OWN channel registration. See the ordering note there.
    return;
  } while (_helloPending && expired(now, _nextHelloMs));
}

void AffaDisplayBase::pumpSync() {
  if (_passive) return;                   // the radio owns the handshake

  const uint32_t now = _clock.millis();

  // A deferred 5C1 control ACK has priority over B9/BA and the staged hello. It is the
  // display's own channel registration and is the only raw response that must interleave
  // with B0#1/B0#2 on the captured opening.
  pumpGenericAck(now);
  if (_genericAckPending) return;

  if (_unauthControlPending) {
    pumpUnauthControl(now);
    return;
  }

  // Coalesce a line-rate 61 11 retry storm to one profile-paced sequence. This runs before
  // the authorization gate because the final B0 is what clears that gate.
  pumpHello(now);

  // WE REGISTER AFTER THE DISPLAY DOES, AND THE ORDER IS MEASURED 4/4. In every OEM capture
  // the display's own `1C1 70` arrives 0.81-1.55 ms after B0#1 — between the first and
  // second announce frames — and the radio's `151 70` follows 60.69-61.34 ms LATER, after
  // B0#3. The display registers its channel first; we answer `5C1 74`; only then do we
  // register ours. Firing `151 70` off hello completion alone gets the order right only by
  // luck, and on a panel that is slow to open its channel it is simply wrong.
  if (_profile.registerAfterHello && _peerChannelSeen && !_helloPending &&
      _authRequestObserved && !_authHelloPending &&
      !hasFlag(_sync, SyncState::FuncsReg) && !registrationQueued()) {
    (void)queueRegistrations();
  }

  // AFFA3 NAV is panel-initiated.  `begin()` is deliberately quiet: treating the initial
  // FAILED state as permission to send BA was the source of a BA frame every second before
  // a display was even attached.  A valid panel frame flips this latch in handleSyncFrame().
  if (_profile.waitForPanel && !_panelObserved) {
    // ...but silence on BOTH sides is a deadlock, and the captures show the radio breaking
    // it. Announce slowly until something answers. See SyncProfile::announceWhenSilentMs.
    // BA ONLY, NO B9. The announce is a QUESTION ("is anyone there?"), and BA is the frame
    // that asks it — in "aknowledge offed display.csv" the radio's reattach announce is a
    // bare `3AF BA` with no B9 in front of it. B9 is the heartbeat of an established
    // session and has no business on a bus where the handshake has not started; sending it
    // here is pure noise during the phase that can least afford it.
    if (_profile.announceWhenSilentMs == 0 || !expired(now, _nextAnnounceMs)) return;
    _nextAnnounceMs = now + _profile.announceWhenSilentMs;
    (void)sendSyncRequest();
    return;
  }
  // On AFFA3 NAV a `69` only says the panel is alive. Only the completed good `61 11 00`
  // authorization (and its hello) allows normal heartbeat, registration and rendering.
  if (_profile.requireAuthRequest && (!_authRequestObserved || _authHelloPending)) return;
  // KEEP-ALIVE STARTS AFTER REGISTRATION, not after the hello. In the reattach capture the
  // radio's first B9 is at 85055726 — 15.3 ms after the display's `5F1 74` completed the
  // registration, and nothing on 0x3AF between B0#3 and it. B9 is the heartbeat of an
  // ESTABLISHED session; emitting it mid-opening is noise in the phase that can least
  // afford it. The opening still has its own traffic (B0, the 0x70 probes, the 5C1 reflex),
  // and a silent bus is still answered by the BA announce above, so this cannot deadlock.
  if (_profile.registerAfterHello && !hasFlag(_sync, SyncState::FuncsReg)) return;
  if (!expired(now, _nextSyncMs)) return;

  // THE PACING FLOORS MUST NOT GO WRAP-STALE. Each is re-armed only by the event it gates
  // (a hello burst, a pong), so 2^31 ms without that event flips expired()'s signed
  // half-window compare and the floor reads "not yet" for the NEXT ~25 days — the feature
  // silently off, nothing in any counter to show it. Dragging an already-expired floor up
  // to `now` once per tick changes no pacing decision — expired it was and expired it
  // stays — but it caps the staleness at one tick, which keeps the compare valid for ever.
  if (expired(now, _nextHelloMs)) _nextHelloMs = now;
  if (expired(now, _nextPongMs))  _nextPongMs  = now;

  if (sendAlive() != TxDisposition::Accepted) {
    // A running controller may only be locally queue-full. No sync state changes until its
    // raw B9 offer was accepted, and the short retry floor keeps a tight poll loop benign.
    _nextSyncMs = now + AFFA_TX_RETRY_MS;
    return;
  }

  if (hasFlag(_sync, SyncState::Failed) || hasFlag(_sync, SyncState::Start)) {
    if (_profile.sendSyncRequest && sendSyncRequest() != TxDisposition::Accepted) {
      _nextSyncMs = now + AFFA_TX_RETRY_MS;
      return;
    }

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
    // Wait for its next 61 11 — but not for ever. A panel that went to sleep will never
    // send one, so the slow announce re-arms here and starts calling it back.
    if (_profile.waitForPanel) {
      _panelObserved  = false;
      _nextAnnounceMs = now + _profile.announceWhenSilentMs;
    }
    _syncRequestObserved  = false;
    _authRequestObserved  = false;
    _peerChannelSeen      = false;   // the display re-opens its 1C1 in the new session
    _authHelloPending     = false;
    _helloPending         = false;
    _helloIndex           = 0;
    _nextHelloMs          = now;
    _nextPayloadMs        = now;
    _unauthControlPending = false;
    _unauthControlIssued  = false;
    _unauthControlSpent   = false;
    _nextUnauthControlMs  = now;
    _genericAckPending    = false;
    _genericAckBusyRetries = 0;
    _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;

    // THE QUEUE SURVIVES THE PEER NOW. This used to be dropUnstarted(Cancelled) — the panel
    // has forgotten us, so everything addressed to it was destroyed and the application had
    // to notice the sync event, keep its own copy of what was on screen, and re-issue it.
    // That is precisely the recovery work 0.3.0 moves into the library.
    //
    // What actually became invalid is the REGISTRATION, so that is what is dropped; the
    // renders are held, re-registered by pumpTx() when the panel comes back, and given up
    // by their own hold windows if it does not.
    dropRegistrations();
    for (uint8_t i = 0; i < _qCount; ++i) {
      if ((_queue[i].kind == JobKind::Payload || _queue[i].kind == JobKind::Reassert) &&
          !_queue[i].started)
        _queue[i].holdUntilMs = now + AFFA_TX_HOLD_MS;
    }
  }

  // `= now + interval`, never `+= interval`: a caller that stalled for ten seconds must
  // not produce a catch-up burst of ten heartbeats. It happens only after the raw work
  // above was accepted; a Busy offer gets the shorter retry floor instead.
  _nextSyncMs = now + syncIntervalMs();
}

void AffaDisplayBase::setSync(SyncState s, EventKind extra) {
  if (s == _sync) return;
  const SyncState prev = _sync;
  _sync = s;                                  // state first, callbacks second

  // Registration is the lifetime boundary for the panel's volatile state. The desired
  // power/control cache is deliberately re-armed only on this falling edge, never on a
  // heartbeat or a transient TX retry, so one recovered session produces at most one
  // internal restore before held application work resumes.
  if (hasFlag(prev, SyncState::FuncsReg) && !hasFlag(s, SyncState::FuncsReg) &&
      _cachedControl.valid) {
    _cachedControl.pending = true;
  }

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
  j.reassertAfterSession = opt.reassertAfterSession;
  j.started    = false;
  j.abandon    = false;
  j.len        = len;
  j.sent       = 0;
  j.frameIndex = 0;
  j.tries      = 0;
  j.readyAtMs  = _clock.millis();          // startable immediately; a retry moves it out
  j.holdUntilMs = _clock.millis() + AFFA_TX_HOLD_MS;
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

  // PERMANENT REJECTIONS, and only these. Every one of them is a programming error the
  // caller can fix and no amount of waiting will: a null buffer, a payload past the
  // transport ceiling, an id this panel does not own. Rejecting is the whole point.
  if (!data || len == 0)          { _lastResult = Result::BadArgument;  return kNoTicket; }
  if (len > AFFA_MAX_PAYLOAD)     { _lastResult = Result::TooLong;      return kNoTicket; }
  if (!knownFunc(funcId))         { _lastResult = Result::UnknownFunc;  return kNoTicket; }

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
    _lastResult = _link.isLive() ? Result::NoSync : Result::LinkDown;
    return kNoTicket;
  }

  // Lazy function registration, byte-identical to the legacy wire order: the first send
  // after a resync walks the WHOLE function table in declaration order and sends a 1-byte
  // 0x70 to each, then latches FUNCSREG, then the payload.
  //
  // Only when the link is up: registering into a dead bus would burn the queue slots the
  // held payload needs, and pumpTx() splices the burst in when the link returns anyway.
  const bool needReg = !_passive && linkReady() &&
                       !hasFlag(_sync, SyncState::FuncsReg) && !registrationQueued();

  int ci = -1;
  if (opt.coalesce) ci = findCoalescable(funcId, opt.slot);

  const uint8_t newSlots =
      static_cast<uint8_t>((ci >= 0 ? 0 : 1) + (needReg ? _funcCount : 0));
  if (_qCount + newSlots > AFFA_TX_QUEUE_DEPTH) {
    _lastResult = Result::QueueFull;
    return kNoTicket;
  }

  if (needReg) {
    (void)queueRegistrations();         // capacity was reserved above; the helper places
                                         // the wire-ordered probes ahead of held payloads
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
  if (_profile.requireAuthRequest && (!_authRequestObserved || _authHelloPending)) return false;
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
  uint8_t proposedSent = job.sent;
  while (i < kPacketLength && proposedSent < job.len)
    f.data[i++] = job.data[proposedSent++];
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
  const bool cacheAfterAck = job.reassertAfterSession;
  const uint8_t cachedLen = job.len;
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
        setSync(_sync | SyncState::FuncsReg, EventKind::Registered);
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
  if (!_passive && _profile.requireAuthRequest &&
      (!_authRequestObserved || _authHelloPending))
    return Result::NoSync;

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

  // A wire key is intentionally not queued behind display traffic, so there is no public
  // Busy result to return. Its immediate caller sees the historical SendFailed on either a
  // temporary local-busy or a hard refusal; only queued display payloads get automatic
  // Busy retry semantics.
  return txFrame(f) == TxDisposition::Accepted ? Result::Ok : Result::SendFailed;
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

void AffaDisplayBase::setPassive(bool on) {
  if (_passive && !on) {
    // LEAVING passive after an arbitrary quiet period. Every sync deadline froze the
    // moment passive was entered — pumpSync() early-outs on the flag, so nothing re-armed
    // them — and after 2^31 ms of that, expired()'s signed compare reads every one of
    // them as "not yet" for the NEXT ~25 days: no heartbeat, no hello, no pong, and a
    // takeover that silently is not one. Re-arm the lot from the current clock, exactly
    // as begin() would, so the first active poll() opens with a heartbeat and the peer
    // gets a full window before it can be declared lost.
    const uint32_t now = _clock.millis();
    _nextSyncMs     = now;
    _nextHelloMs    = now;
    _nextPongMs     = now;
    _nextUnauthControlMs = now;
    _nextPayloadMs  = now;
    _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;
  }
  _passive = on;
}
bool AffaDisplayBase::passive() const { return _passive; }
void AffaDisplayBase::setSelfAck(bool on) { _selfAck = on; }

SyncState AffaDisplayBase::syncState() const { return _sync; }
bool AffaDisplayBase::synced() const {
  if (hasFlag(_sync, SyncState::Failed)) return false;
  return !_profile.requireAuthRequest || (_authRequestObserved && !_authHelloPending);
}
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
