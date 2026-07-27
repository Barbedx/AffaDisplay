// Heuristic inference of which SOURCE a Renault radio is playing, by pattern-matching the
// text it puts on the panel's text channel.
//
// READ THIS BEFORE YOU USE IT.
//
// These patterns describe ONE RENAULT RADIO FAMILY — the head unit that happened to be on
// the bench bus — and not the panel, not the protocol, and not your car. Every other
// AFFA_ENABLE_* gate describes something the PANEL defines and is on by default; this one
// describes someone else's product, so it is OFF by default (AFFA_ENABLE_AUX_TRACKER) and
// an application is expected to write its own policy against EventKind::RadioText:
//
//     static void onEvent(const affa::Event& ev, void* ctx) {
//       if (ev.kind != affa::EventKind::RadioText) return;
//       // ev.text.text is NUL-terminated and already transliterated;
//       // ev.text.raw is the reassembled payload. BOTH DIE WHEN THIS RETURNS.
//     }
//
// It is kept because the patterns cost real bench time to find, not because they are
// universal, and because throwing away working reverse-engineering would be worse than
// shipping it honestly labelled. Its verdict is a GUESS, and it must never be treated as
// a protocol fact.
//
// NOTHING IN THE LIBRARY DEPENDS ON IT. The display does not feed it — that was the whole
// change. You wire it up yourself:
//
//     affa::AuxModeTracker aux(clock);
//     static void feedAux(const affa::Frame& f, void* c) {
//       static_cast<affa::AuxModeTracker*>(c)->onFrame(f);
//     }
//     affa::FrameMatch m{};
//     m.id  = 0x151;                 // len stays 0: id only, every frame on 0x151
//     m.dir = affa::Direction::Rx;   // what the RADIO sent, never our own echo
//     display.subscribe(m, &feedAux, &aux);
//
// It works on RAW FRAMES rather than on reassembled text because the discriminator
// includes header byte 6 of the 0x10 frame — the setText format byte, where >= 0x59 marks
// plain ASCII and < 0x59 marks the radio-digit style — which the string form does not
// carry. That is also why it needs a clock: a 0x21 continuation is only trusted within
// 200 ms of the 0x10 header it is paired with.
#pragma once
#include "../AffaConfig.h"

#if AFFA_ENABLE_AUX_TRACKER

#include "../core/AffaTypes.h"
#include "../core/IClock.h"
#include <cstdint>

namespace affa {

// The Carminat text channel. Spelled out here rather than pulled from
// carminat/CarminatConstants.h so the tracker compiles in a build that selected another
// panel — it is a radio heuristic, not a Carminat capability.
inline constexpr uint32_t kAuxWatchId = 0x151;

// How long a 0x10 header stays valid for the 0x21 that follows it.
inline constexpr uint32_t kAuxPairWindowMs = 200;

class AuxModeTracker {
 public:
  using ChangeCb = void (*)(bool aux, void* ctx);

  explicit AuxModeTracker(IClock& clock) : _clock(clock) {}

  AuxModeTracker(const AuxModeTracker&)            = delete;
  AuxModeTracker& operator=(const AuxModeTracker&) = delete;

  // Feed it every frame on kAuxWatchId; it ignores everything else, including short DLCs.
  void onFrame(const Frame& f);

  bool inAux() const { return _aux; }

  // Force the verdict — from a web UI, a test, or an application that knows better than
  // the heuristic does. Does NOT fire ChangeCb: this is the caller telling us, not us
  // telling the caller.
  void setAux(bool v) { _aux = v; }

  void onChange(ChangeCb cb, void* ctx) { _cb = cb; _ctx = ctx; }

 private:
  bool classify(const uint8_t* head, const uint8_t* text) const;

  IClock&  _clock;
  bool     _aux          = false;
  bool     _haveHeader   = false;   // NOT just a zero timestamp: at boot, millis() < 200
                                    // makes a stale `0 + 200` window look open
  uint8_t  _header[8]    = {0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t _headerMs     = 0;
  ChangeCb _cb           = nullptr;
  void*    _ctx          = nullptr;
};

}  // namespace affa

#endif  // AFFA_ENABLE_AUX_TRACKER
