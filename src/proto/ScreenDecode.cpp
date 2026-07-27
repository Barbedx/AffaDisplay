// Entire translation unit gated — see AffaConfig.h.
#include "ScreenDecode.h"

#if AFFA_ENABLE_ISOTP_RX

namespace affa {
namespace screen {
namespace {

// The widest field this file copies is row1 at 30 bytes; 64 leaves headroom for a longer
// 0x74 popup text without ever touching the heap. Fixed, on the stack, per call.
constexpr uint8_t kScratch = 64;

}  // namespace

void asciiz(const uint8_t* payload, uint8_t len, uint8_t a, uint8_t b,
            char* dst, uint8_t dstSize) {
  if (!dst || dstSize == 0) return;
  dst[0] = '\0';
  if (!payload) return;

  char tmp[kScratch];
  uint8_t n = 0;
  // uint16_t and not uint8_t: `i <= b` with b == 255 would never terminate. No caller
  // passes 255 today; the loop is written so that none ever can.
  for (uint16_t i = a; i <= b && i < len && n < kScratch - 1; ++i) {
    const uint8_t c = payload[i];
    if (c == 0) break;                       // NUL ends the field
    if (c >= 0x20 && c < 0x7F) tmp[n++] = static_cast<char>(c);
  }
  tmp[n] = '\0';

  uint8_t start = 0;
  while (start < n && tmp[start] == ' ') ++start;
  int16_t end = static_cast<int16_t>(n) - 1;
  while (end >= static_cast<int16_t>(start) && tmp[end] == ' ') --end;

  uint8_t out = 0;
  for (int16_t i = start; i <= end && out < dstSize - 1; ++i)
    dst[out++] = tmp[i];
  dst[out] = '\0';
}

void menu(const uint8_t* payload, uint8_t len, ScreenModel& m) {
  // kMenuHwMinLen (92), not kMenuMinLen (96): a real panel stops at the declared FF_DL and
  // never receives the last four bytes. See the header for the full arithmetic.
  if (!payload || len < kMenuHwMinLen) return;

  m.mode   = ScreenModel::Mode::Menu;
  m.scroll = payload[kOffScroll];
  m.sel    = -1;                             // a fresh screen clears the highlight
  asciiz(payload, len, kOffHeader, kEndHeader, m.header, sizeof(m.header));
  m.row0Id = payload[kOffRow0Mark];
  asciiz(payload, len, kOffRow0, kEndRow0, m.row0, sizeof(m.row0));
  m.row1Id = payload[kOffRow1Mark];
  asciiz(payload, len, kOffRow1, kEndRow1, m.row1, sizeof(m.row1));
}

bool windowText(const uint8_t* payload, uint8_t len, ScreenModel& m) {
  if (!payload || len < kWinTextMinLen) return false;
  if (payload[0] != 0x10) return false;
  if (payload[2] != kWinTextCmdFull && payload[2] != kWinTextCmdWindow) return false;

  m.mode = ScreenModel::Mode::Menu;
  // b is the last byte of the payload, not a fixed field end: the 0x74 popup is
  // `10 <6+n> 74 …` with a caller-chosen n, so the text runs to the end of what was
  // transmitted. asciiz stops at the first NUL anyway.
  asciiz(payload, len, kOffWinText, static_cast<uint8_t>(len - 1),
         m.header, sizeof(m.header));
  return true;
}

bool infoRow(const uint8_t* payload, uint8_t len, ScreenModel& m) {
  if (!payload || len < kInfoMinLen) return false;
  if (payload[0] != 0x10 || payload[1] != kInfoDeclLen || payload[2] != kInfoCmd)
    return false;

  uint8_t slot;
  switch (payload[kOffInfoSlot]) {
    case kInfoSlot0: slot = 0; break;
    case kInfoSlot1: slot = 1; break;
    case kInfoSlot2: slot = 2; break;
    default:
      // An unrecognised slot code is delivered in arrival order rather than dropped —
      // the same principle as Key::Unknown: the list of slot codes is attested, not
      // proven complete.
      slot = (m.infoCount < 3) ? m.infoCount : 2;
      break;
  }

  m.mode = ScreenModel::Mode::Info;
  asciiz(payload, len, kOffInfoText, static_cast<uint8_t>(kOffInfoText + 7),
         m.info[slot], sizeof(m.info[slot]));
  if (slot + 1 > m.infoCount) m.infoCount = static_cast<uint8_t>(slot + 1);
  return true;
}

void segText(const uint8_t* payload, uint8_t len, ScreenModel& m) {
  if (!payload || len < kSegMinLen) return;
  if (!(payload[0] == 0x10 && payload[1] == kSegDecl && payload[2] == kSegCmd)) return;

  m.mode = ScreenModel::Mode::Menu;
  asciiz(payload, len, kSegNew, static_cast<uint8_t>(kSegNew + 11),
         m.header, sizeof(m.header));
  asciiz(payload, len, kSegOld, static_cast<uint8_t>(kSegOld + 7),
         m.row0, sizeof(m.row0));
}

bool segTextIcons(const uint8_t* payload, uint8_t len, ScreenModel& m,
                  uint8_t* icons, uint8_t* iconMode) {
  if (!payload || len < kSegIconMinLen) return false;
  if (!(payload[0] == 0x10 && payload[1] == kSegIconDecl && payload[2] == kSegIconCmd))
    return false;

  m.mode = ScreenModel::Mode::Menu;
  asciiz(payload, len, kSegIconNew, static_cast<uint8_t>(kSegIconNew + 11),
         m.header, sizeof(m.header));
  asciiz(payload, len, kSegIconOld, static_cast<uint8_t>(kSegIconOld + 7),
         m.row0, sizeof(m.row0));

  if (icons)    *icons    = payload[kOffSegIcons];
  if (iconMode) *iconMode = payload[kOffSegIconMode];
  return true;
}

bool frame(const Frame& f, ScreenModel& m) {
  // The full three-byte guard, not just data[0]: 0x151 carries other single frames
  // (03 52 … display control, 05 56 … clock, 02 54 03 … dismiss) and a looser test would
  // manufacture a highlight out of one of them. Same lesson as the 03 89 key guard.
  if (f.len >= 4 && f.data[0] == kHighlightSf && f.data[1] == kHighlightCmd &&
      f.data[2] == kHighlightArg) {
    m.sel = f.data[3];
    return true;
  }
  return false;
}

}  // namespace screen
}  // namespace affa

#endif  // AFFA_ENABLE_ISOTP_RX
