// Entire body gated: an unselected panel must compile to an empty object file.
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListDisplay.h"
#include "../util/AffaText.h"
#include <cstring>

namespace affa {

using namespace updatelist;

// ---------------------------------------------------------------------------
// setText — 8-segment encoding, 0x121   docs/WIRE-SPEC.md §9.1
// ---------------------------------------------------------------------------
//   10 19 76 <chan> 01 old(8) 10 new(12) 00 81 81
// Four frames, exactly full, last PCI 0x23. The two trailing 0x81 are PAYLOAD bytes the
// builder emits, not transport filler; they sit outside the declared content length 0x19
// and are reproduced because that is what the panel has been accepting.

void UpdateListDisplay::copyCells(const char* src, uint8_t* dst, uint8_t cells) {
  uint8_t i = 0;
  if (src) {
    while (i < cells && src[i] != '\0') {
      dst[i] = static_cast<uint8_t>(src[i]);
      ++i;
    }
  }
  while (i < cells) dst[i++] = 0x00;   // NUL, not space — see the header
}

Result UpdateListDisplay::setText(const char* text, uint8_t digit) {
  // Transliteration is mandatory and it happens here, at the single choke point where
  // this family's frames are built. UTF-8 that reaches the wire is garbage on the glass,
  // not a compile error.
  char t[AFFA_TEXT_MAX];
  toAscii(text, t, sizeof(t));

  uint8_t d[kSegPayload];
  uint8_t n = 0;
  d[n++] = kCmdSetText;                                 // 0x10
  d[n++] = kSegFfDl;                                    // 0x19 = 25 content bytes
  d[n++] = kSegTextType;                                // 0x76
  d[n++] = (digit <= kChanMaxDigit)                     // channel
               ? static_cast<uint8_t>(kChanBase + digit)
               : kChanNone;
  d[n++] = kSegLocation;                                // 0x01
  copyCells(t, &d[n], kOldCells);  n = static_cast<uint8_t>(n + kOldCells);
  d[n++] = kTextSep;                                    // 0x10
  copyCells(t, &d[n], kNewCells);  n = static_cast<uint8_t>(n + kNewCells);
  d[n++] = kTextTerm;                                   // 0x00
  d[n++] = kFiller;                                     // 0x81, outside the declared len
  d[n++] = kFiller;                                     // 0x81

  static_assert(5 + kOldCells + 1 + kNewCells + 3 == kSegPayload,
                "segment setText payload is 29 bytes (WIRE-SPEC §9.1)");
  return enqueueRender(kIdSetText, d, n, RenderSlot::Text);
}

// ---------------------------------------------------------------------------
// Marquee
// ---------------------------------------------------------------------------
// The scrolling itself is widget::Marquee and is not panel code. What is left here is the
// three things that ARE this panel's: when to render, what to render it with, and the rule
// that the AMS banner outranks the scroll.

#if AFFA_ENABLE_MARQUEE

void UpdateListDisplay::setScrollText(const char* text) {
  if (!_marquee.setText(text, _clock.millis())) return;   // same text: keep scrolling
  _lastPos     = 0;
  _needsRedraw = _marquee.length() != 0;
}

void UpdateListDisplay::setScrollActive(bool on) {
  if (on == _marquee.active()) return;
  _marquee.setActive(on, _clock.millis());
  _needsRedraw = true;   // paused draws the frozen window once, then transmits nothing
}

void UpdateListDisplay::renderWindow(uint16_t pos) {
  char win[updatelist::kScrollWidth + 1];
  _marquee.window(pos, win, sizeof(win));
  // Virtual: the LCD variant substitutes its own encoding and inherits this marquee
  // unchanged. The Result is dropped on purpose — a scroll step that could not be queued
  // is superseded by the next one 400 ms later, and there is nothing useful to do about
  // it here. An application that wants the verdict watches onComplete().
  (void)setText(win, 255);
}

#endif  // AFFA_ENABLE_MARQUEE

void UpdateListDisplay::onPoll() {
  UpdateListBase::onPoll();          // the AMS banner schedule runs first, always

#if AFFA_ENABLE_MARQUEE
  if (_marquee.length() == 0) return;
  if (amsFeedbackPending()) return;  // the banner owns the screen for its whole window

  // A render is emitted only when the window actually MOVES, which is what keeps a 5 kHz
  // poll() from producing 5 000 identical screens a second.
  const uint16_t p = _marquee.active() ? _marquee.windowAt(_clock.millis())
                                       : _marquee.base();
  if (!_needsRedraw && (!_marquee.active() || p == _lastPos)) return;
  renderWindow(p);
  _lastPos     = p;
  _needsRedraw = false;
#endif
}

void UpdateListDisplay::onRadioText(bool isAux) {
#if AFFA_ENABLE_MARQUEE
  if (!isAux || !_reassertOnAux) return;
  // The radio has drawn over us. Redraw the current window on the next poll — which is
  // the whole reaction, and notably NOT a change of what is being scrolled.
  _needsRedraw = true;
#else
  (void)isAux;
#endif
}

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
