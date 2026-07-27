#include "CarminatMenuRenderer.h"

// The ENTIRE body is inside the gate, so a build without the menu — or without this panel —
// compiles this translation unit to an empty object file. See AffaConfig.h.
#if AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU

#include "CarminatConstants.h"
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
  static constexpr uint8_t kRowTag[kRows] = {carminat::kRowTagTop, carminat::kRowTagBottom};
  const uint8_t tag = kRowTag[index < kRows ? index : 0];
  // highlightItem() takes the row NUMBER and writes the tag byte itself; the table above is
  // what makes the index -> physical row mapping explicit at this end of the seam.
  return _panel.highlightItem(tag == carminat::kRowTagBottom ? 1 : 0);
}

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
