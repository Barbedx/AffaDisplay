#include "CarminatMenuRenderer.h"

// The ENTIRE body is inside the gate, so a build without the menu — or without this panel —
// compiles this translation unit to an empty object file. See AffaConfig.h.
#if AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU

#include <cstddef>

namespace affa {
namespace {

// Bounded copy into a fixed row buffer. The model hands out a borrowed scratch pointer that
// it reuses for the next row, so the text has to be taken now.
template <size_t N>
void copyRow(char (&dst)[N], const char* src) {
  size_t i = 0;
  if (src) {
    for (; i + 1 < N && src[i] != '\0'; ++i) dst[i] = src[i];
  }
  dst[i] = '\0';
}

}  // namespace

void CarminatMenuRenderer::beginFrame(const char* header, uint8_t scrollMask) {
  copyRow(_header, header);
  // The mask is a PASS-THROUGH: 0x00/0x0B/0x07/0x0C are already this panel's wire values
  // (carminat::kScrollNone/Down/Up/Both), which is exactly why the model keeps them raw.
  _scroll = scrollMask;
}

void CarminatMenuRenderer::row(uint8_t index, const char* text, bool selected) {
  // A geometry taller than the glass is a construction-site error the model cannot catch: it
  // happily runs a 6-row window, and this panel has two row tags. Drop the extra rows rather
  // than write past the array.
  if (index >= kRows) return;
  copyRow(_row[index], text);
  if (selected) _selected = index;
}

// A full redraw: the 96-byte screen, then the highlight that says which of its two rows is
// lit. Both, always — showMenu does not carry the selection.
void CarminatMenuRenderer::endFrame() {
  const Result screen = _panel.showMenu(_header, _row[0], _row[1], _scroll);
  const Result hilite = sendHighlight(_selected);
  _lastResult          = (screen != Result::Ok) ? screen : hilite;
  _lastWasHighlightOnly = false;
}

// The cheap path. The screen already says the right words; only the lit row is wrong.
bool CarminatMenuRenderer::highlightOnly(uint8_t index) {
  if (!_coalesce) return false;      // the demo knob: watch the bus cost double
  if (index >= kRows) return false;  // a taller geometry than this glass has — redraw it
  _selected             = index;
  _lastResult           = sendHighlight(index);
  _lastWasHighlightOnly = true;
  return true;
}

Result CarminatMenuRenderer::sendHighlight(uint8_t index) {
  // THE MAPPING IS THE IDENTITY, and saying so is the whole function. A model row index
  // (0 = top of the window) is already what CarminatDisplay::highlightItem() takes, and
  // that builder is the ONE place that turns a row number into the tag byte
  // (carminat::kRowTagTop 0x7E / kRowTagBottom 0x7F, docs/WIRE-SPEC.md §8.4).
  //
  // This used to go index -> tag -> index through a local table, which stated the tag
  // ordering a second time and then threw the tag away. It looked like a drift guard and
  // was not one: swapping the two constants in CarminatConstants.h changed nothing here.
  // One statement of the ordering, in the builder that puts it on the wire.
  return _panel.highlightItem(index < kRows ? index : 0);
}

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
