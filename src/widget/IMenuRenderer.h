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
// THE ONE THING THAT IS NOT A WHOLE FRAME, and why it is here rather than in every adapter:
// the Carminat widget distinguishes a FULL redraw (one 96-byte ISO-TP screen) from a
// HIGHLIGHT-ONLY change (one 07 29 01 frame, ~14x less bus time — it is why RenderSlot::Menu
// and RenderSlot::Highlight are different slots), because moving the selection inside the
// visible window changes nothing but which row is lit. beginFrame/row/endFrame cannot say
// that, so there is a FOURTH call that can: highlightOnly(). The model raises it exactly
// when the window did not move and only the selected row changed — the same condition
// Menu::selectNext tested — and falls back to a full frame when the renderer declines.
//
// The alternative was to let every adapter cache the last frame and infer the case by
// comparing. That is an obligation on every adapter, silently paid in bus time by the ones
// that forget: nothing fails, the wheel just costs twice what it should. A cost that only
// the seam can make free for everybody belongs in the seam.
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

  // Only the lit row changed; the text and the arrows are exactly as the last frame left
  // them, and `index` is the row index — same 0..rows-1 counting as row() — that is now
  // selected. A display that can move its highlight without redrawing (Carminat: one
  // 07 29 01 frame instead of a 96-byte screen, ~14x less bus time) overrides this, moves
  // the highlight and returns true.
  //
  // THE DEFAULT MUST BE CORRECT, NOT FAST, which is why this is not pure virtual: a renderer
  // that cannot move a highlight on its own — anything that redraws the whole glass, the
  // info-row screen that has no highlight at all, a serial console — says nothing here, and
  // the model emits the full frame it would have emitted anyway. Returning false is always
  // safe; returning true without having drawn anything leaves the selection where the last
  // frame put it.
  //
  // Called ONLY between complete frames, never inside one, and never as the first thing a
  // renderer sees: a full frame always precedes it (open() renders, and every path that
  // changes text renders), so "as the last frame left them" is a state the renderer has.
  //
  // ONLY THE NEW ROW IS PASSED. A panel whose highlight is a command (Carminat: the frame
  // names the row and the panel unlights the other one) needs nothing else. A panel that has
  // to ERASE the old marker before drawing the new one — a `>` column on an LCD — remembers
  // which row it lit, which it already knows: `selected` in the last frame's row() calls said
  // so, and this call's `index` says where it goes next.
  virtual bool highlightOnly(uint8_t index) {
    (void)index;
    return false;
  }
};

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MENU
