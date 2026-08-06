// AffaDisplayBase — lifecycle, the poll() contract, the receive drain, keys, and the
// public surface.
//
// ONE CLASS, FOUR TRANSLATION UNITS, as of step 7 of docs/REFACTOR-PLAN.md. This file was
// 2156 lines spanning four unrelated jobs; the other three are AffaSync.cpp (the opening
// FSM), AffaTx.cpp (queue, ISO-TP, retries) and AffaObserve.cpp (the frame tap and the
// choke point every frame passes through). Nothing moved but line numbers — the split
// commit changes no behaviour, deliberately, so that the next one can be read.
//
// WHY pumpRx() IS HERE AND NOT IN AffaObserve.cpp: it is poll() orchestration. It decides
// which of the three FSMs a frame belongs to, and the ORDER it does that in — echo drop,
// then the deaf watchdog, then sync, then ACK, then the auto-ACK reflex, then the panel
// hook — is the contract, not an implementation detail.
#include "AffaBaseInternal.h"
#include "AffaDisplayBase.h"

#include <cstring>

namespace affa {
using namespace basedetail;


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
  _phase                  = Phase::Silent;   // written directly for the same reason _sync is:
                                             // begin() is a reset, not a transition, and it
                                             // must not log an edge that did not happen
  _panelObserved          = false;       // profiles that wait for their panel start silent
  _syncRequestObserved    = false;       // a bare 69 cannot open a Carminat session
  _helloPending           = false;
  _unauthControlPending   = false;
  _unauthControlSpent     = false;
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
  _autoPowerTicket   = kNoTicket;        // NOT _autoPower: that is a build's decision and
                                         // survives begin(), like _passive and _selfAck
  _sessionsLost      = 0;                // counted since THIS begin(), so a re-begin() does
  _lastSessionLossMs = 0;                // not carry a previous run's drops into a soak
  _lastLossReason    = LossReason::None;
  _lossReasonNext    = LossReason::None;
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

  // THE ONE TIME-DRIVEN PHASE EDGE. Every other transition is a frame; this one is the
  // measured quiet interval between the last registration ACK and the first payload
  // expiring, so it has no frame to hang on and is promoted here instead. After pumpTx()
  // deliberately: the gate that actually holds the payloads back is in there, and a phase
  // that said Ready before that gate opened would be a lie a console would repeat.
  // The power-on the library queued at registration is still in flight, so `Ready` waits for
  // its ACK. Issuing it is NOT done here — see setSync()'s FuncsReg rising edge for why the
  // enqueue has to happen a full quiet interval earlier than this.
  if (_phase == Phase::Settling && expired(_clock.millis(), _nextPayloadMs))
    enterPhase(_autoPowerTicket == kNoTicket ? Phase::Ready : Phase::Powering);

  onPoll();

  _inPoll = false;
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
      // The latch is separate from the phase because the `1C1` usually arrives DURING the
      // burst, not after it — measured between B0#1 and B0#2 — so it has to be remembered
      // rather than acted on in order.
      if (f.len >= 1 && f.data[0] == kRegisterByte) {
        _peerChannelSeen = true;
        if (_phase == Phase::AwaitPeerChannel) enterPhase(Phase::Registering);
      }
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
  _lossReasonNext = LossReason::LinkRestarted;
  setSync(SyncState::Failed | SyncState::Start, EventKind::PeerLost);
  dropRegistrations();
  // A newly installed controller must not resurrect an old Carminat session by itself.
  // The panel owns the opening message; wait for its next 61 11 and keep this node silent
  // until then.  Other profiles retain their existing proactive behavior.
  if (_profile.waitForPanel) { _panelObserved = false; _nextAnnounceMs = _clock.millis() + _profile.announceWhenSilentMs; }
  // ALL THE WAY BACK. A new controller has never announced, so the opening restarts at the
  // top — unlike the panel-voided teardown, which keeps Announced because our BA is still
  // out there as far as the panel is concerned.
  enterPhase(Phase::Silent);
  _syncRequestObserved  = false;
  _helloPending         = false;
  _helloIndex           = 0;
  _unauthControlPending = false;
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
  if (!_passive && !openingReleased()) return Result::NoSync;

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
void AffaDisplayBase::setAutoPower(bool on) { _autoPower = on; }
bool AffaDisplayBase::autoPower() const { return _autoPower; }

// STORED, AND THE SOURCE OF TRUTH — step 4 of docs/REFACTOR-PLAN.md is done. It was derived
// from the booleans for exactly one commit, long enough for
// test_phase_walks_the_measured_opening_in_order to pin the reading against the wire; that
// test is unchanged across the inversion, which is what makes the inversion checkable at all.
Phase AffaDisplayBase::phase() const { return _phase; }

RenderSlot AffaDisplayBase::lastRendered() const { return _lastRendered; }
uint32_t   AffaDisplayBase::lastRenderedMs() const { return _lastRenderedMs; }
uint32_t AffaDisplayBase::sessionsLost() const { return _sessionsLost; }
uint32_t AffaDisplayBase::lastSessionLossMs() const { return _lastSessionLossMs; }
LossReason AffaDisplayBase::lastLossReason() const { return _lastLossReason; }

SyncState AffaDisplayBase::syncState() const { return _sync; }
bool AffaDisplayBase::synced() const {
  if (hasFlag(_sync, SyncState::Failed)) return false;
  return openingReleased();
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
Result AffaDisplayBase::showConfirmBox(const char*, const char*, const char*) {
  return Result::NotSupported;
}
Result AffaDisplayBase::showInfoPopup(const char*, const char*, const char*) {
  return Result::NotSupported;
}

} // namespace affa
