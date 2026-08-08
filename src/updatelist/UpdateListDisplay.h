// The AFFA2 8-segment panel: the 8+12 "old text / new text" segment encoding on 0x121
// (docs/WIRE-SPEC.md §9.1) plus the marquee title scroll, a pure function of
// IClock::millis() driven from onPoll() rather than a call-counted tick.
//
// Artist/title composition and connection state stay OUTSIDE the library; what it owns is
// the eight-cell window and its cadence, so the seam is a string plus a play/pause bit.
#pragma once
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListBase.h"

namespace affa {

class UpdateListDisplay : public UpdateListBase {
 public:
  UpdateListDisplay(ICanLink& link, IClock& clock) : UpdateListBase(link, clock) {}

  bool supports(Feature f) const override { return familySupports(f); }

  // ONE LINE AND NOTHING ELSE. This family is eight segment cells; it has no menu window, no
  // info rows and no image layer, and the zeros say so. It is the reason the geometry is
  // asked for rather than assumed: a fitter written against Carminat's 26-character rows
  // produces nothing this panel can show.
  PanelGeometry panelGeometry() const override {
    PanelGeometry g;
    g.mainChars = updatelist::kOldCells;   // 8
    return g;
  }

  // 0x121, segment encoding. `digit` selects the channel: 0..9 -> 0x70 + digit,
  // anything else (255, the default) -> 0x7A, "no channel". Enqueued on RenderSlot::Text,
  // so a repeated render supersedes a queued one instead of stacking behind it.
  Submitted setText(const char* text, uint8_t digit = 255) override;


 protected:

  // Copy `cells` bytes of `src` into `dst`, padding the tail with NUL — not space. Every
  // capture and every golden vector in docs/WIRE-SPEC.md shows 0x00 there; do not "fix"
  // this to spaces.
  static void copyCells(const char* src, uint8_t* dst, uint8_t cells);

};

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
