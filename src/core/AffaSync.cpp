// AffaSync — the opening, and nothing else.
//
// Split out of AffaDisplayBase.cpp by step 7 of docs/REFACTOR-PLAN.md; see AffaObserve.cpp
// for the four-way division. What lives here is the Phase table and the frames that drive
// it: the announce, the hello burst, the panel's channel reflex, the heartbeat, and the
// watchdog that ends a session.
//
// EVERY RULE IN THIS FILE IS MEASURED. docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md derives them
// from docs/captures/*.csv, and the whole sequence has run 1 h 36 m on glass. Four separate
// protocol bugs lived here, all the same shape — a special case standing in for a general
// rule — so when something looks like it needs a new branch, check the captures first.
#include "AffaBaseInternal.h"
#include "AffaDisplayBase.h"

#include <cstring>

namespace affa {
using namespace basedetail;

bool AffaDisplayBase::handleSyncFrame(const Frame& f) {
  if (_passive) return true;              // a real radio owns the handshake
  if (f.len < 1) return true;

  if (f.data[0] == kSyncRequestByte0 && f.len >= 2 && f.data[1] == kSyncRequestByte1) {
    // ANY COMPLETE `61 11 xx` IS THE SAME REQUEST. BYTE 2 IS NOT AN AUTHORIZATION GRADE.
    //
    // MEASURED, NOT ASSUMED. docs/captures/"aknowledge offed display cONNECT OT POWER.csv"
    // is a real OEM radio meeting a display that had been powered with no radio present.
    // That display repeats `3CF 61 11 01` every ~104 ms and NEVER sends `61 11 00` — not
    // once in sixteen requests — yet the radio answers it with the ordinary B0 announce
    // burst 30.75 ms later and the session completes: registration, `03 52`, ISO-TP text,
    // and the `01` never reappears. The low bit reports the panel's own state.
    //
    // THIS BRANCH USED TO BE TWO. A "good byte" arm gated on `authRequestByte2`, plus a
    // dimmer copy for every other spelling gated on `helloOnNonAuthRequest` and
    // `oneShotResyncOnStart`, with `helloAfterBootstrapRequest` as the trapdoor between
    // them. Two of this session's four protocol bugs lived in that seam and both were the
    // same shape — a special case standing in for a general rule (see HANDOFF.md, "How this
    // project gets things wrong"). The real gate is our own `BA`, never the byte, and it is
    // `helloRequiresAnnounce` below.
    if (_profile.requireAuthRequest) {
      // A short `61 11` has no byte 2 on the wire, and an incomplete request is not one.
      // Do not read stale storage: the OEM corpus really does carry 0x3CF at DLC 1 and
      // DLC 2, and the legacy shim read uninitialised stack here.
      if (f.len < 3) return true;

      const uint32_t now = _clock.millis();
      _panelObserved = true;
      _syncRequestObserved = true;
      _peerDeadlineMs = now + AFFA_PEER_TIMEOUT_MS;

      // A complete `61 11 xx` arriving once we already hold registrations says the panel
      // forgot us: "your registration is void". In every OEM capture a registered display
      // stops sending `61 11` ENTIRELY — so one arriving here is real loss, never chatter,
      // and the value of xx is irrelevant to that.
      //
      // THIS USED TO BE RESTRICTED TO `61 11 01`, and `61 11 00` fell straight through:
      // needsHelloBeforeAuth then computed false and NOTHING happened. Measured on the
      // bench — the panel sent `61 11 00` twenty-six times, then fifteen more, while we
      // cheerfully kept pushing fullscreen rows at it. A deauthorized panel asking to
      // re-open must stop the application traffic, not be talked over.
      if (hasFlag(_sync, SyncState::FuncsReg)) {
        AFFA_LOGW(kTag, "61 11 %02X while registered - the panel voided us; reopening",
                  static_cast<unsigned>(f.data[2]));
        _lossReasonNext = LossReason::PanelVoided;
        setSync(SyncState::Failed, EventKind::SyncChanged);
        invalidateInFlightForSession(now);
        // BACK TO Announced, NOT TO Silent. Our `BA` is long since on the wire and the
        // panel is answering it; this request is the one that draws the replacement burst,
        // immediately, and that is the self-healing path a 96-minute soak took fourteen
        // times without a screen being lost. Falling to Silent would make us wait for an
        // announce the panel has already had.
        enterPhase(Phase::Announced);
        _helloPending        = false;
        _helloIndex          = 0;
        _nextHelloMs         = now;
        // THE THREE PLACES A REGISTRATION IS VOIDED MUST AGE THE QUEUE IDENTICALLY. The
        // re-registration this provokes takes time the held renders were never budgeted
        // for; without a fresh window a payload that aged politely while REGISTERED is
        // given up as NoSync by pumpTx()'s hold check before the re-registration it now
        // depends on has even been spliced. Same re-arm as the peer-timeout and the
        // generic-profile Start path. It used to live only in the deleted second branch,
        // which is to say it was missing from the one path the bench actually exercises.
        for (uint8_t i = 0; i < _qCount; ++i) {
          if ((_queue[i].kind == JobKind::Payload || _queue[i].kind == JobKind::Reassert) &&
              !_queue[i].started)
            _queue[i].holdUntilMs = now + AFFA_TX_HOLD_MS;
        }
      }
      // The request may arrive while the previous hello burst is still inside its rate
      // floor. Keep application TX closed until the hello that answers THIS phase has
      // actually been emitted.
      // Our BA has to be on the wire before the burst means anything. The first request
      // arms it and is answered with nothing else; the panel asks again on its own ~104 ms
      // timer and THAT one opens the session. See SyncProfile::helloRequiresAnnounce.
      // FAIL OPEN, NEVER WEDGE. If the announce is still pending or can still be armed,
      // hold the burst back for it. But once the one-shot is SPENT without ever reaching
      // the wire — a Rejected BA, or a busy budget burned through — the phase stays at
      // Silent for ever, and gating on it alone would refuse the hello on every subsequent
      // request while nothing short of a link reset could clear it. A session opened
      // without the announce is merely less faithful; a session that can never open is
      // broken, so the unreachable case falls through to the ordinary path.
      if (_profile.helloRequiresAnnounce && _phase == Phase::Silent) {
        const bool announceStillPossible =
            _unauthControlPending || !_unauthControlSpent;
        armUnauthControl(now);
        if (announceStillPossible) return true;
        AFFA_LOGW(kTag, "announce unreachable; opening without it rather than stalling");
      }
      // ONE ORDERED QUESTION, where there used to be three booleans in a disjunction.
      // `!_authRequestObserved || _authHelloPending` was "the opening has not released
      // traffic yet", which is exactly `_phase` not having reached AwaitPeerChannel. The
      // third term, `hasFlag(_sync, Failed)`, is kept because it is free and because the
      // two are only provably equivalent as long as pumpHello() is the sole place that
      // clears Failed — a coupling worth a belt as well as braces.
      const bool needsHelloBeforeAuth =
          !atLeast(_phase, Phase::AwaitPeerChannel) || hasFlag(_sync, SyncState::Failed);
      if (needsHelloBeforeAuth) {
        enterPhase(Phase::HelloPending);
        // Do not cancel a discovery announce armed by a preceding request or `69` in this
        // same RX drain. The display may ask again immediately after it; that still owns
        // exactly one BA, never a new retry stream.
        _nextSyncMs = now + syncIntervalMs();
        queueHello(now);
      }
      return true;
    }

    const uint32_t now = _clock.millis();
    const bool firstSyncRequest = !_syncRequestObserved;
    _panelObserved = true;
    _syncRequestObserved = true;
    // NO AUTHORIZATION GATE ON THIS FAMILY, so the opening is released the moment the panel
    // asks and the phase goes straight to the registration wait. It is an approximation:
    // registration here is LAZY, triggered by the first render, so this can sit at
    // Registering with no probe queued. Step 8 of docs/REFACTOR-PLAN.md brings UpdateList
    // onto the measured opening and the mapping stops being a shrug.
    if (!atLeast(_phase, Phase::Registering)) enterPhase(Phase::Registering);
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
        _lossReasonNext = LossReason::PanelVoided;
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

    // The panel's `69` is not authorization. It may nevertheless be the very first display
    // message in the real capture, so a panel-led family answers it with the one bounded
    // `BA` announce here. A bare ping still cannot schedule B0, register functions, or
    // release a render; only a complete `61 11 xx` can do that.
    //
    // ONLY BEFORE THE FIRST REQUEST. `_syncRequestObserved` is the guard, not a flag: once
    // the conversation has started the announce belongs to the request path, and a ping
    // arriving mid-session must not re-open it. This used to be gated additionally on
    // `oneShotResyncOnPeerAlive`, which no profile that can reach this line sets false.
    if (_profile.requireAuthRequest && !_syncRequestObserved) {
      armUnauthControl(now);
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

// THE ONE WRITER. Every phase change in the library comes through here, which is what makes
// the opening readable in a log: nine lines, in order, and the one that does not appear is
// the frame that never arrived.
void AffaDisplayBase::enterPhase(Phase p) {
  if (p == _phase) return;
  AFFA_LOGI(kTag, "phase %s -> %s", phaseName(_phase), phaseName(p));
  _phase = p;
}

// The transmit gate, asked once instead of as `!_authRequestObserved || _authHelloPending`
// in four places. A family without the authorization gate has nothing to wait for.
bool AffaDisplayBase::openingReleased() const {
  return !_profile.requireAuthRequest || atLeast(_phase, Phase::AwaitPeerChannel);
}

// The display's first `61 11 xx` (or its first bare `69`) earns exactly ONE announce.
// Arming is idempotent: a panel repeating its request at 104 ms — or at line rate — must not
// turn the one-shot into a BA stream. What makes it one-shot is `_unauthControlSpent`,
// consumed here; `Phase::Announced` is the separate fact that the frame actually left.
void AffaDisplayBase::armUnauthControl(uint32_t now) {
  if (_unauthControlPending || atLeast(_phase, Phase::Announced) || _unauthControlSpent)
    return;
  _unauthControlSpent       = true;   // consumed on arm, not on success
  _unauthControlPending     = true;
  _unauthControlBusyRetries = 0;
  _nextUnauthControlMs      = now;
}

// THE ANNOUNCE IS A BARE `BA`, ONCE, AFTER RX HAS DRAINED. It is neither a periodic failure
// retry nor authorization.
//
// NO B9 IN FRONT OF IT. `BA` asks the question — "is anyone there?" — and `B9` answers
// "still here", which is only meaningful once there is a session to be still in. [CAP] the
// reattach capture "aknowledge offed display.csv" at 84945066 is the cleanest look at a
// radio opening a conversation and it is a bare `3AF BA`. Two other captures do show a B9
// before registration ("cONNECT OT POWER" at 147328246, "offed display 2" at 9850470), both
// consistent with a radio whose free-running 500 ms heartbeat simply ticked during the
// opening rather than with a two-frame bootstrap. We take the quieter reading, and it is
// what ran 1 h 36 m on glass. This used to be `SyncProfile::bootstrapAliveFrame`, and with
// it goes the two-stage sender: one frame is one frame, so there is no stage to resume at.
//
// IT IS BOUNDED. A permanently undeliverable announce must cost at most one offer plus
// kBootstrapBusyRetries — a panel repeating its request at its own cadence cannot re-arm the
// budget, because armUnauthControl() refuses while this is still pending. Without that bound
// a stuck controller turns the CAN task into an infinite BA producer.
void AffaDisplayBase::pumpUnauthControl(uint32_t now) {
  if (!expired(now, _nextUnauthControlMs)) return;

  const TxDisposition sent = sendSyncRequest();
  if (sent == TxDisposition::Busy && _unauthControlBusyRetries < kBootstrapBusyRetries) {
    ++_unauthControlBusyRetries;
    _nextUnauthControlMs = now + AFFA_TX_RETRY_MS;
    return;
  }

  // Rejected, or the retry budget is gone: the panel repeats its request on its own timer,
  // and the recovery path owns a controller this broken. `_unauthControlSpent` stays set, so
  // handleSyncFrame()'s fail-open arm is what stops that becoming a permanent wedge.
  _unauthControlPending     = false;
  _unauthControlBusyRetries = 0;
  if (sent != TxDisposition::Accepted) return;

  // BA is what the display answers with `61 11 xx`, so this is the moment the announce is
  // issued and the moment the NEXT request becomes the one that draws the burst. See
  // SyncProfile::helloRequiresAnnounce. It must mean "the display has been asked", never
  // merely "we tried" — which is why it is set on ACCEPTANCE and nowhere else.
  if (_phase == Phase::Silent) enterPhase(Phase::Announced);
  _nextSyncMs           = now + syncIntervalMs();
  _peerDeadlineMs       = now + AFFA_PEER_TIMEOUT_MS;
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

    // THE BURST IS OUT. This is the edge that releases registration and rendering, and it
    // fires only once the FINAL frame has been accepted by the nonblocking CAN link — not
    // when the burst was scheduled.
    //
    // Where it goes depends on whether the display has already opened its own channel. It
    // usually has: [CAP] 4/4, the `1C1 70` lands between B0#1 and B0#2, so by the time B0#3
    // is accepted `_peerChannelSeen` is set and AwaitPeerChannel is skipped entirely. The
    // phase exists for the case that made a bench sit dark for a session — a panel that
    // never answered, because it never got our announce.
    if (_phase == Phase::HelloPending) {
      enterPhase(_peerChannelSeen ? Phase::Registering : Phase::AwaitPeerChannel);
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
  // `openingReleased()` is the whole of what `_authRequestObserved && !_authHelloPending`
  // used to say here, and saying it once is the point of the phase.
  if (_profile.registerAfterHello && _peerChannelSeen && !_helloPending &&
      openingReleased() &&
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
  if (!openingReleased()) return;
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
    _lossReasonNext = LossReason::PeerTimeout;
    setSync(SyncState::Failed, EventKind::PeerLost);   // every other bit, FuncsReg
                                                       // included, is dropped
    // Wait for its next 61 11 — but not for ever. A panel that went to sleep will never
    // send one, so the slow announce re-arms here and starts calling it back.
    if (_profile.waitForPanel) {
      _panelObserved  = false;
      _nextAnnounceMs = now + _profile.announceWhenSilentMs;
    }
    // ALL THE WAY BACK TO Silent. A panel that stopped pinging has, as far as we can tell,
    // stopped listening too — so the next opening owes it the whole exchange including a
    // fresh announce, exactly as a cold bus does. This is deliberately NOT the panel-voided
    // teardown, which keeps Announced: there the panel is demonstrably talking to us.
    enterPhase(Phase::Silent);
    _syncRequestObserved  = false;
    _peerChannelSeen      = false;   // the display re-opens its 1C1 in the new session
    _helloPending         = false;
    _helloIndex           = 0;
    _nextHelloMs          = now;
    _nextPayloadMs        = now;
    _unauthControlPending = false;
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
  //
  // THIS FALLING EDGE IS ALSO THE MOMENT THE SESSION IS LOST, i.e. exactly where Phase
  // leaves Ready — Ready requires FuncsReg, and the only other way out of it is the
  // time-based Settling boundary, which is not a loss. Counting it here is what turns "the
  // panel drops us about every seven minutes" from something the owner discovers by reading
  // a 96-minute log into a number on the status page. Fourteen of them went unnoticed
  // through a soak that looked perfect. Step 6 of docs/REFACTOR-PLAN.md hangs the wire-ring
  // snapshot on this same edge.
  if (hasFlag(prev, SyncState::FuncsReg) && !hasFlag(s, SyncState::FuncsReg)) {
    ++_sessionsLost;
    _lastSessionLossMs = _clock.millis();
    _lastLossReason    = _lossReasonNext;
    AFFA_LOGW(kTag, "session lost (#%lu): %s",
              static_cast<unsigned long>(_sessionsLost), lossReasonName(_lastLossReason));
    if (_cachedControl.valid) _cachedControl.pending = true;
    // The panel has forgotten the screen, so the library must too. Otherwise an application
    // holding its marquee back for a menu goes on holding it back for a menu that is no
    // longer there — silently, and for ever.
    _lastRendered   = RenderSlot::None;
    _lastRenderedMs = 0;
  }
  _lossReasonNext = LossReason::None;   // never leaks into the next, unrelated transition

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

} // namespace affa
