// A scrolling text window, display-independent.
//
// This was welded into UpdateListDisplay and hard-coded to that panel's eight cells. There
// is nothing about a marquee that is AFFA2, or Renault, or eight characters wide: it is a
// string, a window, and a cadence. The geometry is injected, exactly as MenuGeometry is for
// MenuModel, and the panel is left with the one thing that IS panel-specific — putting the
// window on the glass.
//
// THE POSITION IS DERIVED FROM THE CLOCK, NEVER ACCUMULATED. The extracted tickMedia()
// stepped a counter every time it was called and happened to find 400 ms on the clock.
// Here:
//
//     window(now) = (base + (now - epoch) / stepMs) mod len
//
// `base` and `epoch` move only when the text changes or the marquee is paused or resumed,
// so the function is pure in `now`. It cannot drift, cannot catch up in a burst after a
// stalled loop, and produces the same sequence whether poll() runs at 5 Hz or 5 kHz.
//
// This class TRANSMITS NOTHING and holds no reference to a panel. It answers "which window
// is showing at time t"; the caller renders it.
#pragma once
#include "../AffaConfig.h"

#if AFFA_ENABLE_MARQUEE

#include <cstdint>

namespace affa {
namespace widget {

// Sanitised once at construction, like MenuGeometry: a zero width or a zero step would
// divide by zero or scroll nothing, and a geometry with no room for one full window plus
// its gap could never move.
struct MarqueeGeometry {
  uint8_t  width  = 8;    // visible cells
  uint8_t  gap    = 8;    // blank cells appended before the wrap, so it reads as a gap
  uint32_t stepMs = 400;  // ms per one-cell step
};

class Marquee {
 public:
  explicit Marquee(const MarqueeGeometry& g);

  // The text to scroll. Transliterated and cleaned with affa::normalizeTitle(), then given
  // `gap` blank cells so the wrap reads as a pause rather than a collision. nullptr, or a
  // string that normalises to nothing, switches the marquee off.
  //
  // RE-SETTING THE SAME TEXT IS A NO-OP that does not reset the position. An application
  // re-publishing the current track on every media update would otherwise pin the window
  // to character 0 for ever, and the title would never scroll. Returns true if the content
  // actually changed.
  bool setText(const char* text, uint32_t now);

  // Frozen keeps the current window; resuming continues from it rather than from wherever
  // a free-running clock would have reached. The pause does not advance the marquee.
  void setActive(bool on, uint32_t now);
  bool active() const { return _active; }

  // Window position at `now`, in characters from the start of the text. 0 when empty.
  uint16_t windowAt(uint32_t now) const;

  // The frozen position — what a paused marquee shows.
  uint16_t base() const { return _base; }

  // Copy the `width` characters starting at `pos` into `out`, NUL-terminated. `outSize`
  // includes the NUL; a buffer smaller than width+1 is truncated rather than overrun.
  void window(uint16_t pos, char* out, uint8_t outSize) const;

  // 0 disables the marquee entirely — there is nothing to scroll and nothing to draw.
  uint16_t length() const { return _len; }

  const MarqueeGeometry& geometry() const { return _geom; }

 private:
  MarqueeGeometry _geom;
  char     _text[AFFA_TEXT_MAX] = {0};   // the string INCLUDING its blank tail
  uint16_t _len     = 0;                 // strlen(_text)
  uint16_t _base    = 0;                 // window position in effect at _epochMs
  uint32_t _epochMs = 0;
  bool     _active  = false;
};

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MARQUEE
