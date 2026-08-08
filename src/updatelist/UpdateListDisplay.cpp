// Entire body gated: an unselected panel must compile to an empty object file.
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListDisplay.h"
#include "../util/AffaText.h"
#include <cstring>

namespace affa {

using namespace updatelist;

// ---------------------------------------------------------------------------
// setText — ONE BUILDER, TWO HEADERS, 0x121   docs/WIRE-SPEC.md §9.1 and §9.2
// ---------------------------------------------------------------------------
//   segment  10 19 76 <chan> 01           old(8) 10 new(12) 00 81 81   29 B, 4 frames, PCI 0x23
//   LCD      10 1C 7F 55 55 FF 60 03      old(8) 10 new(12) 00         30 B, 5 frames, PCI 0x24
//
// The tail from `old(8)` onwards is IDENTICAL, which is why this is one function; everything
// before it is the variant. Both declared lengths are correct and neither is the other plus
// three: they cover different amounts of content.
//
// The segment form's two trailing 0x81 are PAYLOAD bytes the builder emits, not transport
// filler — they sit outside the declared 0x19 and are reproduced because that is what the
// panel has been accepting.

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

Submitted UpdateListDisplay::setText(const char* text, uint8_t digit) {
  // Transliteration is mandatory and it happens here, at the single choke point where
  // this family's frames are built. UTF-8 that reaches the wire is garbage on the glass,
  // not a compile error.
  char t[AFFA_TEXT_MAX];
  toAscii(text, t, sizeof(t));

  // The larger of the two, so one buffer serves both variants.
  uint8_t d[kLcdPayload];
  uint8_t n = 0;

  if (_glass == UpdateListGlass::Lcd) {
    d[n++] = kCmdSetText;                               // 0x10
    d[n++] = kLcdFfDl;                                  // 0x1C = 28 content bytes
    d[n++] = kLcdFixed;                                 // 0x7F text + icons
    d[n++] = kLcdIcons;                                 // 0x55 NO_TRAFFIC|NO_NEWS|...
    d[n++] = kLcdIconSep;                               // 0x55 literal separator
    d[n++] = kLcdIconMode;                              // 0xFF ICON_MODE_NONE
    d[n++] = kLcdChannel;                               // 0x60 — `digit` has no place here
    d[n++] = kLcdLocation;                              // 0x03 LOCATION(0,0)|SEL|FULLSCR
  } else {
    d[n++] = kCmdSetText;                               // 0x10
    d[n++] = kSegFfDl;                                  // 0x19 = 25 content bytes
    d[n++] = kSegTextType;                              // 0x76 text only
    d[n++] = (digit <= kChanMaxDigit)                   // channel
                 ? static_cast<uint8_t>(kChanBase + digit)
                 : kChanNone;
    d[n++] = kSegLocation;                              // 0x01
  }

  // IDENTICAL FROM HERE, both variants. This is the whole reason the two are one function.
  copyCells(t, &d[n], kOldCells);  n = static_cast<uint8_t>(n + kOldCells);
  d[n++] = kTextSep;                                    // 0x10
  copyCells(t, &d[n], kNewCells);  n = static_cast<uint8_t>(n + kNewCells);
  d[n++] = kTextTerm;                                   // 0x00

  if (_glass != UpdateListGlass::Lcd) {
    d[n++] = kFiller;                                   // 0x81, outside the declared len
    d[n++] = kFiller;                                   // 0x81
  }

  static_assert(5 + kOldCells + 1 + kNewCells + 3 == kSegPayload,
                "segment setText payload is 29 bytes (WIRE-SPEC §9.1)");
  static_assert(8 + kOldCells + 1 + kNewCells + 1 == kLcdPayload,
                "LCD setText payload is 30 bytes (WIRE-SPEC §9.2)");
  static_assert(kLcdPayload >= kSegPayload, "the buffer must hold the larger variant");
  return enqueueRender(kIdSetText, d, n, RenderSlot::Text);
}

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
