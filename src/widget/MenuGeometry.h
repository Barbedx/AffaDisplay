// The shape of the display the menu is drawn on — injected, never assumed.
//
// This is the one thing the extracted Carminat menu got wrong: it did not have a geometry,
// it had TWO ROWS OF TWENTY-SIX CHARACTERS welded into the arithmetic (`row0`/`row1`, the
// `count <= 2` scroll rule, a fixed pair of `char[AFFA_MENU_ROW_MAX]` buffers). The
// algorithm above that — a window sliding over N items, in-place editing, Select walking
// the fields — has nothing to do with either number.
//
// So the numbers become parameters. The three panels this is meant to survive:
//
//     Carminat menu screen   rows = 2   rowChars = 26   (the 0x21 two-row window)
//     Carminat info screen   rows = 3   rowChars = 8    (showInfoPopup, three short rows)
//     a 4" OLED              rows = 6+  rowChars = 20+  (whatever the font gives you)
//
// Nothing else about the panel belongs here. Row TAGS (the Carminat 0x7E / 0x7F that mark
// which physical row a string lands on), highlight frames, arrow glyphs and charset are the
// adapter's business — see IMenuRenderer.h.
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

  // Usable characters per row, NOT counting the terminating NUL. MenuModel writes at most
  // rowChars + 1 bytes into the buffer you hand it, so the buffer must be that big.
  // Clamped to AFFA_MENU_ROW_MAX - 1 by the model: the row buffer is a fixed array and the
  // library allocates nothing after construction. Raise AFFA_MENU_ROW_MAX for a wider
  // panel; 26 is the Carminat window row, 8 an info row.
  uint8_t rowChars = 26;

  // Does the selection wrap past the ends. The Carminat widget does NOT wrap (a wheel with
  // detents and no end stop makes "you are at the bottom" invisible), which is why this
  // defaults to false and why the ported navigation keeps its two early `return`s.
  bool wrap = false;
};

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MENU
