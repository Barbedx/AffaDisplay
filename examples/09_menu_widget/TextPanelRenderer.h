// TextPanelRenderer — the same MenuModel on a display the library has never heard of.
//
// A stand-in for the 4" OLED: six rows of twenty characters, drawn as a box on the serial
// console. It includes <Arduino.h> and src/widget/, and NOTHING else — no CarminatDisplay,
// no AffaDisplay.h, no CAN, no ISO-TP, no wire constant. That is the point of the exercise:
// if the menu still works here, the algorithm was never about the panel.
//
// It is also the third of the three possible things an adapter can do with the scroll mask.
// The Carminat adapter passes it through (the values ARE its wire bytes); the info-row
// adapter ignores it (that screen has no arrows); this one TRANSLATES it into whatever
// glyphs the display has. The mask is data, not a command.
#pragma once

#include <Arduino.h>
#include <widget/MenuModel.h>

#if !AFFA_ENABLE_MENU
#  error "TextPanelRenderer needs -D AFFA_ENABLE_MENU=1"
#endif

class TextPanelRenderer final : public affa::widget::IMenuRenderer {
 public:
  // Six rows of twenty. Nothing here is derived from any panel the library supports — the
  // numbers come from the font and the glass, which is where they should come from. Wrap is
  // ON, because a display that shows the whole list at once has a visible end and wrapping
  // off the bottom is not disorienting the way it is on a two-row window.
  static constexpr uint8_t kRows  = 6;
  static constexpr uint8_t kChars = 20;
  static constexpr affa::widget::MenuGeometry geometry() { return {kRows, kChars, true}; }

  // ---- IMenuRenderer -------------------------------------------------------
  void beginFrame(const char* header, uint8_t scrollMask) override {
    copy(_header, header);
    _scroll = scrollMask;
  }

  void row(uint8_t index, const char* text, bool selected) override {
    if (index >= kRows) return;
    copy(_row[index], text);
    _sel[index] = selected;
  }

  // highlightOnly() is deliberately NOT overridden either, for the opposite reason to the
  // info-row screen: this display CAN show a selection (the '>' column) but cannot address
  // one row of an already-printed box — a console line, once printed, is gone. Staying on the
  // default reprints the box, which is correct and is what a full-redraw display should do.
  // Between the three adapters in this example both sides of the seam's fourth call are
  // exercised: one overrides it, two do not.

  void endFrame() override {
    // The mask, translated into this display's glyphs. An OLED would draw two triangles;
    // a serial console draws two characters.
    const char up   = (_scroll == 0x07 || _scroll == 0x0C) ? '^' : ' ';
    const char down = (_scroll == 0x0B || _scroll == 0x0C) ? 'v' : ' ';

    char bar[kChars + 6];
    bar[0] = '+';
    for (uint8_t i = 1; i < kChars + 4; ++i) bar[i] = '-';
    bar[kChars + 4] = '+';
    bar[kChars + 5] = '\0';

    Serial.println(bar);
    Serial.printf("| %-*s %c%c |\n", kChars - 1, _header, up, down);
    Serial.println(bar);
    for (uint8_t r = 0; r < kRows; ++r)
      Serial.printf("| %c %-*s |\n", _sel[r] ? '>' : ' ', kChars, _row[r]);
    Serial.println(bar);
  }

 private:
  template <size_t N>
  static void copy(char (&dst)[N], const char* src) {
    size_t i = 0;
    if (src) { for (; i + 1 < N && src[i] != '\0'; ++i) dst[i] = src[i]; }
    dst[i] = '\0';
  }

  char    _header[kChars + 1]        = {0};
  char    _row[kRows][kChars + 1]    = {{0}};
  bool    _sel[kRows]                = {false};
  uint8_t _scroll                    = 0;
};
