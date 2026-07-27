// InfoMenuRenderer — the SAME MenuModel on the Carminat info-row screen.
//
// Three rows of eight characters, drawn by showInfoPopup() (docs/WIRE-SPEC.md §8.10 — three
// separate messages, one per row, deliberately not coalesced against each other). This is
// the case the owner named, and it is the interesting one because almost nothing it needs
// looks like the menu screen:
//
//   * rows = 3, not 2 — the window is one item taller and the arithmetic does not care;
//   * rowChars = 8, not 26 — every label is truncated by the model, on a character boundary
//     it computed from the geometry it was handed;
//   * there is NO highlight frame and NO arrow glyph on this screen, so this adapter ignores
//     both `selected` and `scrollMask`, and there is no header slot either.
//
// An adapter is allowed to ignore what its panel cannot draw. That is the whole reason the
// model hands over an index and a mask instead of calling highlightItem() itself.
//
// WHAT IGNORING `selected` COSTS, honestly: on this screen the user cannot see which of the
// three rows the wheel is on. The one-line fix is to declare rowChars = 7 instead of 8 and
// spend the eighth cell on a marker — `snprintf(cell, 9, "%c%s", sel ? '>' : ' ', text)` in
// row(). It is left out here because the brief for this example is a renderer that has no
// selection tag at all, and because which of the eight columns you are willing to sell is a
// layout decision that belongs to whoever owns the screen.
#pragma once

#include <AffaDisplay.h>
#include <widget/MenuModel.h>

#if !AFFA_PANEL_CARMINAT || !AFFA_ENABLE_MENU
#  error "InfoMenuRenderer needs -D AFFA_PANEL_CARMINAT=1 -D AFFA_ENABLE_MENU=1"
#endif

class InfoMenuRenderer final : public affa::widget::IMenuRenderer {
 public:
  // carminat::kInfoCells is 8 — the same number, taken from the panel's own constants rather
  // than typed in again.
  static constexpr uint8_t kRows  = 3;
  static constexpr uint8_t kChars = affa::carminat::kInfoCells;
  static constexpr affa::widget::MenuGeometry geometry() { return {kRows, kChars, false}; }

  explicit InfoMenuRenderer(affa::CarminatDisplay& panel) : _panel(panel) {}

  // ---- IMenuRenderer -------------------------------------------------------
  void beginFrame(const char* header, uint8_t scrollMask) override {
    // No header slot and no arrows on this screen. Both arguments are genuinely dropped;
    // they are not stashed "just in case", because a value nothing reads is a lie about the
    // panel's capabilities.
    (void)header;
    (void)scrollMask;
  }

  void row(uint8_t index, const char* text, bool selected) override {
    (void)selected;                      // no highlight exists on this screen — see above
    if (index >= kRows) return;
    copy(_row[index], text);
  }

  void endFrame() override {
    // Three rows, one call, whatever the list length was: rows past the end of the list
    // arrive as empty strings and blank their cells, which is what the panel expects.
    (void)_panel.showInfoPopup(_row[0], _row[1], _row[2]);
  }

  const char* rowText(uint8_t i) const { return (i < kRows) ? _row[i] : ""; }

 private:
  template <size_t N>
  static void copy(char (&dst)[N], const char* src) {
    size_t i = 0;
    if (src) { for (; i + 1 < N && src[i] != '\0'; ++i) dst[i] = src[i]; }
    dst[i] = '\0';
  }

  affa::CarminatDisplay& _panel;
  char _row[kRows][kChars + 1] = {{0}};
};
