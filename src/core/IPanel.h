#pragma once
#include "AffaTypes.h"

namespace affa {

// The minimal RENDERING port: "how to draw", nothing else.
//
// Menu and any future page draws through this, not through the concrete display, so the
// menu widget is unit-testable against a fake panel and a future WebPanel (render to a
// browser, no CAN) satisfies the same four calls. No defaults: a rendering caller passes
// every argument explicitly.
struct IPanel {
  virtual ~IPanel() = default;

  virtual Result showMenu(const char* header, const char* row0, const char* row1,
                          uint8_t scrollIndicator) = 0;
  virtual Result setText(const char* text, uint8_t digit) = 0;
  virtual Result highlightItem(uint8_t row) = 0;
  virtual Result showPopupText(const char* text, uint8_t icon,
                               uint8_t srcIcon, uint8_t fmt) = 0;
};

} // namespace affa
