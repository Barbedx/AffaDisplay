#pragma once
#include "AffaTypes.h"

namespace affa {

// The CAN seam, deliberately a PULL port.
//
// The obvious design is a push callback (onReceive(cb)), and that is what the code this
// library was extracted from used. It is also what made the ACK deadlock possible: the
// protocol layer blocked waiting for a frame that only a push from somewhere else could
// deliver, and the drain was downstream of the spin. With recv(), the protocol layer owns
// its own drain and cannot wait on a thing it is itself preventing.
struct ICanLink {
  virtual ~ICanLink() = default;

  // Hand one frame to the controller. MUST NOT BLOCK. Returns false if the frame was not
  // accepted (TX gate closed, driver queue full, bus off). Never retries.
  virtual bool send(const Frame& f) = 0;

  // Pop one buffered received frame. Returns false when the buffer is empty.
  // MUST NOT BLOCK. Called in a tight loop by AffaDisplayBase::poll().
  virtual bool recv(Frame& out) = 0;

  // False disables all transmission at the protocol layer: enqueue() returns LinkDown and
  // in-flight jobs complete LinkDown. Default true.
  virtual bool isLive() const { return true; }

  virtual Stats stats() const { return Stats{}; }
};

} // namespace affa
