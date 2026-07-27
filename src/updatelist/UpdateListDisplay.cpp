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
// The extracted tickMedia() stepped a position every time it was called and happened to
// find 400 ms on the clock. Here the position is DERIVED from the clock:
//
//     window(now) = (base + (now - epoch) / kScrollStepMs) mod len
//
// `base` and `epoch` change only when the text changes or the marquee is paused/resumed,
// so the function is pure in `now` and cannot drift, cannot catch up in a burst after a
// stalled loop, and produces the same frames whether poll() runs at 5 Hz or 5 kHz. A
// render is emitted only when the window actually moves.

void UpdateListDisplay::setScrollText(const char* text) {
  char buf[AFFA_TEXT_MAX];

  // normalizeTitle strips the video annotations and trims, on top of transliterating.
  // The gap is reserved out of the buffer up front so appending it can never truncate
  // mid-sequence.
  const size_t n = normalizeTitle(text, buf, sizeof(buf) - kScrollGap);
  if (n == 0) {
    // Nothing to scroll. Deliberately transmits nothing: blanking the panel because an
    // application published an empty title would be a decision the application did not
    // make. Call setText("") if a blank screen is what you want.
    _text[0]     = '\0';
    _len         = 0;
    _base        = 0;
    _lastPos     = 0;
    _needsRedraw = false;
    return;
  }

  size_t i = n;
  for (uint8_t g = 0; g < kScrollGap && i + 1 < sizeof(buf); ++g) buf[i++] = ' ';
  buf[i] = '\0';

  // Identical content is a no-op. An application that re-publishes the current track on
  // every media update would otherwise reset the window to 0 forever and the title would
  // never scroll — this is the one line that stops that.
  if (std::strcmp(buf, _text) == 0) return;

  std::memcpy(_text, buf, i + 1);
  _len         = static_cast<uint16_t>(i);
  _base        = 0;
  _epochMs     = _clock.millis();
  _lastPos     = 0;
  _needsRedraw = true;
}

void UpdateListDisplay::setScrollActive(bool on) {
  if (on == _active) return;
  const uint32_t now = _clock.millis();
  if (on) {
    // Resume where it froze: the base is already the frozen position, so re-basing the
    // epoch is the whole of it. The pause does not advance the marquee.
    _epochMs = now;
  } else {
    _base    = windowAt(now);
    _epochMs = now;
  }
  _active      = on;
  _needsRedraw = true;   // paused draws the frozen window once, then transmits nothing
}

uint16_t UpdateListDisplay::windowAt(uint32_t now) const {
  if (_len == 0) return 0;
  const uint32_t steps = (now - _epochMs) / kScrollStepMs;
  return static_cast<uint16_t>((static_cast<uint32_t>(_base) + steps) % _len);
}

void UpdateListDisplay::renderWindow(uint16_t pos) {
  char win[kScrollWidth + 1];
  for (uint8_t i = 0; i < kScrollWidth; ++i)
    win[i] = _text[(pos + i) % _len];
  win[kScrollWidth] = '\0';
  // Virtual: the LCD variant substitutes its own encoding and inherits this marquee
  // unchanged. The Result is dropped on purpose — a scroll step that could not be queued
  // is superseded by the next one 400 ms later, and there is nothing useful to do about
  // it here. An application that wants the verdict watches onComplete().
  (void)setText(win, 255);
}

void UpdateListDisplay::onPoll() {
  UpdateListBase::onPoll();          // the AMS banner schedule runs first, always

  if (_len == 0) return;
  if (amsFeedbackPending()) return;  // the banner owns the screen for its whole window

  if (_active) {
    const uint16_t p = windowAt(_clock.millis());
    if (p == _lastPos && !_needsRedraw) return;
    renderWindow(p);
    _lastPos     = p;
    _needsRedraw = false;
    return;
  }

  if (_needsRedraw) {
    renderWindow(_base);
    _lastPos     = _base;
    _needsRedraw = false;
  }
}

void UpdateListDisplay::onRadioText(bool isAux) {
  if (!isAux || !_reassertOnAux) return;
  // The radio has drawn over us. Redraw the current window on the next poll — which is
  // the whole reaction, and notably NOT a change of what is being scrolled.
  _needsRedraw = true;
}

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
