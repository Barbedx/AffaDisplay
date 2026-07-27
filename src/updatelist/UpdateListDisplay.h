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

namespace affa {

class UpdateListDisplay : public UpdateListBase {
 public:
  UpdateListDisplay(ICanLink& link, IClock& clock) : UpdateListBase(link, clock) {}

  bool supports(Feature f) const override { return familySupports(f); }

  // 0x121, segment encoding. `digit` selects the channel: 0..9 -> 0x70 + digit,
  // anything else (255, the default) -> 0x7A, "no channel". Enqueued on RenderSlot::Text,
  // so a repeated render supersedes a queued one instead of stacking behind it.
  [[nodiscard]] Result setText(const char* text, uint8_t digit = 255) override;

  // ---- marquee ------------------------------------------------------------
  // The text to scroll through the eight-cell window. Transliterated and cleaned with
  // affa::normalizeTitle(), then given a kScrollGap blank tail so the wrap reads as a gap
  // rather than a collision. nullptr, or a string that normalises to nothing, switches the
  // marquee off and transmits nothing.
  //
  // Re-setting the SAME text is a no-op that does NOT reset the position, so an
  // application re-publishing the current track every second does not freeze the scroll on
  // character 0. That check is why this takes a const char* and copies.
  void setScrollText(const char* text);

  // Frozen draws the current window ONCE and then transmits nothing. Resuming continues
  // where it froze: the position is a base plus an elapsed-time offset, not accumulated
  // per call.
  void setScrollActive(bool on);
  bool scrollActive() const { return _active; }

  // Draw the current window on the next poll even though nothing changed. The one
  // library-side reaction to another node overwriting our screen.
  void reassert() { _needsRedraw = true; }

  // On by default (the OEM-plausible behaviour), but it is a reaction to a RADIO, so it is
  // a replaceable default; with it off, the policy is the application's.
  void setReassertOnAux(bool on) { _reassertOnAux = on; }
  bool reassertOnAux() const { return _reassertOnAux; }

 protected:
  void onPoll() override;
  void onRadioText(bool isAux) override;

  // Copy `cells` bytes of `src` into `dst`, padding the tail with NUL — not space. Every
  // capture and every golden vector in docs/WIRE-SPEC.md shows 0x00 there; do not "fix"
  // this to spaces.
  static void copyCells(const char* src, uint8_t* dst, uint8_t cells);

 private:
  void   renderWindow(uint16_t pos);
  uint16_t windowAt(uint32_t now) const;

  // The scrolled string INCLUDING its blank tail. Fixed buffer, no String, no heap.
  char     _text[AFFA_TEXT_MAX] = {0};
  uint16_t _len          = 0;      // strlen(_text); 0 disables the marquee entirely
  uint16_t _base         = 0;      // window position in effect at _epochMs
  uint32_t _epochMs      = 0;      // when _base was in effect
  uint16_t _lastPos      = 0;      // last position actually transmitted
  bool     _active       = false;
  bool     _needsRedraw  = false;
  bool     _reassertOnAux = true;

  // The gap is reserved out of this buffer, so at or below this bound there is no room for
  // one window of actual text and the marquee could never move.
  static_assert(AFFA_TEXT_MAX > updatelist::kScrollGap + updatelist::kScrollWidth,
                "AffaDisplay: AFFA_TEXT_MAX must exceed kScrollGap + kScrollWidth or the "
                "UpdateList marquee has nothing to scroll");
};

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
