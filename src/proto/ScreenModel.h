// The decoded state of a panel screen: the SEMANTIC ORACLE.
//
// A test asserts `twin.screen().header` is "CLOCK", not that eleven bytes matched. The
// difference matters because a byte comparison passes for the wrong reason (two bugs
// cancelling) and fails for no reason (a harmless padding change), whereas this fails
// exactly when the driver would have put the wrong thing on the glass.
//
// Header-only aggregate, no gate: it costs nothing unless something instantiates it, and
// core/AffaTypes.h forward-declares `struct ScreenModel;` for Event::screen.model, so it
// MUST stay in namespace affa and MUST stay a struct.
#pragma once
#include <cstdint>

namespace affa {

struct ScreenModel {
  enum class Mode : uint8_t {
    None,   // nothing decoded yet
    Menu,   // a screen payload was decoded (menu / now-playing / notification / text)
    Info,   // the 3-row info-settings popup (0x76 rows)
  };

  Mode mode = Mode::None;

  // The three text fields of the Carminat windowed menu (command 0x21 mode 0x01). Sizes
  // are field width + 1 for the NUL. Offsets are quoted in ScreenDecode.h, in PAYLOAD
  // origin, converted once — see docs/PROTOCOL-NOTES.md §1.2 for why three origins are in
  // circulation and why this file states which one it uses.
  char    header[27] = {0};   // payload [11..36], 26 bytes
  char    row0[26]   = {0};   // payload [39..63], 25 bytes
  char    row1[31]   = {0};   // payload [66..95], 30 bytes
  uint8_t row0Id     = 0;     // row-0 tag at payload [38], 0x7E
  uint8_t row1Id     = 0;     // row-1 tag at payload [65], 0x7F
  int16_t sel        = -1;    // highlighted row TAG (0x7E / 0x7F), -1 = no highlight.
                              // It is the tag and not the index, because that is what
                              // `07 29 01 <row>` carries and what the panel latches.
  uint8_t scroll     = 0;     // scroll-arrow byte at payload [10]: 0x00 none, 0x07 up,
                              // 0x0B down, 0x03 both (0x0C was never on the wire)

  // The info-settings popup: three 8-character slots. Rows arrive as THREE SEPARATE
  // messages (docs/WIRE-SPEC.md §8.10), so a decoder sees them one at a time and
  // infoCount grows; it is not a single transfer.
  char    info[3][9] = {{0}};
  uint8_t infoCount  = 0;

  void clear() { *this = ScreenModel(); }
};

}  // namespace affa
