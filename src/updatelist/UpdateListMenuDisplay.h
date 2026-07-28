// UpdateListMenuDisplay — the AFFA2 LCD variant.
//
// Same bus, same ids, same sync, same keys, same marquee. The ONLY difference from the
// 8-segment panel is the setText payload: the LCD wants the `10 1C 7F ..` "text + icons"
// form with the packed channel/location bytes, where the segment panel wants `10 19 76 ..`
// (docs/WIRE-SPEC.md §9.2 vs §9.1). Everything else is inherited, deliberately — a second
// copy of the marquee is how the two would drift apart.
//
// AffaConfig promotes AFFA_PANEL_UPDATELIST whenever this variant is selected, so the
// base panel is always present to derive from.
#pragma once
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST_MENU

#include "UpdateListDisplay.h"

namespace affa {

class UpdateListMenuDisplay final : public UpdateListDisplay {
 public:
  UpdateListMenuDisplay(ICanLink& link, IClock& clock) : UpdateListDisplay(link, clock) {
#if AFFA_ENABLE_MARQUEE
    // THE LCD'S VISIBLE FIELD IS WIDER THAN THE SEGMENT PANEL'S. Both encodings carry an
    // 8-cell "old" and a 12-cell "new" field, and the panel shows `new` — but the segment
    // glass is 8 characters while this one renders all 12 (WIRE-SPEC §9.2). Inheriting the
    // 8-wide window would scroll into a 12-cell field and leave four cells always blank.
    setMarqueeGeometry(widget::MarqueeGeometry{updatelist::kNewCells,
                                               updatelist::kScrollGap,
                                               updatelist::kScrollStepMs});
#endif
  }

  // `digit` is IGNORED: the LCD encoding carries a fixed channel byte (0x60 | 0) rather
  // than the segment panel's 0x70 + digit. The parameter stays in the signature because
  // it is IDisplay's and IPanel's, and dropping it here would break the one-override-
  // satisfies-both-bases arrangement AffaDisplayBase depends on.
  [[nodiscard]] Result setText(const char* text, uint8_t digit = 255) override;
};

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST_MENU
