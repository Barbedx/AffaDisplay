// CarminatMenuRenderer — MenuModel onto the Carminat two-row menu screen.
//
// This is the adapter half of the seam described in src/widget/IMenuRenderer.h. Everything
// this file knows that the model does not:
//
//   * the window is TWO rows of TWENTY-SIX characters (geometry(), below),
//   * a redraw is one 96-byte ISO-TP screen (showMenu) plus one small frame (highlightItem),
//     and those two cost wildly different amounts of bus time,
//   * the physical rows are named by the TAGS 0x7E and 0x7F.
//
// The model speaks row INDEX 0..rows-1 and nothing else. The mapping index -> tag lives
// here, in kRowTag, because it is a fact about the glass.
//
// THE HIGHLIGHT-ONLY CASE. IMenuRenderer cannot say "only the lit row changed" — one redraw
// is always beginFrame/row/row/endFrame. That distinction is worth ~14x the bus time on this
// panel (it is why RenderSlot::Menu and RenderSlot::Highlight are different slots), so the
// adapter recovers it the way the seam's docstring prescribes: keep the last header, rows and
// scroll mask that were actually sent, and when a frame arrives with identical text and an
// identical mask but a different `selected` row, emit the highlight alone and skip the
// screen. Turning the wheel inside the visible window therefore costs exactly what it cost
// before the model was extracted. Pass coalesceHighlight = false to see the difference.
#pragma once

#include <AffaDisplay.h>
#include <widget/MenuModel.h>
#include <cstring>

#if !AFFA_PANEL_CARMINAT || !AFFA_ENABLE_MENU
#  error "CarminatMenuRenderer needs -D AFFA_PANEL_CARMINAT=1 -D AFFA_ENABLE_MENU=1"
#endif

class CarminatMenuRenderer final : public affa::widget::IMenuRenderer {
 public:
  // The shape of this panel's menu screen. 26 is the usable width of a window row
  // (docs/WIRE-SPEC.md §8.5 — item2 is limited to 26 even though the builder accepts 30),
  // and the wheel has no end stop, so the selection does not wrap.
  static constexpr uint8_t kRows  = 2;
  static constexpr uint8_t kChars = 26;
  static constexpr affa::widget::MenuGeometry geometry() { return {kRows, kChars, false}; }

  explicit CarminatMenuRenderer(affa::CarminatDisplay& panel, bool coalesceHighlight = true)
      : _panel(panel), _coalesce(coalesceHighlight) {}

  // ---- IMenuRenderer -------------------------------------------------------
  void beginFrame(const char* header, uint8_t scrollMask) override {
    copy(_header, header);
    // The mask is a PASS-THROUGH: 0x00/0x0B/0x07/0x0C are already this panel's wire values
    // (carminat::kScrollNone/Down/Up/Both), which is exactly why the model keeps them raw.
    _scroll = scrollMask;
  }

  void row(uint8_t index, const char* text, bool selected) override {
    // A geometry taller than the glass is a construction-site error the model cannot catch:
    // it happily runs a 6-row window, and this panel has two row tags. Drop the extra rows
    // rather than write past the array.
    if (index >= kRows) return;
    copy(_row[index], text);
    if (selected) _selected = index;
  }

  void endFrame() override {
    const bool textUnchanged = _sent && _scroll == _sentScroll &&
                               std::strcmp(_header, _sentHeader) == 0 &&
                               std::strcmp(_row[0], _sentRow[0]) == 0 &&
                               std::strcmp(_row[1], _sentRow[1]) == 0;

    _highlightOnly = _coalesce && textUnchanged;
    if (!_highlightOnly) {
      (void)_panel.showMenu(_header, _row[0], _row[1], _scroll);
      copy(_sentHeader, _header);
      copy(_sentRow[0], _row[0]);
      copy(_sentRow[1], _row[1]);
      _sentScroll = _scroll;
      _sent       = true;
    }

    // THE ROW TAGS LIVE HERE. 0x7E is the top row, 0x7F the bottom (carminat::kRowTagTop /
    // kRowTagBottom, docs/WIRE-SPEC.md §8.4). CarminatDisplay::highlightItem() takes the row
    // NUMBER and puts the tag byte on the wire itself, so this table is the one place that
    // states which physical row a model row index means — an adapter for a panel that wants
    // the raw byte changes these two lines and nothing else.
    static constexpr uint8_t kRowTag[kRows] = {affa::carminat::kRowTagTop,
                                               affa::carminat::kRowTagBottom};
    const uint8_t tag = kRowTag[_selected < kRows ? _selected : 0];
    (void)_panel.highlightItem(tag == affa::carminat::kRowTagBottom ? 1 : 0);
  }

  // ---- what the last frame said, for tracing and for tests -----------------
  const char* header() const { return _header; }
  const char* rowText(uint8_t i) const { return (i < kRows) ? _row[i] : ""; }
  uint8_t     scroll() const { return _scroll; }
  uint8_t     selectedRow() const { return _selected; }
  // True when the last endFrame() sent a highlight only — the wheel moved inside the window.
  bool        lastWasHighlightOnly() const { return _highlightOnly; }

 private:
  template <size_t N>
  static void copy(char (&dst)[N], const char* src) {
    size_t i = 0;
    if (src) { for (; i + 1 < N && src[i] != '\0'; ++i) dst[i] = src[i]; }
    dst[i] = '\0';
  }

  affa::CarminatDisplay& _panel;
  bool _coalesce;

  char    _header[AFFA_MENU_ROW_MAX]      = {0};
  char    _row[kRows][AFFA_MENU_ROW_MAX]  = {{0}};
  uint8_t _scroll   = 0;
  uint8_t _selected = 0;

  char    _sentHeader[AFFA_MENU_ROW_MAX]     = {0};
  char    _sentRow[kRows][AFFA_MENU_ROW_MAX] = {{0}};
  uint8_t _sentScroll   = 0;
  bool    _sent         = false;
  bool    _highlightOnly = false;
};
