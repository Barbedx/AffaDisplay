// The AFFA2 8-segment panel: the 8+12 "old text / new text" segment encoding on 0x121
// (docs/WIRE-SPEC.md §9.1) plus the marquee title scroll, a pure function of
// IClock::millis() driven from onPoll() rather than a call-counted tick.
//
// Artist/title composition and connection state stay OUTSIDE the library; what it owns is
// the eight-cell window and its cadence, so the seam is a string plus a play/pause bit.
#pragma once
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListBase.h"
#if AFFA_ENABLE_MARQUEE
#  include "../widget/Marquee.h"
#endif

namespace affa {

class UpdateListDisplay : public UpdateListBase {
 public:
  UpdateListDisplay(ICanLink& link, IClock& clock) : UpdateListBase(link, clock) {}

  bool supports(Feature f) const override { return familySupports(f); }

  // 0x121, segment encoding. `digit` selects the channel: 0..9 -> 0x70 + digit,
  // anything else (255, the default) -> 0x7A, "no channel". Enqueued on RenderSlot::Text,
  // so a repeated render supersedes a queued one instead of stacking behind it.
  [[nodiscard]] Result setText(const char* text, uint8_t digit = 255) override;

#if AFFA_ENABLE_MARQUEE
  // ---- marquee ------------------------------------------------------------
  // THE STATE MACHINE IS NOT HERE. It is widget::Marquee, which knows nothing about this
  // panel; what these four methods add is the eight-cell geometry, the render, and the
  // "hold off while the AMS banner owns the screen" rule. Everything below forwards.

  // The text to scroll through the eight-cell window. Transliterated and cleaned with
  // affa::normalizeTitle(), then given a blank tail so the wrap reads as a gap rather than
  // a collision. nullptr, or a string that normalises to nothing, switches the marquee off
  // and transmits nothing.
  //
  // Re-setting the SAME text is a no-op that does NOT reset the position, so an
  // application re-publishing the current track every second does not freeze the scroll on
  // character 0. That check is why this takes a const char* and copies.
  void setScrollText(const char* text);

  // Frozen draws the current window ONCE and then transmits nothing. Resuming continues
  // where it froze: the position is a base plus an elapsed-time offset, not accumulated
  // per call.
  void setScrollActive(bool on);
  bool scrollActive() const { return _marquee.active(); }

  // Draw the current window on the next poll even though nothing changed. The one
  // library-side reaction to another node overwriting our screen.
  void reassert() { _needsRedraw = true; }

  // The window this panel scrolls: 8 cells, 400 ms a step, an 8-cell gap before the wrap.
  // Exposed so a caller can see what it got rather than assume the OEM numbers.
  static widget::MarqueeGeometry geometry() {
    return widget::MarqueeGeometry{updatelist::kScrollWidth, updatelist::kScrollGap,
                                   updatelist::kScrollStepMs};
  }
#endif

#if AFFA_ENABLE_MARQUEE
  // On by default (the OEM-plausible behaviour), but it is a reaction to a RADIO, so it is
  // a replaceable default; with it off, the policy is the application's.
  void setReassertOnAux(bool on) { _reassertOnAux = on; }
  bool reassertOnAux() const { return _reassertOnAux; }
#endif

 protected:
  void onPoll() override;
  void onRadioText(bool isAux) override;

  // Copy `cells` bytes of `src` into `dst`, padding the tail with NUL — not space. Every
  // capture and every golden vector in docs/WIRE-SPEC.md shows 0x00 there; do not "fix"
  // this to spaces.
  static void copyCells(const char* src, uint8_t* dst, uint8_t cells);

 private:
#if AFFA_ENABLE_MARQUEE
  void renderWindow(uint16_t pos);

  widget::Marquee _marquee{geometry()};
  uint16_t _lastPos       = 0;      // last position actually transmitted
  bool     _needsRedraw   = false;
  bool     _reassertOnAux = true;

  // The gap is reserved out of the marquee's buffer, so at or below this bound there is no
  // room for one window of actual text and it could never move. Marquee::sane() also
  // clamps, but a panel whose OEM geometry does not fit should say so at compile time
  // rather than silently scroll something narrower than its glass.
  static_assert(AFFA_TEXT_MAX > updatelist::kScrollGap + updatelist::kScrollWidth,
                "AffaDisplay: AFFA_TEXT_MAX must exceed kScrollGap + kScrollWidth or the "
                "UpdateList marquee has nothing to scroll");
#endif
};

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
