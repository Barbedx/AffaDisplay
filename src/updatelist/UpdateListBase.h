// Everything the AFFA2 family shares below the text encoding: the SyncProfile and function
// table (0x121, 0x1B1), setPower's 0x1B1 payload, the 0x0A9 key channel and the 0x121 inbound
// radio-text sniff.
//
// THE AMS GESTURE WAS HERE AND IS GONE, 2026-08-08. It decided that hold-Load meant "toggle
// whether keys reach the application", swallowed that key so the app never saw it, chose the
// words "AMS  ON" / "AMS OFF", rendered them itself three times 100 ms apart, and then told
// the application to hold off drawing for 300 ms. That is a product decision end to end, and
// this library is a transport. The keys reach onKey(); what a gesture means is the
// application's to decide, in about ten lines.
//
// ABSTRACT ON PURPOSE — no setText and no supports(). The two concrete panels differ in
// exactly the text encoding, and a base answering supports(Feature::Text) == true while
// setText returned NotSupported would be a capability lie.
#pragma once
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListConstants.h"
#include "../core/AffaDisplayBase.h"

namespace affa {

class UpdateListBase : public AffaDisplayBase {
 public:
  UpdateListBase(ICanLink& link, IClock& clock);

  // NAME HIDING, not decoration. The protected `bool onFrame(const Frame&)` below hides
  // EVERY base member called onFrame, including the public Layer-0 tap
  // `void onFrame(FrameTap, void*)`; without this line `display.onFrame(&tap, ctx)` does
  // not compile through an UpdateListDisplay&.
  using AffaDisplayBase::onFrame;

  // 0x1B1: `04 52 <state> FF FF` padded with 0x81. Enqueued on RenderSlot::Control, so it
  // never coalesces against a text render. Asynchronous, like every render call.
  Submitted setPower(bool on) override;

 protected:
  uint8_t  packetFiller() const override { return updatelist::kFiller; }
  uint16_t keyTxId()      const override { return updatelist::kIdKeyPressed; }

  // 0x0A9 key frames and 0x121 radio text. Everything else falls through unconsumed.
  bool onFrame(const Frame& f) override;

  // The one family quirk on top of the base's own suppression list: a MALFORMED key frame
  // (`03` with something other than `89` behind it) is not acknowledged. Every other
  // frame on 0x0A9 — including the panel's own `70` registration probe — is.
  bool shouldAutoAck(const Frame& f) const override;


  // Called when another node (the radio) transmits the segment text encoding on 0x121.
  // `isAux` is a heuristic and nothing more: the first three cells of the sender's "old
  // text" field spell AUX. Exists for the one library-side reaction that is a panel
  // concern — re-asserting our own content after someone else overwrote it.

  // Shared capability table; the two concrete panels differ in no Feature, only in bytes.
  static bool familySupports(Feature f);

  // enqueue() + the Result mapping every render call in this family repeats.
  Submitted enqueueRender(uint16_t funcId, const uint8_t* data, uint8_t len,
                          RenderSlot slot);
};

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
