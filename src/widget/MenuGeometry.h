// The shape of the display the menu is drawn on — injected, never assumed:
//
//     Carminat menu screen   rows = 2   rowChars = 26   (the 0x21 two-row window)
//     Carminat info screen   rows = 3   rowChars = 8    (showInfoPopup)
//     a 4" OLED              rows = 6+  rowChars = 20+
//
// Nothing else about the panel belongs here. Row TAGS (Carminat 0x7E / 0x7F), highlight
// frames, arrow glyphs and charset are the adapter's business — see IMenuRenderer.h.
#pragma once
#include "../AffaConfig.h"

#if AFFA_ENABLE_MENU

#include <cstdint>

namespace affa {
namespace widget {

struct MenuGeometry {
  // Visible rows of the sliding window. 1 is legal (a one-line display steps item by
  // item); 0 is not, and MenuModel clamps it to 1 rather than dividing by it.
  uint8_t rows = 2;

  // Usable characters per row, excluding the NUL. MenuModel writes at most rowChars + 1
  // bytes into the buffer you hand it. Clamped by the model to AFFA_MENU_ROW_MAX - 1;
  // raise that macro for a wider panel. 26 is the Carminat window row, 8 an info row.
  uint8_t rowChars = 26;

  // Does the selection wrap past the ends. False for Carminat: a wheel with detents and no
  // end stop makes "you are at the bottom" invisible.
  bool wrap = false;
};

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MENU
