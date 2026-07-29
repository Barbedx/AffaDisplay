// Entire body gated, so a build without the marquee compiles this to an empty object.
#include "RowScreen.h"

#if AFFA_ENABLE_MARQUEE

#include <cstring>

namespace affa {
namespace widget {
namespace {

// A static row is still a Marquee — it just never steps. Giving it a real stepMs rather than
// 0 keeps Marquee's own sanitiser out of it (0 would be clamped to 1 and the row would race)
// and means `_stepMs[i] == 0` is the ONE authority for "static", checked in exactly one
// place: render() below.
constexpr uint32_t kStaticStepMs = 1000;

MarqueeGeometry geomFor(const RowScreenGeometry& g, uint32_t stepMs) {
  MarqueeGeometry m;
  m.width  = g.width;
  m.gap    = g.gap;
  m.stepMs = stepMs ? stepMs : kStaticStepMs;
  return m;
}

}  // namespace

RowScreen::RowScreen(const RowScreenGeometry& g)
    : _geom(g),
      _row{Marquee{geomFor(g, 0)}, Marquee{geomFor(g, 0)}, Marquee{geomFor(g, 0)}} {}

void RowScreen::setScroll(uint8_t row, uint32_t stepMs) {
  row = clampRow(row);
  if (_stepMs[row] == stepMs) return;

  // MarqueeGeometry is sanitised at construction and not mutable afterwards — deliberately,
  // so a zero width can never appear halfway through a run. Rebuilding the row is therefore
  // the honest way to change its cadence, and it blanks the row; see the header.
  _stepMs[row] = stepMs;
  _row[row]    = Marquee{geomFor(_geom, stepMs)};
  _valid       = false;           // the row's content is gone; force a repaint
}

uint32_t RowScreen::scroll(uint8_t row) const { return _stepMs[clampRow(row)]; }

bool RowScreen::setText(uint8_t row, const char* text, uint32_t now) {
  // Phase::Keep, and this single argument is the reason this class exists. See the header.
  return _row[clampRow(row)].setText(text, now, Marquee::Phase::Keep);
}

bool RowScreen::setRows(const char* r0, const char* r1, const char* r2, uint32_t now) {
  // Three separate calls, all of them made: `||` would short-circuit and leave rows 1 and 2
  // un-published the moment row 0 changed — which on a screen where every row carries a
  // clock is every single time.
  const bool a = setText(0, r0, now);
  const bool b = setText(1, r1, now);
  const bool c = setText(2, r2, now);
  return a || b || c;
}

void RowScreen::render(uint32_t now, char out[kRows][kCell]) const {
  for (uint8_t i = 0; i < kRows; ++i) {
    if (_row[i].length() == 0) { out[i][0] = '\0'; continue; }
    // A static row is frozen at its base rather than at literal 0, so a row switched from
    // scrolling to static stays where the caller last saw it.
    const uint16_t pos = _stepMs[i] ? _row[i].windowAt(now) : _row[i].base();
    _row[i].window(pos, out[i], kCell);
  }
}

bool RowScreen::dirty(uint32_t now) const {
  if (!_valid) return true;
  char now3[kRows][kCell];
  render(now, now3);
  for (uint8_t i = 0; i < kRows; ++i)
    if (std::strcmp(now3[i], _painted[i]) != 0) return true;
  return false;
}

void RowScreen::painted(uint32_t now) {
  char now3[kRows][kCell];
  render(now, now3);
  for (uint8_t i = 0; i < kRows; ++i) std::memcpy(_painted[i], now3[i], kCell);
  _valid = true;
}

void RowScreen::invalidate() { _valid = false; }

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MARQUEE
