#pragma once
#include "../core/ICanLink.h"
#include "../core/AffaConstants.h"
#include "../core/AffaRing.h"

namespace affa {

// Header-only test double. Records everything the library transmits, lets a test inject
// what the panel would have said, and can synthesise the per-frame ACK so a test does not
// have to hand-write one per ISO-TP frame.
//
// It does NOT echo transmitted frames back into the RX ring by default. A real controller
// does not receive its own transmissions, and a test double that silently did would make
// every host test disagree with hardware. setEcho(true) turns the echo on deliberately,
// for the tests that exist to prove the Frame::fromSelf rule holds on an echoing link.
template <uint16_t N = 128>
class LoopbackLink final : public ICanLink {
 public:
  bool send(const Frame& f) override {
    if (!_live) { ++_stats.txDropped; return false; }
    if (!_sent.push(f)) { ++_stats.txDropped; return false; }
    ++_stats.txFrames;
    if (_echo) { _rx.push(f); ++_stats.rxFrames; }
    if (_autoAck) synthesiseAck(f);
    return true;
  }
  bool  recv(Frame& out) override { return _rx.pop(out); }
  bool  isLive() const override   { return _live; }

  // The recovery seam, as a test double. Counts every attempt so a test can assert the
  // BACKOFF rather than merely that something happened, and only actually revives the link
  // when the test says so — a link that always recovered could not exercise the path this
  // exists for, which is a controller that stays broken.
  bool recover() override {
    ++_recoverCalls;
    if (!_recoverable) return false;
    _live = true;
    return true;
  }
  Stats stats()  const override   {
    Stats s = _stats;
    s.ringOverflow = _rx.overflow();
    return s;
  }

  // ---- test side ----
  void     inject(const Frame& f) { _rx.push(f); ++_stats.rxFrames; }  // panel -> library
  bool     takeSent(Frame& out)   { return _sent.pop(out); }           // library -> test
  uint32_t sentCount() const      { return _sent.size(); }
  void     setLive(bool v)        { _live = v; }
  void     setEcho(bool v)        { _echo = v; }
  void     setRecoverable(bool v) { _recoverable = v; }
  uint32_t recoverCalls() const   { return _recoverCalls; }

  // Answers each transmitted frame on id|replyFlag. `partialFor` is the number of leading
  // frames of a transfer that are answered PARTIAL (30 01 00) before the DONE (0x74);
  // the default of 0 answers DONE to everything, which is what a single-frame message
  // wants. Set it to (frameCount - 1) to drive a multi-frame transfer to completion.
  void setAutoAck(bool v)              { _autoAck = v; }
  void setAutoAckPartials(uint16_t n)  { _partialsLeft = n; }
  void setAckReplyFlag(uint16_t f)     { _replyFlag = f; }
  void setAckFiller(uint8_t f)         { _ackFiller = f; }

  void clear() {
    _rx.reset();
    _sent.reset();
    _stats = Stats{};
    _partialsLeft = 0;
  }

 private:
  void synthesiseAck(const Frame& f) {
    Frame a;
    a.id  = f.id | _replyFlag;
    a.len = kPacketLength;
    if (_partialsLeft > 0) {
      --_partialsLeft;
      a.data[0] = kAckPartial0;
      a.data[1] = kAckPartial1;
      a.data[2] = kAckPartial2;
      for (uint8_t i = 3; i < kPacketLength; ++i) a.data[i] = _ackFiller;
    } else {
      a.data[0] = kAckDone;
      for (uint8_t i = 1; i < kPacketLength; ++i) a.data[i] = _ackFiller;
    }
    _rx.push(a);
    ++_stats.rxFrames;
  }

  AffaRing<Frame, N> _rx;
  AffaRing<Frame, N> _sent;
  Stats    _stats{};
  uint16_t _replyFlag    = kReplyFlag;
  uint16_t _partialsLeft = 0;
  uint8_t  _ackFiller    = 0xA3;   // what the bench panel actually pads with, so a test
                                   // that wrongly matched on the filler fails loudly
  bool     _live    = true;
  bool     _echo    = false;
  bool     _autoAck = false;
  bool     _recoverable = false;   // OFF by default: a recovery that always works would let
                                   // a broken backoff pass every test
  uint32_t _recoverCalls = 0;
};

} // namespace affa
