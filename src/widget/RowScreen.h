// Three rows of live text, as content rather than as frames. A WIDGET, like MenuModel.
//
// SINGLE RESPONSIBILITY, AND HERE IS THE LINE. This class holds no link, no display and no
// task. It transmits nothing, it calls nothing, and it knows no panel primitive. Every method
// is a function of (content, geometry, now). It answers exactly one question:
//
//     what do the three rows say at time t, and is that different from what is on the glass?
//
// The APPLICATION renders. It samples the answer at whatever rate it likes and calls
// showFullscreenText() — or showMenu(), or three UpdateList rows, or an OLED — itself. That
// is the same split MenuModel/IMenuRenderer already uses: the widget owns the state, the
// application owns the transmission, and the library's transmit path never learns either.
//
// WHY IT IS WORTH HAVING AT ALL, given that it is application logic: every consumer that
// wants a live screen writes the same four things — a marquee per row, a comparison against
// what was last drawn, a "which window is showing now", and the one non-obvious rule below.
// That is reusable, so it is reusable HERE rather than copied into each application.
//
// THE NON-OBVIOUS RULE, which is most of the value: a row that SCROLLS and also CONTAINS A
// CLOCK has text that changes every second. Marquee's default policy restarts a changed
// string at character 0 — right for a track title, catastrophic here, because the row is then
// pinned to character 0 for ever and never scrolls. It flickers once a second instead. So the
// phase belongs to the ROW, not to the string (Marquee::Phase::Keep).
#pragma once
#include "../AffaConfig.h"

#if AFFA_ENABLE_MARQUEE

#include "Marquee.h"
#include <cstdint>

namespace affa {
namespace widget {

struct RowScreenGeometry {
  uint8_t width = 24;   // visible cells per row
  uint8_t gap   = 4;    // blank cells before the wrap, so it reads as a pause
};

class RowScreen {
 public:
  // Three, because that is what a three-row screen is. Not a template parameter: every
  // primitive this feeds — showFullscreenText, showMenu+header, showInfoPopup — takes three.
  static constexpr uint8_t kRows = 3;

  // One row's rendered window, NUL-terminated. Sized from Marquee's own text buffer so a
  // geometry wider than a row can hold is truncated rather than overrunning.
  static constexpr uint8_t kCell = AFFA_TEXT_MAX;

  explicit RowScreen(const RowScreenGeometry& g = RowScreenGeometry{});

  // Scroll cadence for one row, in ms per character. 0 makes the row STATIC: it shows the
  // first `width` characters and never moves, which is the right answer for a short label
  // and costs no repaints at all.
  //
  // A SETUP CALL. Changing a cadence BLANKS THE ROW — a Marquee holds the normalised,
  // gap-padded text rather than the caller's string, so there is nothing to carry over that
  // would not be a truncation. Re-publish the row afterwards; a live screen's publishing loop
  // does that on its next pass anyway.
  //
  // HARMONISE THE THREE CADENCES ONTO ONE GRID — 400/800/1200, not 250/400/700. This is the
  // one sizing decision a caller has to make and it is easy to get wrong, so: dirty() goes
  // true when ANY row steps, and on a Carminat a fullscreen repaint is FOURTEEN FRAMES, each
  // waiting on a panel ACK — about 190 ms measured, so the wire carries roughly five per
  // second. Cadences of 250/400/700 ask for ~8 steps per second and are 60% oversubscribed;
  // multiples of one base make the step instants COINCIDE, so one repaint serves all three
  // rows and the same visible motion costs 2.5/s.
  //
  // Oversubscribing is safe — the caller skips while the wire is busy and the library
  // coalesces what it cannot — but the rows then step at whatever rate the wire allows rather
  // than the rate you configured, and the scroll looks uneven. test_rowscreen measures both.
  void setScroll(uint8_t row, uint32_t stepMs);
  uint32_t scroll(uint8_t row) const;

  // Which way the row travels. Unlike setScroll() this is FREE and LOSSLESS — it does not
  // blank the row and does not move the text; see Marquee::setDirection(). A console can
  // toggle it as fast as a person can click.
  //
  // It also survives a setScroll(), which is the only reason RowScreen tracks it at all: a
  // cadence change rebuilds the underlying Marquee, and a reversed row that silently
  // un-reversed itself when you changed its speed would be a genuinely baffling console.
  void setDirection(uint8_t row, Marquee::Direction d, uint32_t now);
  Marquee::Direction direction(uint8_t row) const;

  // Publish a row's text. Returns true if the content actually changed.
  //
  // The scroll position carries across the change (see the header comment), and re-publishing
  // IDENTICAL text is a no-op that does not disturb it either — so an application may call
  // this every loop iteration without thinking about it.
  bool setText(uint8_t row, const char* text, uint32_t now);
  bool setRows(const char* r0, const char* r1, const char* r2, uint32_t now);

  // The three windows at `now`. Always writes all three, always NUL-terminated; a row with no
  // text writes an empty string.
  void render(uint32_t now, char out[kRows][kCell]) const;

  // Would render(now) differ from what painted() last latched? This is what an application
  // polls, and it is what keeps a 500 kbit/s bus mostly idle: a static row that has not been
  // re-published contributes nothing, and a scrolling row contributes one repaint per step
  // rather than one per poll.
  bool dirty(uint32_t now) const;

  // Latch what render(now) produces as "this is on the glass". Call it when the render has
  // been ACCEPTED, not when it completes: a coalesced render that is superseded before it
  // reaches the wire is still the newest value for its slot, and re-latching on completion
  // would repaint the same window a second time.
  void painted(uint32_t now);

  // Forget what is on the glass, so the next dirty() is true whatever the content is. For the
  // one case the content cannot express: something else took the screen — a resync, a popup,
  // another render — and the rows must be redrawn unchanged.
  void invalidate();

  const RowScreenGeometry& geometry() const { return _geom; }

 private:
  static uint8_t clampRow(uint8_t row) {
    return row < kRows ? row : static_cast<uint8_t>(kRows - 1);
  }

  RowScreenGeometry _geom;
  Marquee  _row[kRows];
  uint32_t _stepMs[kRows]  = {0, 0, 0};
  Marquee::Direction _dir[kRows] = {Marquee::Direction::Forward,
                                    Marquee::Direction::Forward,
                                    Marquee::Direction::Forward};
  char     _painted[kRows][kCell] = {{0}, {0}, {0}};
  bool     _valid = false;          // has painted() ever latched? false forces one repaint
};

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MARQUEE
