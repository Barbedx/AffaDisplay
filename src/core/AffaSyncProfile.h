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
  //
  // Keep this field at its historical aggregate-initializer position. New profile controls
  // are appended below so a downstream positional SyncProfile initializer that supplied a
  // hello floor continues to mean the same thing.
  uint32_t helloMinMs = 0;

  // Legacy Carminat replied to every complete `61 11 xx` with its hello burst. The AFFA3
  // monitor captures instead show `01 -> B9/BA -> 00 -> hello`, so a modern profile can
  // keep 01 as discovery-only. Leave this true by default for existing profiles/source
  // compatibility; the captured Carminat profile deliberately sets it false.
  bool helloOnNonAuthRequest = true;

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

  // Some panel-initiated sessions begin with a bare `69` liveness frame, before any full
  // `61 11`. When enabled, the first such frame gets the same bounded B9 -> BA discovery
  // transaction as a START request. It is still NOT authorization: only the profile's good
  // full request can release hello, registration, or application traffic.
  bool oneShotResyncOnPeerAlive = false;

  // MEASURED, NOT ASSUMED. docs/captures/"aknowledge offed display cONNECT OT POWER.csv" is
  // a real OEM radio meeting a display that had been powered with no radio present. That
  // display repeats `3CF 61 11 01` every ~104 ms and NEVER sends `61 11 00` — not once in
  // sixteen requests — yet the radio answers it with the ordinary B0 announce burst 30.75 ms
  // later and the session completes: registration, `03 52`, ISO-TP text, and the `01` never
  // reappears. `61 11 01` is therefore the SAME request as `61 11 00`; the low bit reports
  // the panel's own state, it is not an authorization grade.
  //
  // The gate is our own BA, not the byte: a `01` seen before we have sent BA is the panel
  // calling into an empty bus and earns only the bounded B9 -> BA discovery pair. The first
  // `01` that arrives AFTER that BA is the answer to it, and authorizes exactly as `00`
  // does. See docs/PROTOCOL.md §3.6 for the four-capture timing table.
  bool helloAfterBootstrapRequest = false;

  // Whether the function-registration probes are part of the OPENING or part of RENDERING.
  // The captured AFFA3 radio puts `151 70` and `1F1 70` on the wire 0.10-0.59 ms after the
  // third B0, with no application involvement at all — so a Carminat build that never
  // renders must still register, or the panel sits in a half-open session for ever.
  // UpdateList has a separately validated startup contract in which registration is lazy
  // and follows the first payload, so this stays false for that family. Do not unify.
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

  // Does the one-shot discovery bootstrap lead with an alive frame, or go straight to the
  // request? B9 is the heartbeat of an ESTABLISHED session; on a profile that starts its
  // keep-alive only after registration, emitting one during the opening is noise in the
  // phase that can least afford it, and it muddles the wire log at the exact moment you
  // most want to read it.
  //
  // NOTE THE EVIDENCE IS MIXED, deliberately recorded here rather than smoothed over: two
  // OEM captures DO show B9 before registration ("cONNECT OT POWER" at 147328246,
  // "offed display 2" at 9850470), both consistent with a radio whose free-running 500 ms
  // heartbeat simply ticked during the opening. The reattach capture
  // ("offed display.csv" at 84945066) shows the announce as a bare BA with no B9. We take
  // the quieter reading: BA asks the question, B9 answers "still here" only once there is
  // a session to be still here in.
  bool bootstrapAliveFrame = true;
};

} // namespace affa
