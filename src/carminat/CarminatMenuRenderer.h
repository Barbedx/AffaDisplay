// widget::MenuModel onto the Carminat two-row menu screen — the adapter half of the seam
// in src/widget/IMenuRenderer.h, and the only thing between the state machine and this
// panel's glass.
//
// What this file knows and the model does not: the window is two rows of 26 characters; a
// redraw is one 96-byte ISO-TP screen plus one small highlight frame, costing wildly
// different bus time; a geometry taller than this panel is dropped rather than written past
// the array. The model speaks row INDEX 0..rows-1, which on this panel IS the row number
// highlightItem() takes — the tag bytes 0x7E/0x7F appear nowhere here.
//
// coalesceHighlight = false DECLINES highlightOnly(), and the model then falls back to a
// full frame, doubling the bus cost of turning the wheel.
//
// MenuModel returns void from every render path: whether a frame reached the panel is not
// something a UI state machine can act on. The adapter holds the IPanel, so the verdict
// lives here and is read back through lastResult().
#pragma once
#include "../AffaConfig.h"

#if AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU

#include "../core/AffaTypes.h"
#include "../core/IPanel.h"
#include "../widget/IMenuRenderer.h"
#include "../widget/MenuGeometry.h"
#include <cstdint>

namespace affa {

class CarminatMenuRenderer final : public widget::IMenuRenderer {
 public:
  // The shape of this panel's menu screen. 26 is the usable width of a window row
  // (docs/WIRE-SPEC.md §8.5 — item2 is limited to 26 even though the builder accepts 30),
  // and the wheel has no end stop, so the selection does not wrap.
  static constexpr uint8_t kRows  = 2;
  static constexpr uint8_t kChars = 26;
  static constexpr widget::MenuGeometry geometry() { return {kRows, kChars, false}; }

  // Takes the IPanel, not CarminatDisplay: the four rendering calls are all this needs,
  // and the concrete display would make the two headers include each other.
  explicit CarminatMenuRenderer(IPanel& panel, bool coalesceHighlight = true)
      : _panel(panel), _coalesce(coalesceHighlight) {}

  CarminatMenuRenderer(const CarminatMenuRenderer&)            = delete;
  CarminatMenuRenderer& operator=(const CarminatMenuRenderer&) = delete;

  // ---- IMenuRenderer -------------------------------------------------------
  void beginFrame(const char* header, uint8_t scrollMask) override;
  void row(uint8_t index, const char* text, bool selected) override;
  void endFrame() override;
  bool highlightOnly(uint8_t index) override;

  // ---- what the last frame said, for tracing, for the console and for tests ----
  const char* header() const { return _header; }
  const char* rowText(uint8_t i) const { return (i < kRows) ? _row[i] : ""; }
  uint8_t     scroll() const { return _scroll; }
  uint8_t     selectedRow() const { return _selected; }

  // True when the last thing that went out was a highlight alone — the wheel moved inside
  // the window.
  bool lastWasHighlightOnly() const { return _lastWasHighlightOnly; }

  // The panel's verdict on the last redraw: the FIRST non-Ok of the screen and the
  // highlight, because a menu that drew its rows but not its highlight is still wrong.
  Result lastResult() const { return _lastResult; }

 private:
  // Move the highlight to a model ROW INDEX. Used by BOTH paths, so a whole-screen redraw
  // and a lone highlight cannot disagree about which row is lit.
  Result sendHighlight(uint8_t index);

  IPanel& _panel;
  bool    _coalesce;

  char    _header[AFFA_MENU_ROW_MAX]     = {0};
  char    _row[kRows][AFFA_MENU_ROW_MAX] = {{0}};
  uint8_t _scroll               = 0;
  uint8_t _selected             = 0;
  bool    _lastWasHighlightOnly = false;
  Result  _lastResult           = Result::Ok;
};

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
