// Entire body gated: an unselected panel must compile to an empty object file.
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "UpdateListDisplay.h"
#include "../util/AffaText.h"
#include <cstring>

namespace affa {

using namespace updatelist;

// ---------------------------------------------------------------------------
// setText — ONE ENCODING, 0x121   docs/WIRE-SPEC.md §9.1
// ---------------------------------------------------------------------------
//   10 19 76 <chan> 01 old(8) 10 new(12) 00 81 81      29 B, 4 frames, PCI 0x23
//
// Sent to every glass in the family. The header does not describe the panel; see the class
// comment for why the second "LCD" encoding was a misreading and is gone.
//
// The two trailing 0x81 are PAYLOAD bytes the builder emits, not transport filler — they sit
// outside the declared 0x19 and are reproduced because that is what the panel accepts.

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

  uint8_t d[kSegPayload];
  uint8_t n = 0;

  d[n++] = kCmdSetText;                                 // 0x10
  d[n++] = kSegFfDl;                                    // 0x19 = 25 content bytes
  d[n++] = kSegTextType;                                // 0x76 text only
  d[n++] = (digit <= kChanMaxDigit)                     // channel
               ? static_cast<uint8_t>(kChanBase + digit)
               : kChanNone;
  d[n++] = kSegLocation;                                // 0x01

  // BOTH fields go out in full. The segment glass renders the first eight cells of `new`
  // and a wider one renders all twelve, which is exactly why there is nothing to branch on.
  copyCells(t, &d[n], kOldCells);  n = static_cast<uint8_t>(n + kOldCells);
  d[n++] = kTextSep;                                    // 0x10
  copyCells(t, &d[n], kNewCells);  n = static_cast<uint8_t>(n + kNewCells);
  d[n++] = kTextTerm;                                   // 0x00
  d[n++] = kFiller;                                     // 0x81, outside the declared len
  d[n++] = kFiller;                                     // 0x81

  static_assert(5 + kOldCells + 1 + kNewCells + 3 == kSegPayload,
                "setText payload is 29 bytes (WIRE-SPEC §9.1)");
  return enqueueRender(kIdSetText, d, n, RenderSlot::Text);
}

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
