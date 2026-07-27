// CarminatMenuRenderer — affa::widget::MenuModel onto the Carminat two-row menu screen.
//
// This is the adapter half of the seam described in src/widget/IMenuRenderer.h, and it is the
// ONLY thing left between the panel-independent menu state machine and this panel's glass.
// The state machine itself is src/widget/MenuModel; there is no second copy of it any more
// (src/carminat/Menu/ is gone — see docs/MENU-WIDGET.md §1).
//
// Everything this file knows that the model does not:
//
//   * the window is TWO rows of TWENTY-SIX characters (geometry(), below),
//   * a redraw is one 96-byte ISO-TP screen (showMenu) plus one small frame (highlightItem),
//     and those two cost wildly different amounts of bus time,
//   * the physical rows are named by the TAGS 0x7E and 0x7F.
//
// The model speaks row INDEX 0..rows-1 and nothing else. The mapping index -> tag lives here,
// in sendHighlight(), because it is a fact about the glass.
//
// THE HIGHLIGHT-ONLY CASE is the seam's fourth call, and on this panel it is the whole reason
// the fourth call exists: moving the selection inside the visible window changes nothing but
// which row is lit, and saying so costs one 07 29 01 frame instead of a 96-byte screen —
// ~14x less bus time, which is why RenderSlot::Menu and RenderSlot::Highlight are different
// slots. The adapter does not have to detect the case, cache the last frame or compare
// anything: the model raises highlightOnly() precisely when the window did not move, and the
// override below answers "yes, this panel can do that" in four lines. Pass
// coalesceHighlight = false to DECLINE it — the model then falls back to a full frame and the
// bus cost of turning the wheel doubles, which is exactly what a renderer that stays on the
// default gets.
//
// WHERE THE Result WENT. MenuModel returns void from every render path on purpose: whether a
// frame reached the panel is not something a UI state machine can act on. The adapter is the
// layer that CAN, because it is the one holding the IPanel, so the verdict is kept here and
// read back through lastResult() — see CarminatDisplay::menuRenderer().
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

  // Takes the IPanel, not CarminatDisplay: the four rendering calls are all this needs, and
  // depending on the concrete display would make CarminatDisplay.h and this header include
  // each other.
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

  // The panel's verdict on the last redraw: the FIRST non-Ok of the screen and the highlight,
  // because a menu that drew its rows but not its highlight is still wrong. Result::Ok before
  // anything has been drawn.
  Result lastResult() const { return _lastResult; }

 private:
  // THE ROW TAGS LIVE HERE. 0x7E is the top row, 0x7F the bottom (carminat::kRowTagTop /
  // kRowTagBottom, docs/WIRE-SPEC.md §8.4). CarminatDisplay::highlightItem() takes the row
  // NUMBER and puts the tag byte on the wire itself, so this is the one place that states
  // which physical row a model row index means. Used by BOTH paths, so the mapping is stated
  // once whether the frame is a whole screen or a lone highlight.
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
