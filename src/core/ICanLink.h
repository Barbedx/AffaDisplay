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

  // "Is there a working controller at all", as distinct from isLive()'s "may I transmit
  // right now". The two differ for exactly one reason and it matters: a SOFTWARE TX GATE.
  //
  // An application shuts the gate deliberately — for the duration of an OTA write, or to run
  // the bench's is-it-us-or-the-bus test — and isLive() correctly goes false so that renders
  // are held rather than dropped on the floor. But the CONTROLLER is perfectly healthy, and
  // a recovery layer that watched isLive() would tear the driver down in the middle of the
  // flash write it was gated for. So recovery watches THIS, and it ignores the gate.
  //
  // Defaults to isLive(), so a link with no gate needs no override.
  virtual bool healthy() const { return isLive(); }

  // Bring a link that healthy() reports down back into service, and return whether it is up
  // AFTERWARDS — not whether an attempt was made.
  //
  // THE DEFAULT IS "I HAVE NO RECOVERY", which is why it is false and not true: a link that
  // cannot recover must not report that it did, or AffaDisplayBase's backoff would reset on
  // every attempt and spin. Optional with a body so every existing implementation — the
  // loopback, every consumer's own — keeps compiling untouched.
  //
  // Called ONLY from poll(), i.e. from the one task that owns the FSM, and never from a
  // driver callback. It MAY block for the duration of a driver restart (hundreds of ms);
  // that is the one place in this library where that is sanctioned, because the alternative
  // is a controller that is down for ever.
  virtual bool recover() { return false; }

  virtual Stats stats() const { return Stats{}; }
};

} // namespace affa
