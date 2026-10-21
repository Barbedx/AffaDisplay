#pragma once
#include <stdint.h>

namespace affa {

// When to stop transmitting to a panel that is not there. Portable: the host
// tests drive it with numbers, and TwaiLink::service() feeds it
// twai_get_status_info() every 100 ms. Moved here from MegaOpen, where it ran
// on the panel task beside an application-side link.
//
// A frame nobody ACKs is retransmitted by the controller for ever. With the
// panel unplugged that is ~1400 bus errors a second, each an interrupt on
// core 1 — the key core — for nothing. So when the controller says nobody is
// ACKing (TEC at error passive, or frames failing) AND for NO_RX_MS nothing
// has been heard and nothing we sent was ACKed, the gate shuts: the link
// turns transmit off and aborts the frame in flight. A panel that is
// plugged back in talks first, and the first frame heard opens the gate at
// once.
//
// While shut it also re-probes on its own, in case the panel only listens:
// it opens for PROBE_MS, and if nothing was heard and the controller still
// says nobody ACKs, it shuts again for twice as long (1, 2, 4 ... 30 s).
// Clear evidence ends the probe early (frames failing or TEC climbing at
// once, TEC stuck at error passive with nothing ACKed after PROBE_STUCK_MS),
// so the retransmit storm lasts a fraction of the window.
//
// "Nobody ACKs" is read from the transmit error counter (TEC) and its
// direction, never from REC: an unACKed frame adds 8 while error active, and
// nothing once error passive (that is why it never goes bus-off), while each
// ACKed frame takes 1 off. So a node that reached a listener can still be
// error passive for a while; a falling TEC is the evidence, not its level.
// REC says how receiving went, and with nobody talking it cannot fall at all.
//
// The counters it is fed are totals since boot; a driver reinstall that
// resets the controller's own counters only looks like "nothing failed", or
// once like an ACK (TEC back to 0): at worst a 2 s later shut.
class TxGate {
public:
  static constexpr uint32_t NO_RX_MS       = 2000;
  static constexpr uint32_t PROBE_MS       = 2000;
  // A probe whose TEC sits at error passive with nothing ACKed is given up
  // after this, not PROBE_MS: the window is a retransmit storm.
  static constexpr uint32_t PROBE_STUCK_MS = 500;
  static constexpr uint32_t BACKOFF_MIN_MS = 1000;
  static constexpr uint32_t BACKOFF_MAX_MS = 30000;
  static constexpr uint32_t TEC_PASSIVE    = 128;

  struct Sample {
    uint32_t nowMs      = 0;
    uint32_t tec        = 0;       // the controller's tx_error_counter
    uint32_t txFailed   = 0;       // the controller's tx_failed_count
    uint32_t rxFrames   = 0;       // every frame received, since boot
    // False when transmit is not ours to gate right now (listen-only, no
    // driver): the gate opens and forgets what it was waiting for.
    bool     mayTx      = true;
  };

  enum class Act : uint8_t { None, Shut, Open };

  // What to do now. The caller applies Shut/Open to the bus.
  Act tick(const Sample& s);

  bool     shut() const      { return _shut; }
  bool     probing() const   { return _probing; }
  uint32_t shuts() const     { return _shuts; }
  uint32_t backoffMs() const { return _backoffMs; }

private:
  void close(uint32_t nowMs);

  bool     _primed = false;
  bool     _shut = false;
  bool     _probing = false;
  bool     _probeFailed = false;
  bool     _probeAcked = false;   // TEC fell during this probe: somebody ACKs
  uint32_t _rxWas = 0, _txFailWas = 0, _tecWas = 0;
  uint32_t _tecAtProbe = 0, _probeAtMs = 0;
  // The last sign of another node: a frame heard, or one of ours ACKed.
  uint32_t _lastPeerMs = 0;
  uint32_t _reopenAtMs = 0, _probeUntilMs = 0;
  uint32_t _backoffMs = BACKOFF_MIN_MS;
  uint32_t _shuts = 0;
};

}  // namespace affa
