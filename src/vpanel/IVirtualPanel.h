// A faithful TWIN of a real AFFA panel.
//
// It consumes what our radio side transmits, reassembles the ISO-TP, decodes the screen,
// and in EMULATION mode answers the way the panel does: auto-ACK on `id | replyFlag`,
// sync replies on 0x3CF, key frames back on the key id. That buys two things nothing else
// can:
//
//   * a SEMANTIC test oracle — "the panel shows header 'MENU', row 0 'Bright: 50%'",
//     rather than a byte comparison that passes for the wrong reason;
//   * a development loop with NO PANEL ATTACHED AT ALL.
//
// The twin must stay an INDEPENDENT WITNESS. Its offsets come from docs/WIRE-SPEC.md, not
// from our encoder. If it and a render call disagree by a byte, that is a finding about
// one of the two, and the way to settle it is a capture — never by moving a constant in
// proto/ until the test goes green.
//
// It is FED frames and never drains a link itself. That is what keeps one implementation
// usable in three places without modification: a host harness over two LoopbackLinks, a
// bench ESP32 sitting PASSIVE on a real bus, and a replay of a recorded capture.
#pragma once

#include "../AffaConfig.h"
#if AFFA_ENABLE_VIRTUAL_PANEL

#include "../core/AffaTypes.h"
#include "../core/ICanLink.h"
#include "../core/IClock.h"
#include "../proto/ScreenModel.h"

namespace affa {

struct IVirtualPanel {
  virtual ~IVirtualPanel() = default;

  // The twin transmits its ACKs, sync replies and keys through this link. Returns false
  // if it was already begun with a different link, or the arguments are unusable.
  virtual bool begin(ICanLink& link, IClock& clock) = 0;

  // Feed it one frame that the OTHER node transmitted. The harness owns the wiring.
  virtual void onFrame(const Frame& f) = 0;

  // Transmit a key, panel -> radio. `code` is the raw wire code (the value of Key).
  //
  // Named transmitKey and NOT pressKey: on the twin this is only the WIRE half, and
  // reusing the name of AffaDisplayBase::pressKey — which by default also has a LOCAL
  // effect — is exactly the lookalike-function trap KeySource exists to close.
  virtual bool transmitKey(uint16_t code, bool hold) = 0;

  virtual const ScreenModel& screen() const = 0;
};

}  // namespace affa
#endif  // AFFA_ENABLE_VIRTUAL_PANEL
