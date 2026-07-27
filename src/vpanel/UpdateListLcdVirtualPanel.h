// The UpdateList monochrome-LCD twin.
//
// Same identifiers as the 8-segment panel (it reuses updateListProfile()); the difference
// is only in the screen payload. UpdateListMenuDisplay overrides setText with the
// "text plus icons" form from the third-party affa3 library, so the LCD panel's text
// arrives on 0x121 as command 0x7F with the 30-byte layout of docs/WIRE-SPEC.md §9.2 —
// NOT as the 0x76 form the 8-segment driver emits, and not as a Carminat-style 96-byte
// screen.
//
// PROVISIONAL, AND HERE IS EXACTLY HOW MUCH SO. The protocol mechanics this class inherits
// (auto-ACK, sync reply, key transmit, reassembly) are shared with the other two twins and
// are exercised by them. What is NOT confirmed against a real mono UpdateList panel is
// whether it renders anything BEYOND the 8+12 text cells — whether it has a menu screen,
// a highlight, a scroll indicator. The reference twin guessed that it reused the Carminat
// 96-byte showMenu layout; that guess is retained below as a last-resort fallback, clearly
// marked, because our own driver has never emitted such a payload on 0x121 and so the
// guess has never once been exercised. Validate against hardware before trusting it.
#pragma once

#include "../AffaConfig.h"
#if AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_UPDATELIST_MENU

#include "UpdateListSegVirtualPanel.h"   // reuses updateListProfile()

namespace affa {

class UpdateListLcdVirtualPanel final : public VirtualPanelBase {
 public:
  UpdateListLcdVirtualPanel() : VirtualPanelBase(updatelist_twin::kProfile) {}

 protected:
  void decode(const Frame& f) override;
};

}  // namespace affa
#endif  // AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_UPDATELIST_MENU
