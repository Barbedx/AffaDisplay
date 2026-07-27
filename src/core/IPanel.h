#pragma once
#include "AffaTypes.h"

namespace affa {

// The minimal RENDERING port: "how to draw", nothing else.
//
// An IPage, and the menu ADAPTER (carminat/CarminatMenuRenderer), draw through this rather
// than through the concrete display, so both are unit-testable against a fake panel and a
// future WebPanel (render to a browser, no CAN) satisfies the same four calls.
//
// The menu STATE MACHINE does not appear in that list any more: widget::MenuModel draws
// through widget::IMenuRenderer and has no idea this interface exists — CarminatMenuRenderer
// is the only thing between the two. No defaults: a rendering caller passes every argument
// explicitly.
struct IPanel {
  virtual ~IPanel() = default;

  [[nodiscard]] virtual Result showMenu(const char* header, const char* row0,
                                        const char* row1, uint8_t scrollIndicator) = 0;
  [[nodiscard]] virtual Result setText(const char* text, uint8_t digit) = 0;
  [[nodiscard]] virtual Result highlightItem(uint8_t row) = 0;
  [[nodiscard]] virtual Result showPopupText(const char* text, uint8_t icon,
                                             uint8_t srcIcon, uint8_t fmt) = 0;
};

} // namespace affa
