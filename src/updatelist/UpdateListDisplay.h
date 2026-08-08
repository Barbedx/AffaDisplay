// The AFFA2 / UpdateList panel — ONE encoding, every glass.
//
// The eight-segment display and the wider LCD are DIFFERENT GLASS ON THE SAME BUS, and the
// radio does not know which one is out there. It emits one frame and each panel renders what
// it can: the segment shows eight cells, the LCD shows all twelve. Nothing in a SENDER can
// branch on a fact the sender is never told.
//
// This is a correction, and the evidence is worth keeping because the mistake was mine.
// I previously shipped two encodings as two "glasses":
//
//   0x76 form   10 19 76 <chan> 01           old(8) 10 new(12) 00 81 81    29 bytes
//   0x7F form   10 1C 7F 55 55 FF 60 03      old(8) 10 new(12) 00          30 bytes
//
// BYTE [2] IS A FLAVOUR, NOT A PANEL SELECTOR. Three values have been seen in that one
// position across independent projects — 0x76, 0x7E and 0x7F — and both the 0x76 and the
// 0x7F forms have been driven into UpdateList displays by separate people:
//
//   0x76  abecikxp/RenaultUpdateListDisplay lcd.ino, data[2]=0x76, and OUR bench panel
//   0x7E  hackaday.io/project/27439 part 2, "0x10, 0x19, ~, q, 0x01"  (~ = 0x7E, q = 0x71)
//   0x7F  hackaday.io/project/27439 part 3, "121 ... 10:1C:7F:55:55:3F:60:01", the author's
//         own board driving the display with no Renault radio attached
//
// So the split was never segment-versus-LCD. 0x7F is the TEXT-PLUS-ICONS flavour of the same
// command, which is why it carries three extra bytes.
//
// WHY THE 0x7F FORM IS GONE RATHER THAN KEPT AS AN OPTION:
//   - it buys no capability we expose. This driver has never set an icon, so both flavours
//     put the same characters on the same glass;
//   - our copy of it was [REF] — reconstructed from archive affa3 source, never transmitted
//     from here — and the ONE real capture of it disagrees with our bytes in two places:
//     [5] was 0x3F where we wrote 0xFF, [7] was 0x01 where we wrote 0x03;
//   - the 0x76 form is bench-verified here and shipping in an independent project.
//
// Eight header bytes of unproven, uncapturable, capability-free risk are not a feature.
// If icons are ever exposed, the flavour comes back as an ICON argument with the capture's
// bytes, not as a panel type. docs/WIRE-SPEC.md §9.2 keeps the record.
#pragma once
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListBase.h"

namespace affa {

class UpdateListDisplay : public UpdateListBase {
 public:
  UpdateListDisplay(ICanLink& link, IClock& clock) : UpdateListBase(link, clock) {}

  bool supports(Feature f) const override { return familySupports(f); }

  // EIGHT, and it is a promise rather than a measurement.
  //
  // The frame always carries a 12-cell "new text" field, so a wider glass shows more for
  // free — but the sender cannot tell which glass answered, and eight cells are what every
  // panel in this family is known to render. A fitter that trusts 12 produces text the
  // segment display silently truncates; one that trusts 8 is correct everywhere.
  //
  // The rest is zero: no menu window, no info rows, no image layer, and the zeros say so.
  PanelGeometry panelGeometry() const override {
    PanelGeometry g;
    g.mainChars = updatelist::kOldCells;   // 8 — guaranteed visible, not the field width
    return g;
  }

  // 0x121. `digit` selects the channel: 0..9 -> 0x70 + digit, anything else (255, the
  // default) -> 0x7A, "no channel".
  //
  // Enqueued on RenderSlot::Text, so a repeated render supersedes a queued one instead of
  // stacking behind it.
  Submitted setText(const char* text, uint8_t digit = 255) override;

 protected:
  // Copy `cells` bytes of `src` into `dst`, padding the tail with NUL — not space. Every
  // capture and every golden vector in docs/WIRE-SPEC.md shows 0x00 there; do not "fix"
  // this to spaces.
  static void copyCells(const char* src, uint8_t* dst, uint8_t cells);
};

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
