// The AFFA2 panel — BOTH GLASSES, one class.
//
// Same bus, same ids, same sync, same keys, same registration. The only thing that differs
// between the eight-segment display and the LCD is the setText HEADER, and it is a header
// and not a width:
//
//   segment  10 19 76 <chan> 01           old(8) 10 new(12) 00 81 81    29 bytes
//   LCD      10 1C 7F 55 55 FF 60 03      old(8) 10 new(12) 00          30 bytes
//
// `0x76` is the text-only variant of the command and `0x7F` is text-plus-icons, so the LCD
// form carries three icon bytes the segment form has no room for. THE TEXT FIELDS ARE
// IDENTICAL — both carry an 8-cell "old" and a 12-cell "new" — which is what makes one
// builder possible; the headers are what stop it being one encoding.
//
// WHY A CONSTRUCTOR ARGUMENT AND NOT A SUBCLASS. UpdateListMenuDisplay was a whole file and
// a build gate (AFFA_PANEL_UPDATELIST_MENU) to override exactly one method, and its name
// lied: nothing about it was a menu. A variant that differs by eight header bytes is data,
// not a type.
//
// AND THE TWO ENCODINGS ARE NOT MERGED, deliberately. Picking one for both panels would mean
// choosing an encoding we can only test on one of them: the segment glass is bench-verified,
// the LCD has never been on a bench here. Breaking a proven family for an unproven one is the
// wrong direction to be wrong in, so both byte sequences stay exactly as captured.
#pragma once
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListBase.h"

namespace affa {

// WHICH GLASS is on the bus. Chosen at construction because it is a property of the
// hardware, not a mode: no panel turns into the other one at runtime.
enum class UpdateListGlass : uint8_t {
  Segment = 0,   // eight segment cells. Bench-verified 2026-08-04.
  Lcd     = 1,   // the wider LCD. Golden vectors only — no hardware here has ever seen it.
};

class UpdateListDisplay : public UpdateListBase {
 public:
  explicit UpdateListDisplay(ICanLink& link, IClock& clock,
                             UpdateListGlass glass = UpdateListGlass::Segment)
      : UpdateListBase(link, clock), _glass(glass) {}

  bool supports(Feature f) const override { return familySupports(f); }

  UpdateListGlass glass() const { return _glass; }

  // WHAT REACHES THE GLASS, and it differs by variant. Both encodings carry a 12-cell "new"
  // field and the panel shows it, but the segment display renders eight of those cells and
  // the LCD renders all twelve — so a caller fitting text has to ask, which is exactly what
  // panelGeometry() is for.
  //
  // Everything else is zero: this family has no menu window, no info rows and no image
  // layer, and the zeros say so. A fitter written against Carminat's 26-character rows
  // produces nothing either of these panels can show.
  PanelGeometry panelGeometry() const override {
    PanelGeometry g;
    g.mainChars = (_glass == UpdateListGlass::Lcd) ? updatelist::kNewCells    // 12
                                                   : updatelist::kOldCells;   // 8
    return g;
  }

  // 0x121. `digit` selects the channel on the SEGMENT encoding: 0..9 -> 0x70 + digit,
  // anything else (255, the default) -> 0x7A, "no channel".
  //
  // IT IS IGNORED ON THE LCD, whose header carries a fixed 0x60 channel byte instead. The
  // parameter stays in the signature because it is IDisplay's and IPanel's, and dropping it
  // would break the one-override-satisfies-both-bases arrangement AffaDisplayBase depends on.
  //
  // Enqueued on RenderSlot::Text, so a repeated render supersedes a queued one instead of
  // stacking behind it.
  Submitted setText(const char* text, uint8_t digit = 255) override;

 protected:
  // Copy `cells` bytes of `src` into `dst`, padding the tail with NUL — not space. Every
  // capture and every golden vector in docs/WIRE-SPEC.md shows 0x00 there; do not "fix"
  // this to spaces.
  static void copyCells(const char* src, uint8_t* dst, uint8_t cells);

 private:
  UpdateListGlass _glass;
};

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
