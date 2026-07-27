// The seam between the panel-independent menu state machine and a panel.
//
// MenuModel knows items, fields, a selection, a window and an edit cursor. It does not know
// what a frame is, what a row tag is, whether the display has arrows, or whether drawing
// costs one message or fourteen. Everything on that side of the line lives in an adapter
// that implements this interface: a CarminatMenuRenderer that turns a frame into
// showMenu(header, row0, row1, scroll) + highlightItem(0x7E|0x7F), an InfoRowMenuRenderer
// that turns the same frame into showInfoPopup(a, b, c), an OledMenuRenderer that blits.
//
// The model calls exactly one beginFrame, then `rows` calls to row() with index 0..rows-1
// in order, then one endFrame — per redraw, always the whole window, never a partial one.
//
// WHAT THIS SEAM DELIBERATELY DOES NOT CARRY, and what an adapter does about it: the
// Carminat widget distinguishes a FULL redraw (one 96-byte ISO-TP screen, ~14x the bus time)
// from a HIGHLIGHT-ONLY change (one frame), because moving the selection inside the visible
// window changes nothing but which row is lit. A three-call frame protocol cannot express
// "only the lit row changed" without inventing a second entry point, so the model always
// emits a whole frame and an adapter that cares keeps the last header/rows/scrollMask it
// sent and compares: identical text + identical scrollMask + a different `selected` row is
// precisely the highlight-only case. That decision is now the adapter's, which is where the
// knowledge of what a message costs actually lives.
#pragma once
#include "../AffaConfig.h"

#if AFFA_ENABLE_MENU

#include <cstdint>

namespace affa {
namespace widget {

struct IMenuRenderer {
  virtual ~IMenuRenderer() = default;

  // Called in this order, once per redraw. `header` is never null (the model substitutes
  // "" for a null header). `scrollMask` is derived by the model from the window position:
  //
  //     0x00  no arrows  — the whole list fits in the window
  //     0x0B  bottom arrow only  — the window is at the top of the list
  //     0x07  top arrow only     — the window is at the bottom of the list
  //     0x0C  both
  //
  // Those are the Carminat wire values (docs/API.md §8.6) kept verbatim so the adapter for
  // that panel is a pass-through; an adapter with no arrows ignores the argument, and one
  // with different glyphs switches on it.
  virtual void beginFrame(const char* header, uint8_t scrollMask) = 0;

  // One visible row. `index` is a ROW INDEX, 0..rows-1, counted from the top of the window
  // — NOT an item index and NOT a panel row tag. `text` is NUL-terminated, at most
  // geometry.rowChars characters, already transliterated to the 7-bit charset, and EMPTY
  // for a row the item list does not reach (the bottom row of a one-item menu on a two-row
  // panel is a legitimate call). The pointer is a borrowed scratch buffer that is reused
  // for the next row: copy it, do not keep it.
  //
  // `selected` marks the row carrying the selection. Exactly one row has it whenever the
  // menu is open.
  virtual void row(uint8_t index, const char* text, bool selected) = 0;

  virtual void endFrame() = 0;
};

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MENU
