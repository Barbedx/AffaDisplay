#pragma once
#include <cstdint>

namespace affa {

// One sync FSM, two panel families: everything family-specific is data here, the code is
// in AffaDisplayBase::pumpSync(). Duplicating the FSM is what let the same two defects
// live twice — a watchdog counting tick() CALLS instead of milliseconds, and a delay(100)
// in the sync-request branch.
//
// WHAT A FIELD IN HERE IS FOR, since six of them have just been deleted. A profile field is
// a place the two families GENUINELY differ, or a measured timing. It is NOT a place to
// record a question nobody had answered yet. Everything below the line
//
//     "did the OEM radio do X?"  ->  yes/no, measured 4/4 in docs/captures/*.csv
//
// belongs in the code as a rule, because a flag holding a settled fact is a seam where the
// two halves of this codebase can disagree — and four separate protocol bugs lived in
// exactly those seams. Removed 2026-08-04 with the facts they were hedging, and where each
// fact now lives:
//
//   authRequestByte2, helloAfterBootstrapRequest, helloOnNonAuthRequest, oneShotResyncOnStart
//                          "does only `61 11 00` authorize, or does `01` count too?"
//                          -> ANY complete `61 11 xx` is the same request; byte 2 is the
//                             panel's own state, not an authorization grade.
//                             AffaDisplayBase::handleSyncFrame().
//   oneShotResyncOnPeerAlive  "may a bare `69` arm the announce?"
//                          -> yes, on a panel-led family, and only before any `61 11`.
//                             AffaDisplayBase::handleSyncFrame(), the kSyncPeerAlive branch.
//   bootstrapAliveFrame    "does the announce lead with B9?"
//                          -> no. `BA` alone asks the question; `B9` only says "still here".
//                             AffaDisplayBase::pumpUnauthControl().
//
// See docs/REFACTOR-PLAN.md for the remaining ones and the order they go in.
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
                          // all over again.
                          //
                          // FALSE on the captured Carminat profile — its B9 is a
                          // free-running 500.08 ms timer (sigma 0.33) that never flinches
                          // as the panel's 69 drifts past it. It is retained because it is
                          // genuinely unsettled for UpdateList, whose reference driver
                          // pongs every `0x69` and for whom that pong was the ONLY
                          // heartbeat until March 2026. If an UpdateList panel ever stalls
                          // in the handshake, this is the first knob to turn.
                          // docs/REFACTOR-PLAN.md, "The one finding to act on first".
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
  // `61 11 xx` must arrive before application traffic is released. AFFA3 NAV sets this
  // true. Keeping it separate from waitForPanel matters: the latter suppresses boot
  // traffic, whereas this keeps an early or stray ping from impersonating authorization.
  //
  // It is also what makes a bare `69` arm the one-shot announce, and what selects the
  // measured opening in handleSyncFrame() over the older UpdateList startup contract.
  // Step 8 of docs/REFACTOR-PLAN.md brings UpdateList onto the same rules and this
  // becomes universal; until then it is the family switch.
  bool requireAuthRequest = false;

  // Minimum spacing between complete hello bursts. Zero uses the conservative generic
  // AFFA_HELLO_MIN_MS. A panel with a proven faster startup cadence carries that measured
  // floor here instead of changing the policy for other panel families.
  //
  // Keep this field at its historical aggregate-initializer position. New profile controls
  // are appended below so a downstream positional SyncProfile initializer that supplied a
  // hello floor continues to mean the same thing.
  uint32_t helloMinMs = 0;

  // Timing belongs to the panel profile, not the application poll rate. A zero keeps the
  // historical immediate burst behaviour. AFFA3 NAV uses a 30/31 ms first-frame and
  // inter-frame spacing, which leaves the panel's `1C1 -> 5C1` control ACK between B0s.
  uint32_t helloFirstDelayMs = 0;
  uint32_t helloFrameGapMs = 0;

  // Delay application/control payloads after the final function-registration ACK. The
  // captured AFFA3 radio waits about 400 ms before `03 52 <on/off>`; zero preserves the
  // existing immediate behaviour for other panel families.
  uint32_t payloadAfterRegistrationMs = 0;

  // Per-profile heartbeat cadence. Zero uses AFFA_SYNC_INTERVAL_MS, retaining the public
  // default for existing profiles. AFFA3 NAV's observed B9/69 cadence is about 500 ms.
  uint32_t syncIntervalMs = 0;

  // Whether the function-registration probes are part of the OPENING or part of RENDERING.
  // The captured AFFA3 radio puts `151 70` and `1F1 70` on the wire 0.10-0.59 ms after the
  // third B0, with no application involvement at all — so a Carminat build that never
  // renders must still register, or the panel sits in a half-open session for ever.
  // UpdateList's reference driver registers lazily, on its first render, so this stays
  // false for that family until step 8 of docs/REFACTOR-PLAN.md moves it over.
  bool registerAfterHello = false;

  // THE RADIO ANNOUNCES ITSELF. In "aknowledge on on display.csv" the first frame on the bus
  // is ours — `3AF B9 01`, unprompted — followed 8.2 ms later by `BA 00`, and only THEN does
  // the display answer `61 11 00`. waitForPanel alone is therefore a deadlock against a
  // display that has gone quiet: it stays silent waiting for us, we stay silent waiting for
  // it, and `rx 0 / tx 0` is exactly what the bench showed.
  //
  // This is the interval between announce pairs while NO panel frame has ever been seen.
  // It is deliberately slow. The defect waitForPanel was introduced to kill was a BA every
  // second into an empty room; a few seconds apart keeps that cured while still waking a
  // sleeping panel. Zero keeps the strictly-silent behaviour for other families.
  uint32_t announceWhenSilentMs = 0;

  // OUR REQUEST COMES FIRST, AND THE HELLO ANSWERS THE *NEXT* PANEL REQUEST. Measured 4/4:
  // in every OEM capture the radio's `BA` precedes the `61 11 xx` that draws the B0 burst,
  // and in "cONNECT OT POWER" the gap is explicit — the display is already repeating
  // `61 11 01`, the radio answers BA, and it is the FOLLOWING request 81 ms later that gets
  // the burst 30.75 ms after it.
  //
  // Answering the first request with the burst skips the exchange the display is waiting on.
  // Confirmed on hardware 2026-08-04: without this the panel never opens its own `1C1`
  // channel and the session dies at "waiting for the display's 1C1", every time; with it,
  // registration, power and the clock all complete unattended.
  //
  // THE ONE KNOB STEP 8 LEAVES STANDING. UpdateList's reference answers the FIRST `61 11`
  // straight out of its receive path with no announce precondition, so it stays false
  // there: our `BA` should provoke a request, but that panel volunteers them anyway.
  bool helloRequiresAnnounce = false;
};

} // namespace affa
