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

// The scroll hint the model computes and the renderer draws. THE VALUES ARE THE CARMINAT
// WIRE BYTES, so CarminatMenuRenderer stays a pass-through and nothing has to translate on
// every redraw — but they are declared HERE, in the panel-independent layer, because this
// is the layer that computes them. src/widget/ must not include a panel's constants.
//
// The tie is a static_assert in carminat/CarminatMenuRenderer.cpp, which is the one place
// that may see both spellings: change a value here and that file stops compiling rather
// than quietly drawing the wrong arrow. A panel whose wire values differ translates in its
// own adapter; that is what the adapter is for.
enum Scroll : uint8_t {
  kScrollNone = 0x00,   // no arrows — the whole list fits the window
  kScrollUp   = 0x07,   // top arrow only    — the window is at the END of the list
  kScrollDown = 0x0B,   // bottom arrow only — the window is at the START
  kScrollBoth = 0x0C,   // both — the window is in the middle
};

struct IMenuRenderer {
  virtual ~IMenuRenderer() = default;

  // `header` is never null (the model substitutes ""). `scrollMask` is one of the Scroll
  // values above — docs/API.md §8.6.
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
