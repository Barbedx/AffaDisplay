#pragma once
#include "AffaTypes.h"

namespace affa {

// What happened to one non-blocking transmit offer.  `Accepted` means the link took
// ownership of the frame (usually by putting it in the controller TX queue), not that the
// panel has acknowledged the protocol payload.  `Busy` is deliberately separate from a
// hard refusal: a running controller can have a locally full queue for a moment, and the
// protocol FSM must leave its bytes uncommitted and try again later.  `Rejected` means the
// link did not take the frame and retrying immediately would not help (gate shut, driver
// stopped, bus-off, malformed frame, ...).
enum class TxDisposition : uint8_t { Accepted, Busy, Rejected };

// What ICanLink::waitRx() did.
enum class RxWait : uint8_t { Unsupported, Ready, Timeout };

// The CAN seam, deliberately a PULL port.
//
// The obvious design is a push callback (onReceive(cb)), and that is what the code this
// library was extracted from used. It is also what made the ACK deadlock possible: the
// protocol layer blocked waiting for a frame that only a push from somewhere else could
// deliver, and the drain was downstream of the spin. With recv(), the protocol layer owns
// its own drain and cannot wait on a thing it is itself preventing.
struct ICanLink {
  virtual ~ICanLink() = default;

  // Hand one frame to the controller. MUST NOT BLOCK, never retries. `Busy` is a transient
  // local shortage (a full driver queue), which AffaDisplayBase retries without consuming
  // payload bytes or a retry attempt; `Rejected` is a link that cannot send right now.
  virtual TxDisposition trySend(const Frame& f) = 0;

  // Pop one buffered received frame. Returns false when the buffer is empty.
  // MUST NOT BLOCK. Called in a tight loop by AffaDisplayBase::poll().
  virtual bool recv(Frame& out) = 0;

  // False disables all transmission at the protocol layer: enqueue() returns LinkDown and
  // in-flight jobs complete LinkDown. Default true.
  virtual bool isLive() const { return true; }

  // "Is there a working controller at all", as distinct from isLive()'s "may I transmit
  // right now". The two differ for exactly two reasons and both matter: a SOFTWARE TX
  // GATE, and a LISTEN-ONLY driver mode.
  //
  // An application shuts the gate deliberately — for the duration of an OTA write, or to run
  // the bench's is-it-us-or-the-bus test — or drops to listen-only to observe a bus without
  // acknowledging it, and isLive() correctly goes false so that renders are held rather
  // than dropped on the floor. But the CONTROLLER is perfectly healthy, and a recovery
  // layer that watched isLive() would tear the driver down in the middle of the flash
  // write it was gated for. So recovery watches THIS, and it ignores both.
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
  // `force` says the CALLER has evidence the link is broken that the link cannot see in its
  // own state — the RUNNING-but-deaf case, where the controller reports itself perfectly
  // healthy while reception has stopped (AFFA_RX_STALL_MS). Without it a recover() that
  // short-circuits on "already running" — which is the correct answer for the bus-off race
  // it was built for — would return success without touching the one thing that can help,
  // the driver reinstall.
  //
  // Called ONLY from poll(), i.e. from the one task that owns the FSM, and never from a
  // driver callback. It MAY block for the duration of a driver restart (hundreds of ms);
  // that is the one place in this library where that is sanctioned, because the alternative
  // is a controller that is down for ever.
  virtual bool recover(bool force = false) { (void)force; return false; }

  // Block the calling task until a frame is ready to recv() or `ms` has passed — what lets an
  // owner task sleep between polls and still wake the moment a panel key arrives. A link that
  // cannot wait returns Unsupported at once and the caller sleeps on its own; Ready and
  // Timeout both mean the link did the sleeping. Owner task only.
  virtual RxWait waitRx(uint32_t ms) { (void)ms; return RxWait::Unsupported; }

  // Once per poll(), from the task that owns the FSM, before anything is received. For the
  // housekeeping a link cannot do from inside recv()/trySend() — TwaiLink samples the
  // controller here and shuts transmit when nobody ACKs. MUST NOT BLOCK.
  virtual void service(uint32_t nowMs) { (void)nowMs; }

  virtual Stats stats() const { return Stats{}; }
};

} // namespace affa
