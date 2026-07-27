// Entire translation unit gated — see AffaConfig.h.
#include "VirtualPanelBase.h"

#if AFFA_ENABLE_VIRTUAL_PANEL

#include "../core/IClock.h"

namespace affa {

bool VirtualPanelBase::begin(ICanLink& link, IClock& clock) {
  _link  = &link;
  _clock = &clock;

  // begin() resets the MODEL but not the profile: a twin is a panel that was just powered
  // on, and a panel that was just powered on has an empty screen and no latched icons.
  _screen.clear();
  _asm.reset();
  _synced       = false;
  _lastDecodeMs = 0;
  _pingArmed    = false;
  _nextPingMs   = 0;
  _xferActive   = false;
  _xferContent  = 0;
  _ffDl         = 0;
  _iconsKnown   = false;
  _icons        = 0;
  _iconMode     = 0;
  return true;
}

void VirtualPanelBase::trackTransfer(const Frame& f) {
  if (f.len == 0) { _xferActive = false; return; }

  if (f.data[0] == 0x10) {
    // First frame. payload[1] is the DECLARED content length (FF_DL) and payload[0..1]
    // are not themselves content, hence len - 2.
    _xferId      = f.id;
    _ffDl        = (f.len >= 2) ? f.data[1] : 0;
    _xferContent = (f.len >= 2) ? static_cast<uint16_t>(f.len - 2) : 0;
    _xferActive  = true;
    return;
  }

  if (_xferActive && f.id == _xferId && (f.data[0] & 0xF0) == kIsoTpCfBase) {
    _xferContent = static_cast<uint16_t>(_xferContent + (f.len - 1));
    return;
  }

  // A single frame (03 52 …, 05 56 …, 07 29 01 …, the 0x70 registration probe) or a
  // continuation for some other id. Either way no transfer is in progress.
  _xferActive = false;
}

void VirtualPanelBase::onResync() {
  // Everything the panel latches is now unknown. See the header for why the driver's
  // matching obligation is to force a 0x7F on this same edge.
  _screen.clear();
  _asm.reset();
  _iconsKnown = false;
  _icons      = 0;
  _iconMode   = 0;
}

bool VirtualPanelBase::acksId(uint16_t id) const {
  if (id == _p.ctrlId || id == _p.textId) return true;
  for (uint8_t i = 0; i < _p.funcCount; ++i)
    if (_p.funcIds && _p.funcIds[i] == id) return true;
  return false;
}

void VirtualPanelBase::autoAck(uint16_t id) {
  if (!_emulate || !_link) return;

  Frame ack;
  // COMPUTED, never tabulated. 0x0A9 | 0x400 = 0x4A9 and not 0x5A9.
  ack.id  = static_cast<uint32_t>(id) | _p.replyFlag;
  ack.len = kPacketLength;

  bool done = true;
  switch (_ackMode) {
    case AckMode::Done:     done = true;            break;
    case AckMode::Partial:  done = false;           break;
    case AckMode::Declared: done = declaredDone();  break;
  }

  uint8_t i = 1;
  if (!done) {
    ack.data[0] = kAckPartial0;
    ack.data[1] = kAckPartial1;
    ack.data[2] = kAckPartial2;
    i = 3;
  } else {
    ack.data[0] = kAckDone;
    _xferActive = false;      // the transfer ends here, whatever the sender still holds
  }
  // The remaining bytes are OUR filler and carry no meaning. A real panel pads 0xA3, an
  // OEM cluster 0x84, the OEM radio 0xFF — which is exactly why nothing anywhere may
  // match on, validate or assert a received filler byte. Only data[0] (DONE) and
  // data[0..2] (PARTIAL) are readable on this channel.
  for (; i < kPacketLength; ++i) ack.data[i] = _p.filler;

  _link->send(ack);
  ++_acksSent;
}

void VirtualPanelBase::sendSync(uint8_t b0, uint8_t b1, uint8_t b2, bool haveB1B2) {
  if (!_emulate || !_link) return;

  Frame f;
  f.id  = _p.syncReplyId;
  f.len = kPacketLength;
  f.data[0] = b0;
  uint8_t i = 1;
  if (haveB1B2) { f.data[1] = b1; f.data[2] = b2; i = 3; }
  for (; i < kPacketLength; ++i) f.data[i] = _p.filler;

  _link->send(f);
  ++_syncReplies;
}

bool VirtualPanelBase::handleSync(const Frame& f) {
  // Short DLC is real on this channel — the OEM corpus holds 0x3CF with DLC 1 and DLC 2 —
  // so length is checked before every index, here and in the driver.
  if (f.len < 1) return true;

  // The radio's hello / announce. This is the edge that means "the link is up".
  if (f.data[0] == kRegisterByte) {
    if (!_synced) {
      _synced = true;
      onResync();
    }
    return true;
  }

  // The radio's sync request -> the panel asks it to announce itself.
  if (_p.requestByte != 0 && f.data[0] == _p.requestByte) {
    // data[2] is the PANEL'S FILLER, not 0x01. The 0x01 form exists in [REF] and would
    // set SyncState::Start in the driver, but it has NEVER been observed in any capture:
    // all 791 instances in the corpus are `61 11 A3 A3 …`. Emitting 0x01 here would make
    // the twin drive a code path no real panel has ever driven. WIRE-SPEC §5.3.
    sendSync(kSyncRequestByte0, kSyncRequestByte1, _p.filler, true);
    return true;
  }

  // The radio's ~1 Hz heartbeat -> the panel's ~1 Hz peer-alive ping.
  //
  // A real panel pings on a timer of its own. IVirtualPanel is FED frames and never
  // polled, so the twin hangs its ping off the heartbeat instead: same cadence, same
  // effect on the driver's 5000 ms wall-clock watchdog, and no poll() to forget to call.
  // The deadline below is a rate limit, not a schedule — it is what stops a test that
  // advances the clock in 1 ms steps from turning one heartbeat into a ping storm.
  if (_p.aliveByte != 0 && f.data[0] == _p.aliveByte) {
    const uint32_t now = _clock ? _clock->millis() : 0;
    if (_pingArmed && !expired(now, _nextPingMs)) return true;
    _pingArmed  = true;
    _nextPingMs = now + (AFFA_SYNC_INTERVAL_MS / 2);
    // data[0] is the ENTIRE test on the receiving side; nothing else in this frame is
    // inspected and nothing else may be.
    sendSync(kSyncPeerAlive, 0, 0, false);
    return true;
  }

  return true;   // capability announces (B0 14 11 …) and anything else: ignored
}

void VirtualPanelBase::onFrame(const Frame& f) {
  // An ACK echo on the reply channel is never for the panel. Checked first, because
  // ctrlId | replyFlag could in principle collide with another family's ctrlId.
  if ((f.id & _p.replyFlag) != 0) return;

  // The sync channel. NEVER ACKed: it has no ACK semantics, and acknowledging it is
  // exactly the 0x7AF incident (WIRE-SPEC §6.1) with the roles reversed.
  if (f.id == _p.syncId) { handleSync(f); return; }

  // Text / control frames: decode the screen, then acknowledge like a real panel. The
  // ACK follows the decode because a real panel has consumed the frame by the time it
  // answers, and a test that asserts on screen() after pumping one cycle depends on it.
  if (f.id == _p.ctrlId || f.id == _p.textId) {
    trackTransfer(f);
    decode(f);
    if (_clock) _lastDecodeMs = _clock->millis();
    autoAck(static_cast<uint16_t>(f.id));
    return;
  }

  // Any other id in the panel's function table — Carminat's 0x1F1 is the one that matters
  // — carries no screen this twin models, but it still gets the 0x70 registration probe
  // and it still has to be acknowledged or the driver's whole first send after a resync
  // times out.
  if (acksId(static_cast<uint16_t>(f.id))) {
    trackTransfer(f);
    autoAck(static_cast<uint16_t>(f.id));
    return;
  }

  // The key id is panel -> radio. We transmit there; we never consume it. Anything else:
  // another node's traffic, and none of our business.
}

bool VirtualPanelBase::transmitKey(uint16_t code, bool hold) {
  if (!_link) return false;
  // 0 means "this family has no key transmit id" — the same convention as
  // AffaDisplayBase::keyTxId(). Transmitting on id 0 would put a frame with the highest
  // possible arbitration priority on the bus, which is the worst available failure.
  if (_p.keyId == 0) return false;

  const uint8_t hi = static_cast<uint8_t>(code >> 8);
  uint8_t       lo = static_cast<uint8_t>(code & 0xFF);

  // THE ENCODER EXEMPTION. 0x40 is simultaneously RollDown's direction bit and half the
  // hold mask, so masking 0x0141 would rewrite it to 0x0101 — every wheel-down detent
  // reported as a wheel-up. The two detent codes are therefore never hold-masked.
  //
  // The consequence, and it is a design constraint rather than a bug: a HELD detent has
  // no wire representation at all. 0x0101|0xC0 and 0x0141|0xC0 are both 0x01C1, which is
  // not exempt and masks back to RollUp+hold. This call is raw and code-level, so
  // transmitKey(0x0101, true) transmits exactly what transmitKey(0x0101, false)
  // transmits; the library's pressKey(RollUp, Hold, Wire) refuses instead, because
  // silently downgrading a coarse step to a fine one is a wrong screen the caller cannot
  // detect. Never build a feature on a held detent.
  const bool isEncoder = (code == kKeyRollUp || code == kKeyRollDown);
  if (hold && !isEncoder) lo = static_cast<uint8_t>(lo | kKeyHoldMask);

  Frame k;
  k.id  = _p.keyId;
  k.len = kPacketLength;
  k.data[0] = kKeyFrameByte0;   // 0x03 — half of the guard that stops the receiving
  k.data[1] = kKeyFrameByte1;   // 0x89 — decoder inventing keys out of `02 64 0F …`
  k.data[2] = hi;
  k.data[3] = lo;
  for (uint8_t i = 4; i < kPacketLength; ++i) k.data[i] = _p.filler;

  return _link->send(k);
}

}  // namespace affa
#endif  // AFFA_ENABLE_VIRTUAL_PANEL
