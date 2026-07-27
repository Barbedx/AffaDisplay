// Entire body gated on the LCD variant alone: a build that selected only the 8-segment
// panel must not pay a byte for this file.
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST_MENU

#include "UpdateListMenuDisplay.h"
#include "../util/AffaText.h"

namespace affa {

using namespace updatelist;

// ---------------------------------------------------------------------------
// setText — LCD encoding, 0x121   docs/WIRE-SPEC.md §9.2
// ---------------------------------------------------------------------------
//   10 1C 7F 55 55 FF 60 03 old(8) 10 new(12) 00
// Five frames; the tail carries the single terminator byte plus six 0x81 of transport
// filler. Last PCI 0x24.
//
// The `0x1C` is the declared content length, 28 bytes, covering data[2..29] — and it is
// correct, unlike the Carminat length bytes. It is NOT "0x19 + 3"; the two encodings
// declare different lengths because they carry different amounts of content.
//
// Both text fields are NUL-padded, exactly as the extracted builder's strncpy left them.
// The `{' ', ' ', ...}` initialiser in that builder is overwritten by strncpy for every
// input shorter than the field, so the spaces only ever survived for a full-width string.
// Every golden vector shows 0x00. Do not change it to spaces.

Result UpdateListMenuDisplay::setText(const char* text, uint8_t /*digit*/) {
  char t[AFFA_TEXT_MAX];
  toAscii(text, t, sizeof(t));

  uint8_t d[kLcdPayload];
  uint8_t n = 0;
  d[n++] = kCmdSetText;                                 // 0x10 set text
  d[n++] = kLcdFfDl;                                    // 0x1C = 28 content bytes
  d[n++] = kLcdFixed;                                   // 0x7F
  d[n++] = kLcdIcons;                                   // 0x55 NO_TRAFFIC|NO_NEWS|...
  d[n++] = kLcdIconSep;                                 // 0x55 literal separator
  d[n++] = kLcdIconMode;                                // 0xFF ICON_MODE_NONE
  d[n++] = kLcdChannel;                                 // 0x60 channel 0
  d[n++] = kLcdLocation;                                // 0x03 LOCATION(0,0)|SEL|FULLSCR
  copyCells(t, &d[n], kOldCells);  n = static_cast<uint8_t>(n + kOldCells);
  d[n++] = kTextSep;                                    // 0x10
  copyCells(t, &d[n], kNewCells);  n = static_cast<uint8_t>(n + kNewCells);
  d[n++] = kTextTerm;                                   // 0x00

  static_assert(8 + kOldCells + 1 + kNewCells + 1 == kLcdPayload,
                "LCD setText payload is 30 bytes (WIRE-SPEC §9.2)");
  return enqueueRender(kIdSetText, d, n, RenderSlot::Text);
}

}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST_MENU
