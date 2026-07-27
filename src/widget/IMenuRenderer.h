// The seam between the panel-independent menu state machine and a panel. MenuModel knows
// items, fields, a selection, a window and an edit cursor; an adapter implementing this
// interface knows frames, row tags and arrows.
//
// Per redraw the model calls one beginFrame, then row() with index 0..rows-1 in order,
// then one endFrame — always the whole window, never a partial one.
#pragma once
#include "../AffaConfig.h"

#if AFFA_ENABLE_MENU

#include <cstdint>

namespace affa {
namespace widget {

struct IMenuRenderer {
  virtual ~IMenuRenderer() = default;

  // `header` is never null (the model substitutes ""). `scrollMask` carries the Carminat
  // wire values verbatim (docs/API.md §8.6) so that adapter is a pass-through:
  //     0x00 no arrows   0x0B bottom only   0x07 top only   0x0C both
  virtual void beginFrame(const char* header, uint8_t scrollMask) = 0;

  // `index` is a ROW index, 0..rows-1 from the top of the window — not an item index and
  // not a panel row tag. `text` is NUL-terminated, at most geometry.rowChars characters,
  // already transliterated, and EMPTY for a row the item list does not reach. The pointer
  // is a borrowed scratch buffer reused for the next row: copy it, do not keep it.
  // Exactly one row has `selected` whenever the menu is open.
  virtual void row(uint8_t index, const char* text, bool selected) = 0;

  virtual void endFrame() = 0;

  // Only the lit row changed; text and arrows are as the last frame left them, and `index`
  // is the row index now selected. A display that can move its highlight without redrawing
  // (Carminat: one 07 29 01 frame instead of a 96-byte screen, ~14x less bus time — hence
  // the separate RenderSlot::Highlight) overrides this and returns true.
  //
  // Not pure virtual because the default must be correct, not fast: declining is always
  // safe and the model emits the full frame instead. Returning true without drawing leaves
  // the selection where the last frame put it.
  //
  // Called only between complete frames and never first — a full frame always precedes it,
  // so "as the last frame left them" is a state the renderer has. Only the new row is
  // passed; an adapter that must erase an old marker knows which row it lit from the last
  // frame's `selected`.
  virtual bool highlightOnly(uint8_t index) {
    (void)index;
    return false;
  }
};

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MENU
