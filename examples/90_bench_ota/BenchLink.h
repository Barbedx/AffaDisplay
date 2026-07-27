// BenchLink — the one switch that makes this console work with no transceiver attached.
//
// It is an affa::ICanLink, so the library cannot tell which side of the switch it is on.
// The display holds a BenchLink& for its whole life; the switch is a runtime flag, never a
// second display object and never a driver mode change.
//
//   REAL    : send() -> Esp32CanLink, recv() -> Esp32CanLink. The twin still sees every
//             transmitted frame through the Layer-0 tap and decodes it PASSIVELY, so
//             /api/screen works with a real panel on the bus too.
//   VIRTUAL : send() accepts the frame and puts it nowhere. The frame still reaches the
//             tap (AffaDisplayBase::txFrame observes only frames the link ACCEPTED), the
//             tap feeds the twin, the twin is in EMULATION and answers through its own
//             LoopbackLink, and main.cpp drains that back into inject() here. The
//             controller is not touched at all — there need not be one.
//
// WHY THE TAP AND NOT send(): feeding the twin from inside send() would run panel code
// inside the library's own transmit path. Feeding it from the tap keeps the twin exactly
// where a sniffer would be, which is the position it is designed for, and means the same
// wiring serves both modes.
#pragma once

#include <AffaDisplay.h>

// The software TX gate lives HERE and not in Esp32CanLink::setTxEnabled() only because the
// gate has to cover the virtual path too, where there is no Esp32CanLink in the way.
// Esp32CanLink::setTxEnabled() is the library's own equivalent and is driven in step with
// this one; neither is a driver mode change (docs/ESP32CAN-CONTRACT.md).
class BenchLink final : public affa::ICanLink {
 public:
  explicit BenchLink(affa::Esp32CanLink& hw) : _hw(hw) {}

  bool send(const affa::Frame& f) override {
    if (!_gate) { ++_dropped; return false; }
    if (_virtual) { ++_vTx; return true; }
    return _hw.send(f);
  }

  bool recv(affa::Frame& out) override {
    return _virtual ? _vRx.pop(out) : _hw.recv(out);
  }

  // Gate shut => not live => enqueue() returns LinkDown rather than letting every job fail
  // SendFailed one frame at a time. That is the honest answer and it is also the library's
  // own behaviour when Esp32CanLink's gate is shut.
  bool isLive() const override { return _gate && (_virtual || _hw.isLive()); }

  affa::Stats stats() const override {
    if (!_virtual) {
      affa::Stats s = _hw.stats();
      s.txDropped += _dropped;          // frames this gate refused before the driver saw them
      return s;
    }
    affa::Stats s;
    s.txFrames     = _vTx;
    s.rxFrames     = _vRxIn;
    s.txDropped    = _dropped;
    s.ringOverflow = _vRx.overflow();
    return s;
  }

  // ---- bench side ---------------------------------------------------------
  // Producer: the loop task, draining the twin's LoopbackLink. Consumer: the same task,
  // inside display.poll(). One producer, one consumer, and both are the loop — which is
  // what AffaRing requires. Nothing here may be called from the HTTP task.
  void inject(const affa::Frame& f) { if (_vRx.push(f)) ++_vRxIn; }

  void setVirtual(bool on) {
    if (_virtual == on) return;
    _virtual = on;
    _vRx.reset();                       // stale panel replies must not survive the switch
    _vTx = _vRxIn = 0;
  }
  bool isVirtual() const { return _virtual; }

  void setGate(bool on) { _gate = on; }
  bool gate() const     { return _gate; }

  uint32_t gateDropped() const { return _dropped; }

 private:
  affa::Esp32CanLink&              _hw;
  affa::AffaRing<affa::Frame, 32>  _vRx;
  uint32_t _vTx     = 0;
  uint32_t _vRxIn   = 0;
  uint32_t _dropped = 0;
  bool     _virtual = false;
  bool     _gate    = true;
};
