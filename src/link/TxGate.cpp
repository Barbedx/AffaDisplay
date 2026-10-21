// SPDX-License-Identifier: MIT
#include "TxGate.h"

namespace affa {

static bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

void TxGate::close(uint32_t nowMs)
{
  _shut = true;
  _probing = false;
  _reopenAtMs = nowMs + _backoffMs;
  ++_shuts;
}

TxGate::Act TxGate::tick(const Sample& s)
{
  if (!_primed) {
    // The first sample only sets the baseline: "nothing heard" is counted
    // from here, not from boot.
    _primed = true;
    _rxWas = s.rxFrames;
    _txFailWas = s.txFailed;
    _tecWas = s.tec;
    _lastPeerMs = s.nowMs;
    return Act::None;
  }

  const bool heard = s.rxFrames != _rxWas;
  _rxWas = s.rxFrames;
  // Greater, not different: a reinstall starts the controller's count at 0.
  const bool failRose = s.txFailed > _txFailWas;
  _txFailWas = s.txFailed;
  // Only an ACKed frame takes TEC down: somebody is there, if silent.
  const bool acked = s.tec < _tecWas;
  _tecWas = s.tec;
  if (heard || acked) _lastPeerMs = s.nowMs;

  if (!s.mayTx) {
    _probing = false;
    _backoffMs = BACKOFF_MIN_MS;
    if (!_shut) return Act::None;
    _shut = false;
    return Act::Open;
  }

  if (_shut) {
    // Somebody is talking: there is a node to ACK us again.
    if (heard) {
      _shut = false;
      _backoffMs = BACKOFF_MIN_MS;
      return Act::Open;
    }
    if (!reached(s.nowMs, _reopenAtMs)) return Act::None;
    _shut = false;
    _probing = true;
    _probeFailed = false;
    _probeAcked = false;
    _tecAtProbe = s.tec;
    _probeAtMs = s.nowMs;
    _probeUntilMs = s.nowMs + PROBE_MS;
    return Act::Open;
  }

  if (_probing) {
    if (heard) {
      _probing = false;
      _backoffMs = BACKOFF_MIN_MS;
      return Act::None;
    }
    if (failRose) _probeFailed = true;
    if (acked) _probeAcked = true;
    // Nobody ACKed: TEC climbed (8 per unACKed frame while error active),
    // or it sits at error passive, where ACK errors stop counting, and not
    // one frame took it down.
    const bool tecRose  = s.tec > _tecAtProbe;
    const bool tecStuck = s.tec >= TEC_PASSIVE && s.tec >= _tecAtProbe;
    // Early verdict: while the window is open an unACKed frame is
    // retransmitted at full rate, ~1400 bus-error interrupts a second on
    // the key core. A failure or a climbing TEC says nobody at once; a
    // stuck one after PROBE_STUCK_MS, unless any frame was ACKed (TEC
    // fell) meanwhile: a slow listener gets the whole window.
    const bool early = !_probeAcked &&
               (_probeFailed || tecRose ||
              (tecStuck && reached(s.nowMs, _probeAtMs + PROBE_STUCK_MS)));
    if (!early && !reached(s.nowMs, _probeUntilMs)) return Act::None;
    _probing = false;
    if (_probeFailed || tecRose || tecStuck) {
      const uint32_t next = _backoffMs * 2;
      _backoffMs = next > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : next;
      close(s.nowMs);
      return Act::Shut;
    }
    // Our frames went out and somebody ACKed them: a listener, not silence.
    // It may still be error passive for a few frames; the TEC it is taking
    // down keeps _lastPeerMs fresh, so the rule below leaves it open.
    _backoffMs = BACKOFF_MIN_MS;
    return Act::None;
  }

  if ((s.tec >= TEC_PASSIVE || failRose) &&
    (uint32_t)(s.nowMs - _lastPeerMs) >= NO_RX_MS) {
    close(s.nowMs);
    return Act::Shut;
  }
  return Act::None;
}

}  // namespace affa
