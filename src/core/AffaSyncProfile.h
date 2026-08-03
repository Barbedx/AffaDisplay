#pragma once
#include <cstdint>

namespace affa {

// One sync FSM, two panel families: everything family-specific is data here, the code is
// in AffaDisplayBase::pumpSync(). Duplicating the FSM is what let the same two defects
// live twice — a watchdog counting tick() CALLS instead of milliseconds, and a delay(100)
// in the sync-request branch.
struct SyncProfile {
  uint16_t syncId;        // Carminat 0x3AF   UpdateList 0x3DF   (we transmit here)
  uint16_t syncReplyId;   // both 0x3CF                          (panel transmits here)
  uint16_t replyFlag;     // both 0x400
  uint8_t  aliveByte;     // 0xB9 / 0x79   heartbeat, data[0]
  uint8_t  requestByte;   // 0xBA / 0x7A   sync request, data[0]
  uint8_t  requestArg;    // Carminat 0x00, UpdateList 0x01 — data[1] of the request.
                          // DO NOT "harmonise" these: Carminat's `BA 00 00 …` is 0xBA
                          // followed by seven filler bytes that merely happen to be
                          // 0x00, whereas UpdateList's 0x01 is a genuine argument. The
                          // wire looks symmetrical and is not.
  uint8_t  filler;        // 0x00 / 0x81   pads every frame we build. data[1] of the
                          // HEARTBEAT is a literal 0x00 in both families and is NOT the
                          // filler: UpdateList sends `79 00 81 81 …`, not `79 81 81 …`.
  const uint8_t (*hello)[8];  // frames sent in reply to `61 11`, in order
  uint8_t  helloCount;    // Carminat 3 (the second and third are IDENTICAL — two sendCan
                          // calls in the legacy source, and present in the capture),
                          // UpdateList 1
  bool replyToPing = false;   // ANSWER the panel's `69` ping with an immediate heartbeat,
                          // in addition to the paced one. MeganeCAN's Carminat driver —
                          // the implementation proven against a real panel — calls tick()
                          // from its 0x69 handler (CarminatDisplay.cpp:346), so its B9
                          // follows the ping within milliseconds and reads as a REPLY on
                          // the wire; a paced-only B9 merely transmits nearby. Nobody has
                          // a spec for the panel, so whether it TREATS B9 as an answer is
                          // unknown — this flag exists so the Carminat profile can be
                          // wire-identical to the driver that worked, without dragging
                          // the legacy tick()'s other two defects back in (the extra BA
                          // and the watchdog re-armed from the peer; both stay gone).
                          // Paced by AFFA_PING_REPLY_MIN_MS: a storming panel repeats 69
                          // at line rate, and one pong per ping is the 4400-frames/s trap
                          // all over again. Carminat true, UpdateList/Cluster false.
  // A Carminat panel is the session initiator.  While this is true the library is a
  // completely silent CAN participant after begin(): no heartbeat and, importantly, no
  // `BA` probe leave until a valid message from the panel arrives on syncReplyId.  The
  // legacy polling driver happened to start in FAILED and therefore emitted BA once a
  // second before the panel had said anything.  That is not part of the panel-initiated
  // authorization exchange and is actively harmful on a shared or waking bus.
  bool waitForPanel = false;

  // Whether this profile is allowed to initiate recovery with requestByte once it has
  // observed the panel.  AFFA3 NAV does not: a fresh `61 11` is the authoritative request
  // to which we answer hello and then register.  Kept profile-specific because the older
  // UpdateList family has a different, separately validated startup contract.
  bool sendSyncRequest = true;

  // `69` is only a liveness ping on profiles with this set. It is evidence that a panel
  // is present, but it is NOT permission to register functions or render. A complete
  // `61 11 <authRequestByte2>` must arrive before application traffic is released. A
  // profile may still answer a ping after a different complete `61 11 xx` bootstrap; that
  // is separate from authorization. AFFA3 NAV sets this true. Keeping it separate from
  // waitForPanel matters: the latter suppresses boot traffic, whereas this keeps an early
  // or stray ping from impersonating authorization.
  bool requireAuthRequest = false;

  // data[2] required by the above authorization gate. It is ignored unless
  // requireAuthRequest is true. Carminat releases registration and rendering only after
  // `61 11 00`. Its legacy driver still answers every complete `61 11 xx` with hello.
  uint8_t authRequestByte2 = 0x00;

  // Some panel-initiated profiles use `61 11 01` as a bootstrap / START indication. If
  // set, the base sends ONE bounded alive + request pair after that indication. This is
  // deliberately independent of sendSyncRequest: it preserves Carminat's proven bootstrap
  // without restoring the old BA-every-second recovery storm. It never authorizes payload
  // traffic; only authRequestByte2 above does that.
  bool oneShotResyncOnStart = false;

  // Minimum spacing between complete hello bursts. Zero uses the conservative generic
  // AFFA_HELLO_MIN_MS. A panel with a proven faster startup cadence carries that measured
  // floor here instead of changing the policy for other panel families.
  uint32_t helloMinMs = 0;
};

} // namespace affa
